// RenderStats: Renderer::renderStats() returns cumulative per-layer tilemap-texture / sprite-record upload
// counts and full-frame compose counts, each paired with its skip counter — the permanent seam a game (and
// these tests) reads to confirm the renderer is eliminating redundant work. These tests pin two facts: the
// counters accumulate across composes (each compose runs one pass and issues its uploads, an unchanged tile
// layer skipping), and each upload is attributed to the right counter (a tile layer bumps only the tilemap
// counters, a sprite layer only the sprite counters).
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the golden-readback
// test uses, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows,
// Metal on the Mac. A device is REQUIRED on every production-representative platform; the one skip is Windows
// on ARM (a courtesy coverage runner with no production-representative GPU backend in CI).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

// Windows on ARM is a courtesy coverage runner: in a VM it has no production-representative GPU backend, and
// its real production path (D3D12 + DXIL) is covered by the Windows x64 job — so a missing device HERE is a
// documented out-of-scope skip. Everywhere else a missing device is a hard failure (mirrors the golden harness).
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

// A small deterministic indexed atlas (16×16 px = a 2×2 tile grid, indices 0..3) + a 4-color palette.
struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

BaseArt uploadBaseArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
        }
    }
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16).atlasId;
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(pal));
    return {atlas, palette};
}

// Backing storage a scene's layer spans point into — must outlive the render call.
struct SceneBacking {
    std::vector<TileCell> cells;
    std::vector<Sprite>   sprites;
};

// An 8×8 tile layer filling the viewport (z 0).
void addTileLayer(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas,
                         .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
        }
    }
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(b.cells)};
    frame.layers.push_back(bg);
}

// Two opaque, all-Normal sprites (z 10) — the single-buffer sprite fast path.
void addSpriteLayer(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.sprites = {Sprite{.key = "sp0", .x = 12, .y = 20, .atlas = art.atlas, .tile = 1, .palette = art.palette},
                 Sprite{.key = "sp1", .x = 40, .y = 36, .atlas = art.atlas, .tile = 3, .palette = art.palette}};
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    frame.layers.push_back(sp);
}

class RenderStatsTest : public ::testing::Test {
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

// Two composes of the same tile+sprite frame each run one compose pass. Both layers upload once then skip —
// their content is unchanged, so tilemapSkips/spriteSkips grow while the upload counters stay flat. Driven via
// captureViewport: it composes unconditionally (the copy pass runs every call), so the per-upload skip is
// observed in isolation. renderFrame would frame-skip the second identical settled submission whole (the
// frame-level compose skip), never reaching the copy pass — a separate behavior, pinned in compose_skip_test.
TEST_F(RenderStatsTest, CountersAccumulateAcrossFrames) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);  // composite verbatim — deterministic per-frame uploads
    const BaseArt art = uploadBaseArt(r);

    SceneBacking b;
    FrameDrawState frame;
    addTileLayer(frame, art, b);
    addSpriteLayer(frame, art, b);

    (void)r.captureViewport(frame);
    const Renderer::RenderStats s1 = r.renderStats();
    EXPECT_EQ(s1.composePasses, 1u);
    EXPECT_GE(s1.tilemapUploads, 1u);
    EXPECT_GE(s1.spriteUploads, 1u);

    (void)r.captureViewport(frame);
    const Renderer::RenderStats s2 = r.renderStats();
    EXPECT_EQ(s2.composePasses, 2u);
    // The tile layer is unchanged, so its upload skips rather than re-issuing.
    EXPECT_EQ(s2.tilemapUploads, s1.tilemapUploads);
    EXPECT_GT(s2.tilemapSkips, s1.tilemapSkips);
    // The sprite layer is settled (no eased motion, identical records), so it skips too.
    EXPECT_EQ(s2.spriteUploads, s1.spriteUploads);
    EXPECT_GT(s2.spriteSkips, s1.spriteSkips);
}

// A tile-only frame bumps ONLY the tilemap counters; a sprite-only frame bumps ONLY the sprite counters —
// each upload attributed to its own counter.
TEST_F(RenderStatsTest, AttributesUploadsToRightCounters) {
    {
        Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
        r.automaticInterpolation(false);
        const BaseArt art = uploadBaseArt(r);
        SceneBacking b;
        FrameDrawState frame;
        addTileLayer(frame, art, b);  // 8×8 = 64 cells, no sprites

        r.renderFrame(frame);
        const Renderer::RenderStats s = r.renderStats();
        EXPECT_EQ(s.tilemapUploads, 1u);
        EXPECT_EQ(s.spriteUploads, 0u);
    }
    {
        Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
        r.automaticInterpolation(false);
        const BaseArt art = uploadBaseArt(r);
        SceneBacking b;
        FrameDrawState frame;
        addSpriteLayer(frame, art, b);  // 2 sprites, all Normal → single-buffer path, no tiles

        r.renderFrame(frame);
        const Renderer::RenderStats s = r.renderStats();
        EXPECT_EQ(s.spriteUploads, 1u);
        EXPECT_EQ(s.tilemapUploads, 0u);
    }
}

// ── The last frame's phase split ─────────────────────────────────────────────────────────────

// A windowless renderer has no swapchain to blit into, so every frame it composes goes unpresented. The
// counter and the per-frame flag agree on that, which is what makes them readable together: a game diffs
// presentPasses for the rate a viewer actually saw and reads presented for the frame in hand.
TEST_F(RenderStatsTest, AFrameWithNoSwapchainIsCountedUnpresented) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    addTileLayer(frame, art, b);

    r.renderFrame(frame);
    const Renderer::RenderStats s = r.renderStats();

    EXPECT_EQ(s.presentPasses, 0u);
    EXPECT_EQ(s.presentSkips, 1u);
    EXPECT_FALSE(s.lastFrame.presented);
}

// Composing costs time and is attributed to composeMs; the phases are wall-clock, so the only safe
// assertions are that the compose phase registers when a compose happens and that no phase reads negative.
TEST_F(RenderStatsTest, ComposingAFrameRegistersOnTheComposePhase) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    addTileLayer(frame, art, b);
    addSpriteLayer(frame, art, b);

    r.renderFrame(frame);
    const Renderer::RenderStats::PhaseTimings p = r.renderStats().lastFrame;

    EXPECT_FALSE(p.composeSkipped);
    EXPECT_GT(p.composeMs, 0.0);
    EXPECT_GE(p.acquireMs, 0.0);
    EXPECT_GE(p.interpMs, 0.0);
    EXPECT_GE(p.presentMs, 0.0);
}

// Re-submitting a settled frame re-blits the retained output instead of recomposing it. The phase split says
// so twice over — the flag names what happened, and composeMs falls to zero because no compose ran.
TEST_F(RenderStatsTest, ASkippedComposeReportsNoComposeTime) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    addTileLayer(frame, art, b);

    r.renderFrame(frame);
    const Renderer::RenderStats::PhaseTimings first = r.renderStats().lastFrame;
    ASSERT_FALSE(first.composeSkipped) << "the first submission has nothing retained to re-blit";

    r.renderFrame(frame);
    const Renderer::RenderStats::PhaseTimings second = r.renderStats().lastFrame;

    EXPECT_TRUE(second.composeSkipped);
    EXPECT_EQ(second.composeMs, 0.0);
}

}  // namespace
