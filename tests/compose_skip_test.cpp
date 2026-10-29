// Frame-level compose skip — device-backed. When a submission is provably bit-identical
// to the frame that produced the retained compose output, renderFrame skips composeViewport entirely (copy
// pass → layer composite → post-process) and re-blits the retained output. These tests pin the skip decision
// against a live GPU device: an identical settled resubmission skips; any content mutation, an upload that
// recreates a GPU store, or an unsettled (mid-ease) frame recomposes; and captureViewport never skips (the
// golden path always composes). RenderStats::composeSkips / composePasses are the observable seam.
//
// Device-backed, compose-only + windowless (a GPU device, no display): the same harness the golden-readback /
// upload-stats tests use, so it runs on a software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP (D3D12)
// on Windows, Metal on the Mac. A device is REQUIRED on every production-representative platform; the one skip
// is Windows on ARM (a courtesy coverage runner with no production-representative GPU backend in CI).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/frame_timing.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

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

// Backing a scene's spans point into — must outlive the render call.
struct SceneBacking {
    std::vector<TileCell> cells;
    std::vector<Sprite>   sprites;
};

// An 8×8 tile layer + one sprite. The sprite carries a key so the interpolation-settle test can move it.
void buildScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& b, int spriteX = 12) {
    b.cells.assign(8 * 8, TileCell{.atlas = art.atlas, .tile = 1, .palette = art.palette});
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(b.cells)};

    b.sprites = {Sprite{.key = "hero", .x = spriteX, .y = 20, .atlas = art.atlas, .tile = 3, .palette = art.palette}};
    DrawLayer sp{.key = "actors"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};

    frame.layers = {bg, sp};
}

class ComposeSkipTest : public ::testing::Test {
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

// An identical settled submission composes once, then skips: the fingerprint matches and no store upload
// intervened, so the second renderFrame re-blits the retained output rather than recomposing.
TEST_F(ComposeSkipTest, IdenticalResubmitSkipsCompose) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);

    r.renderFrame(frame);
    const Renderer::RenderStats s1 = r.renderStats();
    EXPECT_EQ(s1.composePasses, 1u);
    EXPECT_EQ(s1.composeSkips, 0u);  // first frame has no retained output — it composes

    r.renderFrame(frame);
    const Renderer::RenderStats s2 = r.renderStats();
    EXPECT_EQ(s2.composePasses, 1u);  // no new compose
    EXPECT_EQ(s2.composeSkips, 1u);   // re-blitted the retained output

    r.renderFrame(frame);
    const Renderer::RenderStats s3 = r.renderStats();
    EXPECT_EQ(s3.composePasses, 1u);
    EXPECT_EQ(s3.composeSkips, 2u);  // keeps skipping while nothing changes
}

// Any content change breaks the fingerprint, so the frame recomposes rather than skipping. One representative
// mutation per input surface — a tile cell, a sprite position, a layer scalar.
TEST_F(ComposeSkipTest, ContentMutationRecomposes) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);

    r.renderFrame(frame);   // compose (composePasses 1)
    r.renderFrame(frame);   // skip     (composeSkips 1)
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    b.cells[0].tile = 2;    // a tile changed
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 2u) << "tile change recomposes";
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    b.sprites[0].x = 30;    // a sprite moved (discrete, interpolation off)
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 3u) << "sprite move recomposes";

    frame.layers[0].alpha = 0.5f;  // a layer scalar changed
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 4u) << "layer alpha recomposes";

    r.renderFrame(frame);   // now settled + identical again → skip resumes
    EXPECT_EQ(r.renderStats().composePasses, 4u);
    EXPECT_EQ(r.renderStats().composeSkips, 2u);
}

// An upload written into a store's free room leaves every texel the retained output was composed from
// exactly as it was, so an identical frame that does not reference the new content keeps skipping. Both
// stores are written in place here: a palette, and an atlas the store's free room holds.
TEST_F(ComposeSkipTest, AppendedUploadKeepsSkipping) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);

    r.renderFrame(frame);   // compose
    r.renderFrame(frame);   // skip
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    const std::array<Rgba8, 2> extra{{{1, 2, 3}, {4, 5, 6}}};
    (void)r.uploadPalette(std::span<const Rgba8>(extra));
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 1u) << "an appended palette leaves the composed image alone";
    EXPECT_EQ(r.renderStats().composeSkips, 2u);

    std::array<std::uint8_t, 16 * 16> second{};  // fits the store's free room — written in place
    (void)r.uploadAtlas(second.data(), 16, 16);
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 1u) << "an appended atlas leaves the composed image alone";
    EXPECT_EQ(r.renderStats().composeSkips, 3u);
}

// An upload that recreates a store texture bumps the store generation, forcing a recompose even though the
// fingerprint matches — the compose binds a different texture than the retained output was drawn from. An
// atlas the store's free room does not hold is that upload: the sheets are laid out afresh in a new texture.
TEST_F(ComposeSkipTest, AGrownStoreForcesRecompose) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);

    r.renderFrame(frame);   // compose
    r.renderFrame(frame);   // skip
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    std::array<std::uint8_t, 32 * 16> wider{};
    (void)r.uploadAtlas(wider.data(), 32, 16);  // no room for it → a new store texture

    r.renderFrame(frame);   // identical frame, but the store it binds is a different texture → recompose
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    r.renderFrame(frame);   // generation matches again → skip resumes
    EXPECT_EQ(r.renderStats().composeSkips, 2u);
}

// captureViewport is the golden/inspection path — it always composes, never skips, even when primed with an
// identical retained frame.
TEST_F(ComposeSkipTest, CaptureViewportNeverSkips) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);

    r.renderFrame(frame);          // prime the retained output
    (void)r.captureViewport(frame);
    (void)r.captureViewport(frame);
    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.composeSkips, 0u) << "captureViewport must always compose";
    EXPECT_GE(s.composePasses, 3u);  // renderFrame + 2 captures
}

// A tile layer declaring contentChanged == true is declared-dirty: the frame recomposes even when its
// fingerprint (which does not read a declared layer's cells) is otherwise stable.
TEST_F(ComposeSkipTest, DeclaredDirtyForcesRecompose) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b);
    std::get<TileContent>(frame.layers[0].content).contentChanged = true;  // manual path, always dirty

    r.renderFrame(frame);
    r.renderFrame(frame);
    r.renderFrame(frame);
    // Every frame declares the map changed, so the frame never skips.
    EXPECT_EQ(r.renderStats().composeSkips, 0u);
    EXPECT_EQ(r.renderStats().composePasses, 3u);
}

// Interpolation on: while a keyed sprite is mid-ease the frame is unsettled and never skips; only after the
// motion settles (an equal tick) does the skip engage — and the first settled frame recomposes (the retained
// output was a mid-ease frame), so a stale mid-ease image is never re-blitted as the settled one.
TEST_F(ComposeSkipTest, UnsettledMotionNeverSkipsThenSettles) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(true);
    const BaseArt art = uploadBaseArt(r);
    SceneBacking b;
    FrameDrawState frame;
    buildScene(frame, art, b, /*spriteX=*/0);

    // Tick 1 mounts the sprite (prev == cur → settled); first frame composes.
    publishFrameTiming(FrameTiming{.alpha = 0.0f, .tickAdvanced = true});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composeSkips, 0u);
    EXPECT_EQ(r.renderStats().composePasses, 1u);

    // Same submission, mid-tick, no new tick: still settled, fingerprint matches → skip.
    publishFrameTiming(FrameTiming{.alpha = 0.5f, .tickAdvanced = false});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    // Move the sprite and commit a tick: now prev 0, cur 30 → unsettled → recompose, no skip.
    b.sprites[0].x = 30;
    publishFrameTiming(FrameTiming{.alpha = 0.0f, .tickAdvanced = true});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 1u) << "unsettled frame must not skip";

    // Mid-ease frame (no tick): still unsettled (prev 0 ≠ cur 30) → recompose, no skip.
    publishFrameTiming(FrameTiming{.alpha = 0.5f, .tickAdvanced = false});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 3u);
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    // Equal tick: prev 30, cur 30 → settled again, BUT the retained output was a mid-ease frame, so the first
    // settled frame recomposes (never re-blits the mid-ease image as settled).
    publishFrameTiming(FrameTiming{.alpha = 0.0f, .tickAdvanced = true});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 4u) << "first settled frame recomposes the settled image";
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    // Now settled + identical + retained frame was composed settled → skip resumes.
    publishFrameTiming(FrameTiming{.alpha = 0.5f, .tickAdvanced = false});
    r.renderFrame(frame);
    EXPECT_EQ(r.renderStats().composePasses, 4u);
    EXPECT_EQ(r.renderStats().composeSkips, 2u);

    publishFrameTiming(FrameTiming{});  // reset the thread-local timing for later tests
}

}  // namespace
