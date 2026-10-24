// Tile-layer upload skip: a tile layer's cell content is uploaded to its GPU cache slot only when it
// changes. By default the renderer hashes the packed cells each frame (an FNV-1a-64 fold over the packed
// 32-bit words) and skips the transfer/upload when the hash and dimensions match the slot's last upload.
// A layer may instead declare its changes with TileContent::contentChanged: the renderer then trusts the
// flag and never packs or hashes — `true` re-uploads, `false` skips, and declaring `false` over changed
// cells renders the stale map (the declared contract). These tests pin the hash's padding-insensitivity
// and determinism (device-free), and the skip/re-upload decisions plus their byte-for-pixel correctness
// (device-backed).
//
// Device-backed cases are compose-only + windowless (a GPU device, no display): the same harness the
// golden-readback / cache-rekey / upload-stats tests use, so they run on a software rasterizer in CI —
// lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every
// production-representative platform; the one skip is Windows on ARM (a courtesy coverage runner with no
// production-representative GPU backend in CI).

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;  // 8 tiles × 8 px — an exact-fit viewport
constexpr int kH = 64;

// The renderer's content fold, reproduced here to validate the property the renderer relies on: the hash
// consumes packed cell words, so struct padding never enters it. Kept in sync with the fold in renderer.cpp.
constexpr std::uint64_t kFnv64Offset = 14695981039346656037ull;
constexpr std::uint64_t kFnv64Prime  = 1099511628211ull;

std::uint64_t foldCells(std::span<const TileCell> cells, std::size_t count) {
    std::uint64_t h = kFnv64Offset;
    for (std::size_t k = 0; k < count; ++k) {
        const PackedTileCell pc = (k < cells.size()) ? packTileCell(cells[k]) : PackedTileCell{};
        h ^= pc.w0;
        h *= kFnv64Prime;
        h ^= pc.w1;
        h *= kFnv64Prime;
    }
    return h;
}

// ── Device-free: the fold's contract ─────────────────────────────────────────────────────────────────

// The fold is a pure function of the cells: the same cells fold to the same value every time.
TEST(TileUploadSkipHash, FoldIsDeterministic) {
    const std::array<TileCell, 4> cells{{{.atlas = AtlasId{1}, .tile = 5, .palette = PaletteId{2}},
                                         {.atlas = AtlasId{1}, .tile = 6, .palette = PaletteId{2}},
                                         {.atlas = AtlasId{3}, .tile = 0, .palette = PaletteId{1}, .flipX = true},
                                         {.atlas = AtlasId{1}, .tile = 5, .palette = PaletteId{2}}}};
    EXPECT_EQ(foldCells(cells, cells.size()), foldCells(cells, cells.size()));
}

// Changing any cell's content changes the fold — the fold is not degenerate (e.g. a constant), so it can
// actually detect a mutation.
TEST(TileUploadSkipHash, FoldDistinguishesContent) {
    std::array<TileCell, 4> a{{{.atlas = AtlasId{1}, .tile = 5, .palette = PaletteId{2}},
                               {.atlas = AtlasId{1}, .tile = 6, .palette = PaletteId{2}},
                               {.atlas = AtlasId{1}, .tile = 7, .palette = PaletteId{2}},
                               {.atlas = AtlasId{1}, .tile = 8, .palette = PaletteId{2}}}};
    std::array<TileCell, 4> b = a;
    b[2].tile = 99;  // one cell differs
    EXPECT_NE(foldCells(a, a.size()), foldCells(b, b.size()));
}

// Two cells with identical field values but deliberately different raw padding bytes pack to the same
// words and therefore fold to the same hash — padding does not enter, so it can never cause a spurious
// re-upload or a false skip.
TEST(TileUploadSkipHash, PaddingDoesNotEnterHash) {
    // Zero-initialized: padding bytes are 0.
    TileCell clean{};
    clean.atlas    = AtlasId{4};
    clean.tile     = 21;
    clean.palette  = PaletteId{3};
    clean.flipX    = true;
    clean.flipY    = false;
    clean.rotation = Rotation::Rot90;

    // Raw bytes pre-filled to 0xFF, then the SAME field values assigned — the inter-field padding stays
    // 0xFF while every field matches `clean`.
    TileCell dirty;
    std::memset(&dirty, 0xFF, sizeof(dirty));
    dirty.atlas    = AtlasId{4};
    dirty.tile     = 21;
    dirty.palette  = PaletteId{3};
    dirty.flipX    = true;
    dirty.flipY    = false;
    dirty.rotation = Rotation::Rot90;

    EXPECT_EQ(packTileCell(clean), packTileCell(dirty));  // the property the fold rests on

    const std::array<TileCell, 1> ca{{clean}};
    const std::array<TileCell, 1> da{{dirty}};
    EXPECT_EQ(foldCells(ca, 1), foldCells(da, 1));  // ...so the folds match despite the padding
}

// ── Device-backed: the skip/re-upload decisions and their output correctness ──────────────────────────

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

// A 16×16 indexed atlas (a 2×2 tile grid, indices 0..3) + a 4-color palette. The first atlas / first
// palette on any renderer take the same handle values and identical pixels, so two renderers built this
// way produce byte-identical output for the same frame.
BaseArt uploadBaseArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16).atlasId;
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(pal));
    return {atlas, palette};
}

// Backing a tile layer's cell span points into — must outlive the render/capture call that reads it.
struct TileBacking {
    std::vector<TileCell> cells;
};

// A w×h tile layer keyed `key`, every cell drawing tile 1, optionally carrying a declared contentChanged.
DrawLayer makeTileLayer(std::string_view key, int w, int h, const BaseArt& art,
                        std::optional<bool> changed, TileBacking& b) {
    b.cells.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                   TileCell{.atlas = art.atlas, .tile = 1, .palette = art.palette});
    DrawLayer layer{.key = ObjectKey(key)};
    layer.z       = 0;
    layer.size    = PixelSize{kW, kH};
    layer.content = TileContent{.widthInTiles = w, .heightInTiles = h,
                                .cells = std::span<const TileCell>(b.cells), .contentChanged = changed};
    return layer;
}

class TileUploadSkipTest : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; its production path (D3D12 + DXIL) is covered by the Windows "
                                "x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This device-backed test requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); install a GPU driver (a "
                      "software rasterizer such as lavapipe suffices) and, on a headless runner, set "
                      "SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }
};

// Re-submitting a layer with unchanged cells uploads once, then skips: the hash matches, so no second
// transfer is issued. Driven via captureViewport (composes unconditionally, so the copy pass runs every call)
// to observe the per-upload skip in isolation; renderFrame would frame-skip the second identical settled
// submission whole (the frame-level compose skip, pinned in compose_skip_test), never reaching the copy pass.
TEST_F(TileUploadSkipTest, ResubmitSkipsUpload) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    TileBacking b;
    FrameDrawState frame;
    frame.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/std::nullopt, b)};

    (void)r.captureViewport(frame);
    const Renderer::RenderStats s1 = r.renderStats();
    EXPECT_EQ(s1.tilemapUploads, 1u);
    EXPECT_EQ(s1.tilemapSkips, 0u);

    (void)r.captureViewport(frame);  // identical content
    const Renderer::RenderStats s2 = r.renderStats();
    EXPECT_EQ(s2.tilemapUploads, 1u);  // no new upload
    EXPECT_EQ(s2.tilemapSkips, 1u);    // skipped exactly once
}

// Changing one cell breaks the hash match, so the layer re-uploads rather than skipping.
TEST_F(TileUploadSkipTest, SingleCellMutationReuploads) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    TileBacking b;
    FrameDrawState frame;
    frame.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/std::nullopt, b)};

    r.renderFrame(frame);   // upload
    b.cells[0].tile = 2;    // one cell changes (base tile is 1)
    r.renderFrame(frame);

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.tilemapUploads, 2u);  // content changed → re-upload
    EXPECT_EQ(s.tilemapSkips, 0u);
}

// A dimensions change on the same key re-uploads (the texture is recreated and the skip signature reset),
// never skips against the old-size slot.
TEST_F(TileUploadSkipTest, DimsChangeReuploads) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    TileBacking b8, b16;
    FrameDrawState f8;
    f8.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/std::nullopt, b8)};
    FrameDrawState f16;
    f16.layers = {makeTileLayer("bg", 8, 16, art, /*changed=*/std::nullopt, b16)};

    r.renderFrame(f8);   // upload at 8×8
    r.renderFrame(f16);  // same key, new dims → re-upload

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.tilemapUploads, 2u);
    EXPECT_EQ(s.tilemapSkips, 0u);
}

// Declaring contentChanged=false makes the renderer trust the flag, not the cells: after the first
// submission establishes the slot, mutating cells while still declaring `false` skips the upload (the
// stale map renders — the declared contract).
TEST_F(TileUploadSkipTest, DeclaredUnchangedSkipsMutation) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    TileBacking b;
    FrameDrawState frame;
    frame.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/false, b)};

    // Driven via captureViewport (always composes) so the per-upload skip is observed in isolation — see
    // ResubmitSkipsUpload. renderFrame would frame-skip the second identical settled submission whole.
    (void)r.captureViewport(frame);   // first submission: no prior upload → uploads the baseline
    b.cells[0].tile = 2;              // mutate WITHOUT declaring the change (still false)
    (void)r.captureViewport(frame);   // declared unchanged → skip

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.tilemapUploads, 1u);
    EXPECT_EQ(s.tilemapSkips, 1u);
}

// Declaring contentChanged=true re-uploads even when the cells are byte-identical to the last upload.
TEST_F(TileUploadSkipTest, DeclaredChangedReuploads) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    TileBacking b;
    FrameDrawState f0;
    f0.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/false, b)};
    r.renderFrame(f0);  // baseline upload

    FrameDrawState f1;
    DrawLayer forced = f0.layers[0];  // same cells span, declared changed
    std::get<TileContent>(forced.content).contentChanged = true;
    f1.layers = {forced};
    r.renderFrame(f1);  // declared changed → re-upload despite identical cells

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.tilemapUploads, 2u);
    EXPECT_EQ(s.tilemapSkips, 0u);
}

// A skipped frame renders byte-identically to a fresh renderer that uploads the same frame — skipping the
// upload does not corrupt the output.
TEST_F(TileUploadSkipTest, SkippedFrameMatchesFreshRenderer) {
    Renderer primed{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    primed.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(primed);
    TileBacking b;
    FrameDrawState frame;
    frame.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/std::nullopt, b)};

    primed.renderFrame(frame);                                              // upload
    const std::vector<Rgba8> skipped = primed.captureViewport(frame);      // skip path
    EXPECT_GT(primed.renderStats().tilemapSkips, 0u);                       // it really skipped

    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    fresh.automaticInterpolation(false);
    const BaseArt art2 = uploadBaseArt(fresh);
    TileBacking b2;
    FrameDrawState ref;
    ref.layers = {makeTileLayer("bg", 8, 8, art2, /*changed=*/std::nullopt, b2)};
    const std::vector<Rgba8> uploaded = fresh.captureViewport(ref);

    EXPECT_EQ(skipped, uploaded);
}

// After a mutation re-uploads, the output matches a fresh renderer drawing the mutated frame — the
// re-upload carries the new content, not the stale texture. (Under an always-skip fault this diverges.)
TEST_F(TileUploadSkipTest, MutationReuploadMatchesFreshRenderer) {
    Renderer primed{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    primed.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(primed);
    TileBacking b;
    FrameDrawState frame;
    frame.layers = {makeTileLayer("bg", 8, 8, art, /*changed=*/std::nullopt, b)};

    primed.renderFrame(frame);         // upload original
    b.cells[0].tile = 2;               // change content
    b.cells[1].tile = 3;
    const std::vector<Rgba8> after = primed.captureViewport(frame);  // must carry the new content

    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    fresh.automaticInterpolation(false);
    const BaseArt art2 = uploadBaseArt(fresh);
    TileBacking b2;
    FrameDrawState ref;
    ref.layers = {makeTileLayer("bg", 8, 8, art2, /*changed=*/std::nullopt, b2)};
    b2.cells[0].tile = 2;
    b2.cells[1].tile = 3;
    const std::vector<Rgba8> refOut = fresh.captureViewport(ref);

    EXPECT_EQ(after, refOut);
}

}  // namespace
