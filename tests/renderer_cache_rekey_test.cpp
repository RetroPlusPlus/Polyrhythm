// Per-layer GPU cache identity: the renderer keys its tilemap-texture / sprite-buffer caches by the
// layer's ObjectKey (DrawLayer::key), not its position in frame.layers. These tests pin the three
// consequences: a slot follows its key across a reorder, a slot is evicted (and correctly recreated) when
// its key stops appearing, and two layers that collide on one key never share a slot (the transient
// bypass). Every frame uploads its layers unconditionally, so these tests assert only that each cache
// slot's identity is correct — a captured frame equals the same frame drawn by a fresh renderer.
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the golden-readback
// and upload-stats tests use, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP
// (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every production-representative platform;
// the one skip is Windows on ARM (a courtesy coverage runner with no production-representative GPU backend).

#include <array>
#include <cstdint>
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

constexpr int kW = 64;  // 8 tiles × 8 px — an exact-fit viewport, deterministic sampling
constexpr int kH = 64;

// Windows on ARM is a courtesy coverage runner: in a VM it has no production-representative GPU backend, and
// its real production path (D3D12 + DXIL) is covered by the Windows x64 job — so a missing device HERE is a
// documented out-of-scope skip. Everywhere else a missing device is a hard failure (mirrors the golden harness).
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

// A 16×16 indexed atlas (a 2×2 tile grid, indices 0..3) + a 4-color palette. Uploaded per renderer; the
// first atlas / first palette on any renderer take the same handle values and identical pixels, so two
// renderers built this way produce byte-identical output for the same frame.
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

// Backing a tile layer's cell span points into — must outlive the capture call that reads it.
struct TileBacking {
    std::vector<TileCell> cells;
};

// An 8×8 tile layer filling the viewport: every cell draws `tile` through `palette`, at depth `z` with
// `alpha`. Distinct (tile, z, alpha) triples give visibly distinct, order-sensitive layers.
DrawLayer makeTileLayer(std::string_view key, std::int32_t z, const BaseArt& art,
                        std::uint16_t tile, float alpha, TileBacking& b) {
    b.cells.assign(8 * 8, TileCell{.atlas = art.atlas, .tile = tile, .palette = art.palette});
    DrawLayer layer{.key = ObjectKey(key)};
    layer.z       = z;
    layer.alpha   = alpha;
    layer.size    = PixelSize{kW, kH};
    layer.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                .cells = std::span<const TileCell>(b.cells)};
    return layer;
}

class CacheRekeyTest : public ::testing::Test {
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

// Priming a renderer with layers [X, Y] then capturing the reordered [Y, X] (same per-key content) must
// equal a fresh renderer's capture of [Y, X]: each layer's slot follows its KEY, so the reorder does not
// cross-contaminate. (Position-keying would corrupt this once upload-skip lands — this pins the identity now.)
TEST_F(CacheRekeyTest, LayerSlotFollowsKeyAcrossReorder) {
    Renderer primed{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(primed);

    TileBacking bx, by;
    {
        FrameDrawState a;                       // order [X, Y]
        a.layers = {makeTileLayer("X", 0, art, 1, 1.0f, bx),
                    makeTileLayer("Y", 10, art, 3, 0.5f, by)};
        (void)primed.captureViewport(a);        // populate the caches keyed X, Y
    }
    FrameDrawState b;                            // reordered [Y, X], same per-key content
    b.layers = {makeTileLayer("Y", 10, art, 3, 0.5f, by),
                makeTileLayer("X", 0, art, 1, 1.0f, bx)};
    const std::vector<Rgba8> primedOut = primed.captureViewport(b);

    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art2 = uploadBaseArt(fresh);
    TileBacking bx2, by2;
    FrameDrawState ref;
    ref.layers = {makeTileLayer("Y", 10, art2, 3, 0.5f, by2),
                  makeTileLayer("X", 0, art2, 1, 1.0f, bx2)};
    const std::vector<Rgba8> refOut = fresh.captureViewport(ref);

    EXPECT_EQ(primedOut, refOut);
}

// Rendering key K, then a frame WITHOUT K (evicting K's slot + releasing its texture), then K again must
// reproduce the original output exactly — the evicted slot is recreated cleanly, no stale reuse, no leak.
TEST_F(CacheRekeyTest, EvictedKeyRecreatesCleanly) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    TileBacking bk, bl;
    FrameDrawState withK;
    withK.layers = {makeTileLayer("K", 0, art, 1, 1.0f, bk)};
    const std::vector<Rgba8> first = r.captureViewport(withK);   // creates slot K

    FrameDrawState withoutK;                                     // a different layer — K despawns, evicted
    withoutK.layers = {makeTileLayer("L", 0, art, 3, 1.0f, bl)};
    (void)r.captureViewport(withoutK);

    FrameDrawState withKAgain;
    withKAgain.layers = {makeTileLayer("K", 0, art, 1, 1.0f, bk)};
    const std::vector<Rgba8> third = r.captureViewport(withKAgain);  // recreate slot K

    EXPECT_EQ(third, first);

    // ...and it matches a renderer that only ever saw K — proving no stale content survived the eviction.
    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art2 = uploadBaseArt(fresh);
    TileBacking bk2;
    FrameDrawState onlyK;
    onlyK.layers = {makeTileLayer("K", 0, art2, 1, 1.0f, bk2)};
    EXPECT_EQ(third, fresh.captureViewport(onlyK));
}

// Under WarnAndResolve two layers may collide on one key. The colliding second layer must NOT share the
// first's cache slot — it gets a per-frame transient — so each keeps its own content. Output therefore
// equals the same two layers given DISTINCT keys; if they shared a slot, the second's upload would
// overwrite the first and both would draw the second's content, diverging from the distinct-key reference.
TEST_F(CacheRekeyTest, DuplicateKeyLayersDoNotShareASlot) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.setLayerCollisionPolicy(LayerKeyCollisionPolicy::WarnAndResolve);
    const BaseArt art = uploadBaseArt(r);

    TileBacking bp, bq;
    FrameDrawState dup;                          // two "dup" layers, distinct content + distinct z
    dup.layers = {makeTileLayer("dup", 0, art, 1, 1.0f, bp),
                  makeTileLayer("dup", 10, art, 3, 0.5f, bq)};
    const std::vector<Rgba8> dupOut = r.captureViewport(dup);

    Renderer ref{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art2 = uploadBaseArt(ref);
    TileBacking bp2, bq2;
    FrameDrawState distinct;                     // the same content under distinct keys
    distinct.layers = {makeTileLayer("a", 0, art2, 1, 1.0f, bp2),
                       makeTileLayer("b", 10, art2, 3, 0.5f, bq2)};
    const std::vector<Rgba8> distinctOut = ref.captureViewport(distinct);

    EXPECT_EQ(dupOut, distinctOut);
}

}  // namespace
