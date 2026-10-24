// Sprite-layer upload skip: a sprite layer's GpuSprite records are uploaded to its GPU cache slot only
// when they change. The renderer builds every record each frame (placement must be evaluated to know
// whether it moved), folds the built bytes plus the layer's effect-record slice into a 64-bit hash, and
// skips the transfer/upload when the hash and record count match the slot's last upload. The win is
// settled layers — a HUD, an idle overlay; a moving sprite's record bytes genuinely differ every frame and
// upload by correctness. A layer that splits into per-run buffers (mixed blend modes, or a custom sprite
// pipeline) draws from the frame-wide run pool rather than its cache slot and never skips.
//
// Device-backed cases are compose-only + windowless (a GPU device, no display): the same harness the
// golden-readback / cache-rekey / tile-upload-skip tests use, so they run on a software rasterizer in CI —
// lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows, Metal on the Mac. A device is REQUIRED on every
// production-representative platform; the one skip is Windows on ARM (a courtesy coverage runner with no
// production-representative GPU backend in CI).

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

// Backing a sprite layer's span points into — must outlive the render/capture call that reads it.
struct SpriteBacking {
    std::vector<Sprite> sprites;
};

// A two-sprite layer keyed `key`, both sprites plain (Normal blend, no effects) so the layer takes the
// single-buffer fast path the skip lives on.
DrawLayer makeSpriteLayer(std::string_view key, const BaseArt& art, SpriteBacking& b) {
    b.sprites = {Sprite{.key = "sp0", .x = 12, .y = 20, .atlas = art.atlas, .tile = 1, .palette = art.palette},
                 Sprite{.key = "sp1", .x = 40, .y = 36, .atlas = art.atlas, .tile = 3, .palette = art.palette}};
    DrawLayer layer{.key = ObjectKey(key)};
    layer.z       = 0;
    layer.size    = PixelSize{kW, kH};
    layer.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    return layer;
}

class SpriteUploadSkipTest : public ::testing::Test {
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

// A settled layer — same sprites, same placement — uploads once, then skips: the built records hash the
// same, so no second transfer is issued. Driven via captureViewport (always composes) so the per-upload skip
// is observed in isolation; renderFrame would frame-skip the second identical settled submission whole (the
// frame-level compose skip, pinned in compose_skip_test), never reaching the copy pass.
TEST_F(SpriteUploadSkipTest, SettledLayerSkipsUpload) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};

    (void)r.captureViewport(frame);
    const Renderer::RenderStats s1 = r.renderStats();
    EXPECT_EQ(s1.spriteUploads, 1u);
    EXPECT_EQ(s1.spriteSkips, 0u);

    (void)r.captureViewport(frame);  // identical submission
    const Renderer::RenderStats s2 = r.renderStats();
    EXPECT_EQ(s2.spriteUploads, 1u);  // no new upload
    EXPECT_EQ(s2.spriteSkips, 1u);    // skipped exactly once
}

// A moving sprite's record bytes carry its placement, so they differ every frame and the layer uploads
// every frame — the honest limit of the sprite skip (settled layers win; motion does not).
TEST_F(SpriteUploadSkipTest, MovingSpriteReuploadsEveryFrame) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};

    r.renderFrame(frame);
    b.sprites[0].x += 1;  // it moved
    r.renderFrame(frame);
    b.sprites[0].x += 1;
    r.renderFrame(frame);

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.spriteUploads, 3u);
    EXPECT_EQ(s.spriteSkips, 0u);
}

// An effect param mutation leaves the GpuSprite records themselves identical — the change lives in the
// effect records they point at — so the layer's effect-record slice folds into the hash alongside them.
// Without that the mutation would skip.
TEST_F(SpriteUploadSkipTest, EffectParamMutationReuploads) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};
    // A whole-silhouette color grade on the first sprite — one effect record, records unchanged.
    b.sprites[0].effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation,
                                              .saturation = 200}};

    r.renderFrame(frame);
    const Renderer::RenderStats s1 = r.renderStats();
    EXPECT_EQ(s1.spriteUploads, 1u);

    b.sprites[0].effects[0].saturation = 40;  // same records, different effect record
    r.renderFrame(frame);

    const Renderer::RenderStats s2 = r.renderStats();
    EXPECT_EQ(s2.spriteUploads, 2u);  // re-uploaded, not skipped
    EXPECT_EQ(s2.spriteSkips, 0u);
}

// A layer whose sprites do not all share BlendMode::Normal splits into per-run buffers drawn from the
// frame-wide run pool. Those slots are handed out fresh each frame, so such a layer never skips.
TEST_F(SpriteUploadSkipTest, MixedBlendLayerNeverSkips) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(r);

    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};
    b.sprites[1].blend = BlendMode::Add;  // forces the run split

    // captureViewport composes both times (the copy pass runs every call); a run-split layer never skips its
    // per-run uploads, so both frames upload their two runs. renderFrame would frame-skip the second whole.
    (void)r.captureViewport(frame);
    (void)r.captureViewport(frame);  // identical submission — still no upload skip on this path

    const Renderer::RenderStats s = r.renderStats();
    EXPECT_EQ(s.spriteSkips, 0u);
    EXPECT_GE(s.spriteUploads, 4u);  // two runs uploaded per frame
}

// A skipped frame renders byte-identically to a fresh renderer that uploads the same frame — skipping the
// upload does not corrupt the output.
TEST_F(SpriteUploadSkipTest, SkippedFrameMatchesFreshRenderer) {
    Renderer primed{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    primed.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(primed);
    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};

    primed.renderFrame(frame);                                         // upload
    const std::vector<Rgba8> skipped = primed.captureViewport(frame);  // skip path
    EXPECT_GT(primed.renderStats().spriteSkips, 0u);                   // it really skipped

    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    fresh.automaticInterpolation(false);
    const BaseArt art2 = uploadBaseArt(fresh);
    SpriteBacking b2;
    FrameDrawState ref;
    ref.layers = {makeSpriteLayer("sprites", art2, b2)};
    const std::vector<Rgba8> uploaded = fresh.captureViewport(ref);

    EXPECT_EQ(skipped, uploaded);
}

// After a move re-uploads, the output matches a fresh renderer drawing the moved frame — the re-upload
// carries the new records, not the stale buffer. (Under an always-skip fault this diverges.)
TEST_F(SpriteUploadSkipTest, MovedSpriteMatchesFreshRenderer) {
    Renderer primed{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    primed.automaticInterpolation(false);
    const BaseArt art = uploadBaseArt(primed);
    SpriteBacking b;
    FrameDrawState frame;
    frame.layers = {makeSpriteLayer("sprites", art, b)};

    primed.renderFrame(frame);  // upload original placement
    b.sprites[0].x = 30;        // move it
    b.sprites[1].y = 8;
    const std::vector<Rgba8> after = primed.captureViewport(frame);

    Renderer fresh{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    fresh.automaticInterpolation(false);
    const BaseArt art2 = uploadBaseArt(fresh);
    SpriteBacking b2;
    FrameDrawState ref;
    ref.layers = {makeSpriteLayer("sprites", art2, b2)};
    b2.sprites[0].x = 30;
    b2.sprites[1].y = 8;
    const std::vector<Rgba8> refOut = fresh.captureViewport(ref);

    EXPECT_EQ(after, refOut);
}

}  // namespace
