// An emission-consumer Custom stage joins the emission grammar on the LAYER-scope sprite chain. A Layer-scope
// Custom chain step whose stage carries `// @retropp:emission` reads its own blurred silhouette back through
// sampleEmission(). A Custom step has no emission variant, so the field's content is the sprite's own art
// brightpass at the effect's threshold — produced by a SYNTHETIC Bloom record the raster names (the art path
// has no composited scene to hand the stage's emission() body). The field row seats into the Custom record's
// GATE lanes (its param lanes are the shader's cbuffer), the one convention the Below path also uses, and the
// injected sampleEmission maps the body's within-sprite quad uv to the field's compose position through the
// sprite's placement. Coverage:
//
//   Part 1 (device-free):
//     - collectSpriteLayerCustomEmissionDemand: always engaged (the declaration IS the demand), the reach is
//       `.radius × spriteLinearScale` (art px, the chain convention), the threshold rides the synthetic record,
//       the demand carries the synthetic-record index, the Custom record's store row, and the custom flag; a
//       negative radius clamps; an empty footprint yields no demand.
//     - syntheticLayerEmissionRecord: a whole-silhouette Bloom brightpass at `.threshold`, neutral intensity —
//       the record sprite_emission.frag rasters into the field rect.
//     - seatPlacedRegionEmissionFields: a Custom demand seats its field row into the GATE lanes, a built-in
//       region Bloom into params[3]; a dropped Custom zeroes its own gate.
//     - spriteCustomShadowsSiblingHalo: the sibling-halo warning fires for an undeclared inline custom and is
//       suppressed for an emission-declared one.
//
//   Part 2 (device-backed, via captureViewport): a mid-bright sprite carries a Layer-scope Custom effect. The
//     emission probe (main = art + sampleEmission) brightens the silhouette from within with its own blurred
//     halo; the plain sibling (main = art) adds nothing, so their difference inside the silhouette is exactly
//     the field the declared stage reads, and outside the sprite the two scenes are byte-identical.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: the Layer custom demand (device-free) ───────────────────────────────────────────────

constexpr PixelBox kQuad{.x = 10, .y = 20, .w = 8, .h = 8};

ScreenSpaceEffect layerCustom(std::uint32_t stage, float radius, std::uint8_t threshold) {
    return ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                             .customShader = static_cast<PostProcessStageId>(stage),
                             .radius       = radius,
                             .threshold    = threshold};
}

TEST(LayerCustomEmissionDemand, EngagesAndCarriesTheEffectData) {
    // linearScale 1: the reach is the radius verbatim. synthRecordIndex 4 (the raster's emission index),
    // customStoreIndex 7 (where the seat writes back). The demand is a region-shaped custom demand.
    const auto d = collectSpriteLayerCustomEmissionDemand(layerCustom(3, 10.0f, 128), 1.0f,
                                                          /*synthRecordIndex=*/4, /*customStoreIndex=*/7, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->custom);
    EXPECT_EQ(d->recordIndex, 4u);          // the synthetic Bloom record the raster names
    EXPECT_EQ(d->storeIndex, 7u);           // the Custom record whose gate lanes the seat writes
    EXPECT_FLOAT_EQ(d->blurReach, 10.0f);
    EXPECT_EQ(d->field.x, kQuad.x);
    EXPECT_EQ(d->field.w, kQuad.w);
}

TEST(LayerCustomEmissionDemand, ScalesReachByLinearScale) {
    // A Layer reach is authored in the sprite's own art pixels and blurs in viewport pixels — the chain
    // convention. A sprite scaled 2× turns a radius-6 art reach into a 12 px viewport blur.
    const auto d = collectSpriteLayerCustomEmissionDemand(layerCustom(1, 6.0f, 0), 2.0f, 0, 0, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_FLOAT_EQ(d->blurReach, 12.0f);
}

TEST(LayerCustomEmissionDemand, EngagesEvenAtZeroRadius) {
    // The declaration is the demand — no intensity / radius gate. A radius-0 step still asks (un-blurred field).
    const auto d = collectSpriteLayerCustomEmissionDemand(layerCustom(1, 0.0f, 0), 1.0f, 0, 0, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_FLOAT_EQ(d->blurReach, 0.0f);
    EXPECT_TRUE(d->custom);
}

TEST(LayerCustomEmissionDemand, NegativeRadiusClampsToZero) {
    const auto d = collectSpriteLayerCustomEmissionDemand(layerCustom(1, -5.0f, 0), 2.0f, 0, 0, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_FLOAT_EQ(d->blurReach, 0.0f);   // clamped before the linear scale, so still zero
}

TEST(LayerCustomEmissionDemand, EmptyFootprintYieldsNoDemand) {
    const PixelBox empty{.x = 4, .y = 4, .w = 8, .h = 0};
    EXPECT_FALSE(
        collectSpriteLayerCustomEmissionDemand(layerCustom(1, 8.0f, 0), 1.0f, 0, 0, empty).has_value());
}

// ── Part 1: the synthetic Bloom record (device-free) ────────────────────────────────────────────

TEST(SyntheticLayerRecord, IsAWholeSilhouetteBloomBrightpassAtTheThreshold) {
    const SpriteFxRecord r = syntheticLayerEmissionRecord(layerCustom(2, 9.0f, 128));
    EXPECT_EQ(r.kind, static_cast<std::uint32_t>(ScreenSpaceEffectKind::Bloom));
    EXPECT_EQ(r.flags & kSpriteFxIsRegion, 0u);          // whole silhouette — not a confined region
    EXPECT_FLOAT_EQ(r.params[1], 128.0f / 255.0f);       // threshold, normalized (params.y in the raster)
    EXPECT_FLOAT_EQ(r.params[2], 1.0f);                  // neutral intensity — the body applies its own strength
}

// ── Part 1: gate-lane seating (device-free) ─────────────────────────────────────────────────────

SpriteRegionEmissionDemand customRegionDemand(std::size_t storeIndex) {
    return SpriteRegionEmissionDemand{.storeIndex = storeIndex, .custom = true};
}
SpriteRegionEmissionDemand bloomRegionDemand(std::size_t storeIndex) {
    return SpriteRegionEmissionDemand{.storeIndex = storeIndex, .custom = false};
}

TEST(LayerCustomSeat, CustomFieldRowLandsInTheGateLanesRegionInParams) {
    // A Custom record's param lanes ARE the shader's cbuffer, so the field row seats in the gate (texel 1):
    // radius = the absolute row, strokeWidth = engaged. A built-in region Bloom seats in params[3] as before.
    std::vector<SpriteFxRecord> store(2);
    const std::array demands{customRegionDemand(0), bloomRegionDemand(1)};
    const std::array sheetOf{0, 0};
    const int dropped = seatPlacedRegionEmissionFields(store, demands, sheetOf, /*fieldBase=*/5);
    EXPECT_EQ(dropped, 0);
    // Custom (row 0): field row 5 + index 0 = 5 in the gate lanes; params untouched.
    EXPECT_FLOAT_EQ(store[0].radius, 5.0f);
    EXPECT_FLOAT_EQ(store[0].strokeWidth, 1.0f);
    EXPECT_FLOAT_EQ(store[0].params[3], 0.0f);
    // Bloom (row 1): field row 5 + index 1 = 6 in params[3]; gate lanes untouched.
    EXPECT_FLOAT_EQ(store[1].params[3], 6.0f);
    EXPECT_FLOAT_EQ(store[1].radius, 0.0f);
}

TEST(LayerCustomSeat, DroppedCustomZeroesItsGateLanes) {
    std::vector<SpriteFxRecord> store(1);
    store[0].radius      = 99.0f;   // stale values a drop must clear
    store[0].strokeWidth = 99.0f;
    const std::array demands{customRegionDemand(0)};
    const std::array sheetOf{-1};   // nothing could place it
    const int dropped = seatPlacedRegionEmissionFields(store, demands, sheetOf, 0);
    EXPECT_EQ(dropped, 1);
    EXPECT_FLOAT_EQ(store[0].radius, 0.0f);        // engaged reads 0 → the injected sampleEmission returns 0
    EXPECT_FLOAT_EQ(store[0].strokeWidth, 0.0f);
}

// ── Part 1: the sibling-halo warning gate (device-free) ─────────────────────────────────────────

TEST(LayerCustomWarning, ShadowsSiblingHaloOnlyForUndeclaredCustoms) {
    EXPECT_TRUE(spriteCustomShadowsSiblingHalo(/*hasInlineCustom=*/true, /*consumer=*/false));   // undeclared: warn
    EXPECT_FALSE(spriteCustomShadowsSiblingHalo(true, true));    // emission-declared: emission-aware, suppressed
    EXPECT_FALSE(spriteCustomShadowsSiblingHalo(false, false));  // no inline custom at all
    EXPECT_FALSE(spriteCustomShadowsSiblingHalo(false, true));
}

// ── Part 2: the Layer custom lens (device-backed) ───────────────────────────────────────────────

constexpr int   kW = 32, kH = 32;
constexpr Rgba8 kArt{150, 150, 150, 255};   // mid-bright gray: the brightpass fires and art + halo does not clip
constexpr int   kSpriteX = 12, kSpriteY = 12;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

int idx(int x, int y) { return y * kW + x; }
int rgbSum(Rgba8 c) { return int(c.r) + int(c.g) + int(c.b); }

class LayerCustomEmission : public ::testing::Test {
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
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the demand, "
                                "seat and record coverage is the device-free part, and the D3D12 path is covered "
                                "by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

struct Art { AtlasId atlas{}; PaletteId palette{}; };
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idxs{};   // all index 0
    const AtlasId a = r.uploadAtlas(idxs.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// One 8×8 sprite at (kSpriteX, kSpriteY) carrying `effect` (a Layer-scope Custom chain step) — or none for the
// bare-art baseline. The sprite vector persists through the capture (the frame holds a span into it).
struct Scene { std::vector<Sprite> sprites; };
FrameDrawState layerScene(const Art& art, Scene& keep, const ScreenSpaceEffect* effect) {
    keep.sprites.clear();
    Sprite s{.key = "s", .x = kSpriteX, .y = kSpriteY, .atlas = art.atlas, .tile = 0, .palette = art.palette};
    if (effect) s.effects = {*effect};
    keep.sprites.push_back(s);
    DrawLayer layer{.key = "layer"};
    layer.z = 0; layer.size = PixelSize{kW, kH};
    layer.content = SpriteContent{.sprites = std::span<const Sprite>(keep.sprites)};
    FrameDrawState f;
    f.layers.push_back(layer);
    return f;
}

int spriteCX() { return kSpriteX + 4; }
int spriteCY() { return kSpriteY + 4; }

TEST_F(LayerCustomEmission, EmissionCustomAddsItsHaloInsideTheSilhouette) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId emit  = r.registerPostProcessStage("tests/shaders/emission_layer_probe.frag.hlsl");
    const PostProcessStageId plain = r.registerPostProcessStage("tests/shaders/emission_layer_probe_plain.frag.hlsl");
    const Art art = uploadSolid(r, kArt);

    Scene sEmit, sPlain;
    ScreenSpaceEffect eEmit  = layerCustom(static_cast<std::uint32_t>(emit), 8.0f, 0);
    ScreenSpaceEffect ePlain = layerCustom(static_cast<std::uint32_t>(plain), 8.0f, 0);
    const std::vector<Rgba8> gEmit  = r.captureViewport(layerScene(art, sEmit, &eEmit));
    const std::vector<Rgba8> gPlain = r.captureViewport(layerScene(art, sPlain, &ePlain));
    ASSERT_EQ(gEmit.size(), gPlain.size());

    // Inside the silhouette the emission custom's output is the art PLUS its own blurred brightpass halo,
    // brighter than the plain custom that returns the art alone. Both run the identical inline custom dispatch,
    // so the difference is exactly the field the declared stage read back through sampleEmission.
    const int inEmit  = rgbSum(gEmit[static_cast<std::size_t>(idx(spriteCX(), spriteCY()))]);
    const int inPlain = rgbSum(gPlain[static_cast<std::size_t>(idx(spriteCX(), spriteCY()))]);
    EXPECT_GT(inEmit, inPlain + 40) << "the Layer custom added no halo from its own silhouette";

    // Outside the sprite the halo is invisible — it is read INSIDE the art fragment, so a fragment the sprite
    // never covers is byte-identical between the two scenes.
    const int outX = 2, outY = 2;   // top-left corner, far from the sprite at (12,12)..(20,20)
    EXPECT_EQ(gEmit[static_cast<std::size_t>(idx(outX, outY))].r, gPlain[static_cast<std::size_t>(idx(outX, outY))].r);
    EXPECT_EQ(gEmit[static_cast<std::size_t>(idx(outX, outY))].g, gPlain[static_cast<std::size_t>(idx(outX, outY))].g);
    EXPECT_EQ(gEmit[static_cast<std::size_t>(idx(outX, outY))].b, gPlain[static_cast<std::size_t>(idx(outX, outY))].b);
}

}  // namespace
