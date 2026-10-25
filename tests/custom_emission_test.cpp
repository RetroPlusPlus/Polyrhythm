// A custom shader stage joins the emission grammar at the frame-class site. An `// @retropp:emission`
// declaration makes a game-registered Custom stage an emission consumer: the engine extracts a field (the
// stage's own emission() body, or the stock brightpass at `.threshold` when it defines none), blurs it by the
// effect's `.radius`, and hands it back to main() through sampleEmission() — the same extract → blur → apply
// chain a built-in Bloom/Glow runs, with the composite replaced by the stage's own pass. Coverage:
//
//   Part 1 (device-free):
//     - emissionChainPlanCustom: engagement is unconditional (the declaration IS the demand — no intensity
//       gate), radius 0 engages with zero taps (the un-blurred extract), and above the gate it matches the
//       built-in resolver exactly (same halo shape for the same reach).
//     - registry round-trip: an `@retropp:emission` shader reports as a consumer; one with an emission() body
//       carries an extract variant, one without carries none (the stock-brightpass default); a plain custom
//       shader is neither.
//
//   Part 2 (device-backed, via captureViewport): a frame postEffect emits from a vivid-but-low-luminance blue
//     left half and glows it across the vertical boundary. The body path (blue-channel marker) glows the
//     low-luminance content; the stock-brightpass sibling does not (its luminance key gates it out); and the
//     glow reaches further at a larger `.radius` — the reach rides submission data, by construction.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"
#include "retropp/shader_registry.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: emissionChainPlanCustom (device-free) ───────────────────────────────────────────────

TEST(EmissionChainPlanCustom, EngagesUnconditionally) {
    // The declaration is the demand — the plan takes only a reach, never an intensity, so it cannot be gated
    // by one. Engaged at every non-negative reach, including 0.
    EXPECT_TRUE(emissionChainPlanCustom(0.0f).engaged);
    EXPECT_TRUE(emissionChainPlanCustom(3.0f).engaged);
    EXPECT_TRUE(emissionChainPlanCustom(20.0f).engaged);
}

TEST(EmissionChainPlanCustom, ZeroRadiusEngagesWithZeroTaps) {
    const EmissionChainPlan p = emissionChainPlanCustom(0.0f);
    EXPECT_TRUE(p.engaged);
    EXPECT_EQ(p.taps, 0);          // no blur — the stage samples the un-blurred extract
    EXPECT_FALSE(p.downsample);
}

TEST(EmissionChainPlanCustom, NegativeReachClampsToZero) {
    EXPECT_EQ(emissionChainPlanCustom(-5.0f), emissionChainPlanCustom(0.0f));
}

TEST(EmissionChainPlanCustom, MatchesTheBuiltInResolverAboveTheGate) {
    // Above the identity gate a custom stage's halo has the SAME shape a built-in of the same reach would —
    // the custom resolver only removes the gate, never changes the kernel. Compare against an ENGAGED Bloom.
    for (const float reach : {2.0f, 8.0f, 13.0f, 25.0f}) {
        const ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Bloom, .radius = reach, .intensity = 255};
        EXPECT_EQ(emissionChainPlanCustom(reach), emissionChainPlan(e)) << "reach " << reach;
    }
}

TEST(EmissionChainPlanCustom, ReductionEngagesAtTheThresholdRadius) {
    EXPECT_FALSE(emissionChainPlanCustom(kEmissionDownsampleRadius - 1.0f).downsample);
    EXPECT_TRUE(emissionChainPlanCustom(kEmissionDownsampleRadius).downsample);
    EXPECT_TRUE(emissionChainPlanCustom(kEmissionDownsampleRadius + 10.0f).downsample);
}

// ── Part 1: registry round-trip (device-free) ───────────────────────────────────────────────────
//
// The paths are compiled + registered by the build-time scan (added to retroppengine-tests as EXTRA paths in
// CMakeLists) — no device needed: the registration is a static initializer, the accessors read the table.

constexpr const char* kBody   = "tests/shaders/emission_probe.frag.hlsl";         // @retropp:emission + emission()
constexpr const char* kNoBody  = "tests/shaders/emission_probe_nobody.frag.hlsl";  // @retropp:emission, no body
constexpr const char* kPlain   = "tests/shaders/effect_probe.frag.hlsl";           // plain custom stage

TEST(CustomEmissionRegistry, BodyConsumerCarriesAnExtractVariant) {
    EXPECT_TRUE(detail::isEmissionConsumer(kBody));
    EXPECT_NE(detail::findEmissionShaderVariants(kBody), nullptr);   // its emission() body is the extract
}

TEST(CustomEmissionRegistry, NoBodyConsumerHasNoExtractVariant) {
    EXPECT_TRUE(detail::isEmissionConsumer(kNoBody));                // still a consumer…
    EXPECT_EQ(detail::findEmissionShaderVariants(kNoBody), nullptr); // …but the demand uses the stock brightpass
}

TEST(CustomEmissionRegistry, PlainCustomStageIsNotAnEmissionConsumer) {
    EXPECT_FALSE(detail::isEmissionConsumer(kPlain));
    EXPECT_EQ(detail::findEmissionShaderVariants(kPlain), nullptr);
}

TEST(CustomEmissionRegistry, UnregisteredPathIsNotAConsumer) {
    EXPECT_FALSE(detail::isEmissionConsumer("tests/shaders/does_not_exist.frag.hlsl"));
    EXPECT_EQ(detail::findEmissionShaderVariants("tests/shaders/does_not_exist.frag.hlsl"), nullptr);
}

// ── Part 2: the frame-class emission chain (device-backed) ──────────────────────────────────────

constexpr int kW = 64, kH = 64;

// The emitter: a vivid blue that is LOW luminance (Rec.601 ≈ 5·.299 + 5·.587 + 200·.114 ≈ 0.10, since blue
// weighs least), so the stock brightpass (luminance-keyed, floored at threshold 60/255 ≈ 0.235) gates it out
// while the body's blue-channel marker emits it strongly. Opaque, on the opaque-black backdrop.
constexpr Rgba8 kEmitter{5, 5, 200, 255};

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

int idx(int x, int y) { return y * kW + x; }
int rgbSum(Rgba8 c) { return int(c.r) + int(c.g) + int(c.b); }

class CustomEmission : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;
    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { initError_ = std::string("SDL_Init failed: ") + SDL_GetError(); return; }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }
    static void TearDownTestSuite() {
        if (device_) { SDL_DestroyGPUDevice(device_); device_ = nullptr; }
        SDL_Quit();
    }
    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional)
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the plan "
                                "gating and registry round-trip are the device-free parts, and the D3D12 path "
                                "is covered by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

// An 8×8 solid of `color` — a single-tile atlas (the default carve) whose one 8×8 tile is index 0, and a
// palette whose entries are all `color`. A sprite draws this at its native 8×8 footprint.
struct Art { AtlasId atlas{}; PaletteId palette{}; };
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idxs{};   // all index 0
    const AtlasId a = r.uploadAtlas(idxs.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// The scene: a full-height VERTICAL BAR of the emitter (a column of 8×8 sprites at x in [kBarX, kBarX+8),
// y in [0,64)) on the opaque-black backdrop, so the source is the emitter color on the bar and black
// everywhere else — a clean vertical emission boundary at the bar's right edge (x = kBarX + 8). `effect`
// (if any) is a whole-frame postEffect over the composite. captureViewport pins compose scale 1.
constexpr int kBarX = 8, kBarRight = kBarX + 8;
struct Scene { std::vector<Sprite> sprites; };
FrameDrawState barScene(const Art& art, Scene& keep, const ScreenSpaceEffect* effect) {
    keep.sprites.clear();
    for (int ty = 0; ty < 8; ++ty)
        keep.sprites.push_back(Sprite{.key = "bar" + std::to_string(ty), .x = kBarX, .y = ty * 8,
                                      .atlas = art.atlas, .tile = 0, .palette = art.palette});
    DrawLayer l{.key = "bar"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = SpriteContent{.sprites = std::span<const Sprite>(keep.sprites)};
    FrameDrawState f;
    f.layers.push_back(l);
    if (effect) f.postEffects = {*effect};
    return f;
}

// The added glow at (x, y): the effected readback minus the un-effected baseline, summed over rgb (additive,
// so non-negative where the field reached).
int glowAt(const std::vector<Rgba8>& g, const std::vector<Rgba8>& base, int x, int y) {
    return rgbSum(g[static_cast<std::size_t>(idx(x, y))]) - rgbSum(base[static_cast<std::size_t>(idx(x, y))]);
}

TEST_F(CustomEmission, BodyEmissionGlowsTheMarkerAcrossTheBoundary) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId body = r.registerPostProcessStage("tests/shaders/emission_probe.frag.hlsl");
    const Art art = uploadSolid(r, kEmitter);   // vivid blue, low luminance (≈ 0.10) — the brightpass gates it

    Scene sB, sG;
    const std::vector<Rgba8> base = r.captureViewport(barScene(art, sB, nullptr));
    ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Custom, .customShader = body};
    e.radius    = 10.0f;
    e.threshold = 60;   // unread by the body path (it reads the blue channel), set for parity with the sibling
    const std::vector<Rgba8> got = r.captureViewport(barScene(art, sG, &e));
    ASSERT_EQ(got.size(), base.size());

    // Right of the bar's edge (x = kBarRight = 16) the blurred marker glows over the black backdrop —
    // low-luminance content that a luminance brightpass would never radiate.
    const int glowNear = glowAt(got, base, kBarRight + 6,  32);   // x = 22
    const int glowFar  = glowAt(got, base, kBarRight + 24, 32);   // x = 40
    EXPECT_GT(glowNear, 8) << "no glow spread from the bar's marker emission";
    EXPECT_GT(glowNear, glowFar) << "the glow did not fall off with distance from the emitter";
}

TEST_F(CustomEmission, ReachRidesTheRadius) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId body = r.registerPostProcessStage("tests/shaders/emission_probe.frag.hlsl");
    const Art art = uploadSolid(r, kEmitter);

    Scene sB, s1, s2;
    const std::vector<Rgba8> base = r.captureViewport(barScene(art, sB, nullptr));
    ScreenSpaceEffect small{.kind = ScreenSpaceEffectKind::Custom, .customShader = body};
    small.radius = 5.0f;
    ScreenSpaceEffect large{.kind = ScreenSpaceEffectKind::Custom, .customShader = body};
    large.radius = 24.0f;
    const std::vector<Rgba8> gSmall = r.captureViewport(barScene(art, s1, &small));
    const std::vector<Rgba8> gLarge = r.captureViewport(barScene(art, s2, &large));

    // A pixel far from the bar is dark under a tight blur and lit under a wide one — the reach is the
    // submission `.radius`, satisfied by construction (the field constraint the FIELD arc created).
    // At x = 28 (12 px past the bar's edge) the tight radius-5 blur has already fallen to nothing while the
    // radius-24 blur is still well lit — the halo reaches as far as the submitted reach carries it.
    const int farSmall = glowAt(gSmall, base, kBarRight + 12, 32);   // x = 28
    const int farLarge = glowAt(gLarge, base, kBarRight + 12, 32);
    EXPECT_LE(farSmall, 2) << "the tight radius reached further than expected";  // ~0, one-step slack per backend
    EXPECT_GT(farLarge, farSmall + 20) << "the wider radius did not reach substantially further";
}

TEST_F(CustomEmission, StockBrightpassSiblingDoesNotGlowDarkContent) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId body   = r.registerPostProcessStage("tests/shaders/emission_probe.frag.hlsl");
    const PostProcessStageId nobody = r.registerPostProcessStage("tests/shaders/emission_probe_nobody.frag.hlsl");
    const Art art = uploadSolid(r, kEmitter);

    Scene sB, sBody, sStock;
    const std::vector<Rgba8> base = r.captureViewport(barScene(art, sB, nullptr));
    ScreenSpaceEffect eBody{.kind = ScreenSpaceEffectKind::Custom, .customShader = body};
    eBody.radius = 10.0f; eBody.threshold = 60;
    ScreenSpaceEffect eStock{.kind = ScreenSpaceEffectKind::Custom, .customShader = nobody};
    eStock.radius = 10.0f; eStock.threshold = 60;   // the brightpass floor the low-luminance blue falls below
    const std::vector<Rgba8> gBody  = r.captureViewport(barScene(art, sBody, &eBody));
    const std::vector<Rgba8> gStock = r.captureViewport(barScene(art, sStock, &eStock));

    // The body path (blue-channel marker) glows the low-luminance blue; the stock brightpass (luminance-keyed,
    // floored at threshold 60) emits nothing from it. Same reach, same content — only the extract differs.
    const int glowBody  = glowAt(gBody,  base, kBarRight + 6, 32);   // x = 22
    const int glowStock = glowAt(gStock, base, kBarRight + 6, 32);
    EXPECT_GT(glowBody, 8) << "the body path failed to glow the marker";
    EXPECT_LE(glowStock, 2) << "the stock brightpass glowed dark content it should have gated out";
    EXPECT_GT(glowBody, glowStock) << "the two extract paths did not differ on dark content";
}

}  // namespace
