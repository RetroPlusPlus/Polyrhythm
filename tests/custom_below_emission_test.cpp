// An emission-consumer Custom stage joins the emission grammar on the BELOW-scope sprite lens. A
// Below-scope Custom effect whose stage carries `// @retropp:emission` extracts a field over the lens's
// footprint rect (the stage's own emission() body, or the stock brightpass at `.threshold` when it defines
// none), blurs it by `.radius`, and hands it back to the lens's main() through sampleEmission() — the same
// footprint-rect store the built-in Below Bloom/Glow lenses read, with the field authored by the game body.
// Coverage:
//
//   Part 1 (device-free):
//     - collectSpriteBelowCustomEmissionDemand: always engaged (the declaration IS the demand), the reach is
//       the effect's `.radius` verbatim (below reaches are viewport px), the threshold is `.threshold / 255`,
//       the extract is Custom and carries the stage + body flag; an empty footprint yields no demand.
//     - seatPlacedBelowEmissionFields: a Custom demand seats its field row into the record's GATE lanes (its
//       param lanes are the shader's cbuffer), a Bloom/Glow into params[3]; a dropped demand zeroes its own.
//     - emissionExtractInstance: the glow lane carries the fx record row on a Custom demand (the pipeline loads
//       the stage's params from it) and the 0/1 Bloom/Glow flag otherwise.
//
//   Part 2 (device-backed, via captureViewport): a Below-scope lens sits over a vivid-but-low-luminance blue
//     scene. The body lens (blue-channel marker) adds a halo the luminance brightpass cannot; the stock sibling
//     (no body) gates the low-luminance blue out; and outside the lens silhouette the scene is untouched.

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

// ── Part 1: the below custom demand (device-free) ───────────────────────────────────────────────

constexpr PixelBox kQuad{.x = 10, .y = 20, .w = 8, .h = 8};

ScreenSpaceEffect belowCustom(std::uint32_t stage, float radius, std::uint8_t threshold) {
    return ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                             .customShader = static_cast<PostProcessStageId>(stage),
                             .scope        = ScreenSpaceEffectScope::Below,
                             .radius       = radius,
                             .threshold    = threshold};
}

TEST(BelowCustomEmissionDemand, EngagesAndCarriesTheEffectData) {
    const auto d = collectSpriteBelowCustomEmissionDemand(belowCustom(3, 12.0f, 128), 3, /*hasBody=*/true,
                                                          /*storeIndex=*/7, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->extract, EmissionExtract::Custom);
    EXPECT_EQ(d->stage, 3u);
    EXPECT_TRUE(d->hasBody);
    EXPECT_EQ(d->storeIndex, 7u);
    EXPECT_FLOAT_EQ(d->blurReach, 12.0f);                 // the reach is the radius verbatim (viewport px)
    EXPECT_FLOAT_EQ(d->threshold, 128.0f / 255.0f);       // normalized from the 0..255 field
    EXPECT_EQ(d->field.x, kQuad.x);
    EXPECT_EQ(d->field.w, kQuad.w);
}

TEST(BelowCustomEmissionDemand, EngagesEvenAtZeroRadius) {
    // The declaration is the demand — there is no intensity / radius gate. A radius-0 lens still asks (its
    // field is the un-blurred extract).
    const auto d = collectSpriteBelowCustomEmissionDemand(belowCustom(1, 0.0f, 0), 1, false, 0, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_FLOAT_EQ(d->blurReach, 0.0f);
    EXPECT_FALSE(d->hasBody);
}

TEST(BelowCustomEmissionDemand, NegativeRadiusClampsToZero) {
    const auto d = collectSpriteBelowCustomEmissionDemand(belowCustom(1, -5.0f, 0), 1, true, 0, kQuad);
    ASSERT_TRUE(d.has_value());
    EXPECT_FLOAT_EQ(d->blurReach, 0.0f);
}

TEST(BelowCustomEmissionDemand, EmptyFootprintYieldsNoDemand) {
    const PixelBox empty{.x = 4, .y = 4, .w = 0, .h = 8};
    EXPECT_FALSE(collectSpriteBelowCustomEmissionDemand(belowCustom(1, 8.0f, 0), 1, true, 0, empty).has_value());
}

// ── Part 1: gate-lane seating (device-free) ─────────────────────────────────────────────────────

SpriteBelowEmissionDemand customDemand(std::size_t storeIndex) {
    return SpriteBelowEmissionDemand{.storeIndex = storeIndex, .extract = EmissionExtract::Custom};
}
SpriteBelowEmissionDemand bloomDemand(std::size_t storeIndex) {
    return SpriteBelowEmissionDemand{.storeIndex = storeIndex, .extract = EmissionExtract::Bloom};
}

TEST(BelowCustomSeat, CustomFieldRowLandsInTheGateLanes) {
    // A Custom record's param lanes ARE the shader's cbuffer, so the field row seats in the gate (texel 1):
    // radius = the absolute row, strokeWidth = engaged. A Bloom row seats in params[3], its usual place.
    std::vector<SpriteFxRecord> store(2);
    const std::array demands{customDemand(0), bloomDemand(1)};
    const std::array sheetOf{0, 0};
    const int dropped = seatPlacedBelowEmissionFields(store, demands, sheetOf, /*fieldBase=*/5);
    EXPECT_EQ(dropped, 0);
    // Custom (row 0): field row 5 + index 0 = 5 in the gate lanes; params untouched.
    EXPECT_FLOAT_EQ(store[0].radius, 5.0f);
    EXPECT_FLOAT_EQ(store[0].strokeWidth, 1.0f);
    EXPECT_FLOAT_EQ(store[0].params[3], 0.0f);
    // Bloom (row 1): field row 5 + index 1 = 6 in params[3]; gate lanes untouched.
    EXPECT_FLOAT_EQ(store[1].params[3], 6.0f);
    EXPECT_FLOAT_EQ(store[1].radius, 0.0f);
}

TEST(BelowCustomSeat, DroppedCustomZeroesItsGateLanes) {
    std::vector<SpriteFxRecord> store(1);
    store[0].radius      = 99.0f;   // stale values a drop must clear
    store[0].strokeWidth = 99.0f;
    const std::array demands{customDemand(0)};
    const std::array sheetOf{-1};   // nothing could place it
    const int dropped = seatPlacedBelowEmissionFields(store, demands, sheetOf, 0);
    EXPECT_EQ(dropped, 1);
    EXPECT_FLOAT_EQ(store[0].radius, 0.0f);        // engaged reads 0 → the injected sampleEmission returns 0
    EXPECT_FLOAT_EQ(store[0].strokeWidth, 0.0f);
}

// ── Part 1: the extract instance's glow lane (device-free) ──────────────────────────────────────

TEST(BelowCustomExtractInstance, GlowLaneCarriesTheRecordRowForCustom) {
    const EmissionPlacement p{.rectX = 0, .rectY = 0, .rectW = 8, .rectH = 8, .page = 0};
    const EmissionRectEntry e{};
    // Custom: the glow lane is the fx record row (storeIndex), so the custom rect pipeline loads its params.
    SpriteBelowEmissionDemand cd = customDemand(11);
    EXPECT_FLOAT_EQ(emissionExtractInstance(cd, p, e, 64, 64).glow, 11.0f);
    // Bloom / Glow: the 0/1 kind flag, unchanged.
    EXPECT_FLOAT_EQ(emissionExtractInstance(bloomDemand(11), p, e, 64, 64).glow, 0.0f);
    SpriteBelowEmissionDemand gd{.storeIndex = 11, .extract = EmissionExtract::Glow};
    EXPECT_FLOAT_EQ(emissionExtractInstance(gd, p, e, 64, 64).glow, 1.0f);
}

// ── Part 2: the below custom lens (device-backed) ───────────────────────────────────────────────

constexpr int kW = 32, kH = 32;
constexpr Rgba8 kEmitter{5, 5, 200, 255};   // vivid blue, low luminance (≈ 0.10) — the brightpass gates it

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

int idx(int x, int y) { return y * kW + x; }
int rgbSum(Rgba8 c) { return int(c.r) + int(c.g) + int(c.b); }

class BelowCustomEmission : public ::testing::Test {
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
                                "seat and instance coverage is the device-free part, and the D3D12 path is "
                                "covered by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

// An 8×8 solid of `color` — a single-tile atlas whose one 8×8 tile is index 0, and a palette of all `color`.
struct Art { AtlasId atlas{}; PaletteId palette{}; };
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idxs{};   // all index 0
    const AtlasId a = r.uploadAtlas(idxs.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// The scene beneath (layer 0): the emitter tiled to fill the viewport. The lens (layer 1) is one 8×8 sprite at
// (kLensX, kLensY) carrying `lensEffect` — a Below-scope Custom effect that reads the scene beneath and grades
// it inside its silhouette. A null effect leaves the lens off (the plain emitter scene, the baseline). The two
// sprite vectors persist through the capture (the frame holds spans into them).
constexpr int kLensX = 12, kLensY = 12;
struct Scene { std::vector<Sprite> below; std::vector<Sprite> lens; };
FrameDrawState lensScene(const Art& emitter, const Art& lensArt, Scene& keep, const ScreenSpaceEffect* lensEffect) {
    keep.below.clear();
    keep.lens.clear();
    for (int ty = 0; ty < kH / 8; ++ty)
        for (int tx = 0; tx < kW / 8; ++tx)
            keep.below.push_back(Sprite{.key = "e" + std::to_string(ty * 8 + tx), .x = tx * 8, .y = ty * 8,
                                        .atlas = emitter.atlas, .tile = 0, .palette = emitter.palette});
    DrawLayer scene{.key = "scene"};
    scene.z = 0; scene.size = PixelSize{kW, kH};
    scene.content = SpriteContent{.sprites = std::span<const Sprite>(keep.below)};

    FrameDrawState f;
    f.layers.push_back(scene);
    if (lensEffect) {
        keep.lens.push_back(Sprite{.key = "lens", .x = kLensX, .y = kLensY, .atlas = lensArt.atlas, .tile = 0,
                                   .palette = lensArt.palette, .effects = {*lensEffect}});
        DrawLayer over{.key = "over"};
        over.z = 1; over.size = PixelSize{kW, kH};
        over.content = SpriteContent{.sprites = std::span<const Sprite>(keep.lens)};
        f.layers.push_back(over);
    }
    return f;
}

// The center of the lens silhouette, and a point well outside it.
int lensCX() { return kLensX + 4; }
int lensCY() { return kLensY + 4; }

TEST_F(BelowCustomEmission, BodyLensAddsItsMarkerHaloInsideTheSilhouette) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId body = r.registerPostProcessStage("tests/shaders/emission_below_probe.frag.hlsl");
    const Art emitter = uploadSolid(r, kEmitter);
    const Art lensArt = uploadSolid(r, Rgba8{255, 255, 255, 255});   // opaque → full 8×8 coverage

    Scene sBase, sBody;
    const std::vector<Rgba8> base = r.captureViewport(lensScene(emitter, lensArt, sBase, nullptr));
    ScreenSpaceEffect e = belowCustom(static_cast<std::uint32_t>(body), 8.0f, 60);
    const std::vector<Rgba8> got = r.captureViewport(lensScene(emitter, lensArt, sBody, &e));
    ASSERT_EQ(got.size(), base.size());

    // Inside the silhouette the lens output is the scene beneath PLUS its blue-channel marker halo — brighter
    // than the plain emitter it read.
    const int inBody = rgbSum(got[static_cast<std::size_t>(idx(lensCX(), lensCY()))]);
    const int inBase = rgbSum(base[static_cast<std::size_t>(idx(lensCX(), lensCY()))]);
    EXPECT_GT(inBody, inBase + 40) << "the below lens added no marker halo over the scene it read";

    // Outside the silhouette the scene is byte-identical — the below scratch is transparent there.
    const int outX = 2, outY = 2;   // top-left corner, far from the lens at (12,12)..(20,20)
    EXPECT_EQ(got[static_cast<std::size_t>(idx(outX, outY))].r, base[static_cast<std::size_t>(idx(outX, outY))].r);
    EXPECT_EQ(got[static_cast<std::size_t>(idx(outX, outY))].g, base[static_cast<std::size_t>(idx(outX, outY))].g);
    EXPECT_EQ(got[static_cast<std::size_t>(idx(outX, outY))].b, base[static_cast<std::size_t>(idx(outX, outY))].b);
}

TEST_F(BelowCustomEmission, StockSiblingGatesTheDarkContentTheBodyGlows) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId body   = r.registerPostProcessStage("tests/shaders/emission_below_probe.frag.hlsl");
    const PostProcessStageId nobody = r.registerPostProcessStage("tests/shaders/emission_below_probe_nobody.frag.hlsl");
    const Art emitter = uploadSolid(r, kEmitter);
    const Art lensArt = uploadSolid(r, Rgba8{255, 255, 255, 255});

    Scene sBody, sStock;
    ScreenSpaceEffect eBody  = belowCustom(static_cast<std::uint32_t>(body), 8.0f, 60);
    ScreenSpaceEffect eStock = belowCustom(static_cast<std::uint32_t>(nobody), 8.0f, 60);   // brightpass floor the low-luminance blue misses
    const std::vector<Rgba8> gBody  = r.captureViewport(lensScene(emitter, lensArt, sBody, &eBody));
    const std::vector<Rgba8> gStock = r.captureViewport(lensScene(emitter, lensArt, sStock, &eStock));
    ASSERT_EQ(gBody.size(), gStock.size());

    // Both lenses read the same scene; only the extract differs. The body's blue-channel marker glows the
    // low-luminance blue, the stock brightpass (luminance-keyed, floored at 60/255) emits nothing from it.
    const int inBody  = rgbSum(gBody[static_cast<std::size_t>(idx(lensCX(), lensCY()))]);
    const int inStock = rgbSum(gStock[static_cast<std::size_t>(idx(lensCX(), lensCY()))]);
    EXPECT_GT(inBody, inStock + 40) << "the two extract paths did not differ on the low-luminance scene";
}

}  // namespace
