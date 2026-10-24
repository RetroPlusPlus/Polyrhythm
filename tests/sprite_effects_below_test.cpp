// Below-scope sprite effects (the scene-facing path). A Below-scope chain effect on a
// sprite (scope == Below) distorts / grades the composited SCENE beneath the sprite's layer, confined to
// the sprite's silhouette (its art alpha coverage); the sprite's art then composites on top. Coverage:
//
//   1. Scope classification (device-free). spriteHasLayerEffects / spriteHasBelowEffects split a sprite's
//      effects by scope; buildSpriteFxRecords excludes Below-scope (it never mis-applies as inline);
//      buildSpriteBelowRecords packs the built-in Below kinds (ColorFill / Gleam / RowDisplacement / Ripple /
//      Transparency) while a Below Custom routes through its own scene-read pipeline
//      (spriteBelowInlineCustomShader); spriteHasDisplacement / spriteInlineCustomShader ignore Below-scope steps.
//
//   2. Compositing (device-backed, via captureViewport). A Below-scope ColorFill under a semi-transparent
//      sprite grades the solid scene toward the fill on the silhouette (an exact composite prediction), and
//      leaves every off-silhouette pixel byte-identical to the scene without the sprite (the no-leak
//      property). A Below-scope RowDisplacement moves a scene edge within the silhouette. A Below-scope Custom
//      effect runs a registered game shader whose sampleSource() reads the SCENE, grading it through the
//      silhouette (checked against the probe's exactly-computable saturate(scene * tint + lift)).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/palette.h"
#include "retropp/postprocess.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

ScreenSpaceEffect below(ScreenSpaceEffectKind kind) {
    ScreenSpaceEffect e{.kind = kind};
    e.scope = ScreenSpaceEffectScope::Below;
    return e;
}

// ── Part 1: scope classification (device-free) ─────────────────────────────────────────────────

TEST(BelowScopeSplit, LayerAndBelowEffectsPartitionByScope) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{1, 2, 3, 255}},  // Layer
                 below(ScreenSpaceEffectKind::Ripple)};                                                     // Below
    EXPECT_TRUE(spriteHasLayerEffects(s));
    EXPECT_TRUE(spriteHasBelowEffects(s));
    // The inline records carry only the Layer ColorFill; the Below Ripple is NOT mis-applied inline.
    const std::vector<SpriteFxRecord> inline_ = buildSpriteFxRecords(s);
    ASSERT_EQ(inline_.size(), 1u);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(inline_[0].kind), ScreenSpaceEffectKind::ColorFill);
    // The Below records carry only the Below Ripple.
    const std::vector<SpriteFxRecord> belowRecs = buildSpriteBelowRecords(s);
    ASSERT_EQ(belowRecs.size(), 1u);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(belowRecs[0].kind), ScreenSpaceEffectKind::Ripple);
}

TEST(BelowScopeSplit, ADefaultLayerEffectIsNotBelow) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{9, 9, 9, 255}}};  // Layer (default)
    EXPECT_TRUE(spriteHasLayerEffects(s));
    EXPECT_FALSE(spriteHasBelowEffects(s));
    EXPECT_TRUE(buildSpriteBelowRecords(s).empty());
}

TEST(BelowScopeSplit, OnlyBelowEffectsHaveNoInlineRecords) {
    Sprite s{.key = "s"};
    s.effects = {below(ScreenSpaceEffectKind::RowDisplacement)};
    EXPECT_FALSE(spriteHasLayerEffects(s));
    EXPECT_TRUE(spriteHasBelowEffects(s));
    EXPECT_TRUE(buildSpriteFxRecords(s).empty());          // nothing inline — the plain art draws
    EXPECT_FALSE(spriteHasDisplacement(s));                // a Below displace is NOT an inline art re-read
}

TEST(BelowScopeRecords, PacksTheBuiltInKindsIncludingTransparency) {
    Sprite s{.key = "s"};
    s.effects = {below(ScreenSpaceEffectKind::ColorFill), below(ScreenSpaceEffectKind::Ripple),
                 below(ScreenSpaceEffectKind::Transparency),                                     // built-in below kind
                 below(ScreenSpaceEffectKind::Custom)};                                          // routes via a variant
    const std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 3u);                            // ColorFill + Ripple + Transparency (the built-in path)
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[0].kind), ScreenSpaceEffectKind::ColorFill);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[1].kind), ScreenSpaceEffectKind::Ripple);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[2].kind), ScreenSpaceEffectKind::Transparency);
    // A Below Custom is NOT a built-in record — it routes through the below-custom pipeline
    // (spriteBelowInlineCustomShader selects it), distinct from the Layer-inline pipeline.
    EXPECT_FALSE(spriteInlineCustomShader(s).has_value());       // a Below Custom is not the Layer-inline pipeline
    ASSERT_TRUE(spriteBelowInlineCustomShader(s).has_value());   // it IS the below-custom pipeline
}

TEST(BelowScopeRecords, ARegionScopedBelowPacksARegionRecord) {
    Sprite s{.key = "s"};
    Region rg{.key = "r", .shape = ShapePoints::circle(Point{4, 4}, 3)};
    rg.effects = {below(ScreenSpaceEffectKind::ColorFill)};   // a region-confined scene grade
    s.regions = {rg};
    EXPECT_TRUE(spriteHasBelowRegionEffects(s));
    EXPECT_TRUE(spriteHasBelowEffects(s));                 // the below pass fires for a region-only lens
    EXPECT_TRUE(buildSpriteFxRecords(s).empty());          // a Below region effect is not packed inline
    const std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 1u);                            // one region record for the below pass
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[0].kind), ScreenSpaceEffectKind::ColorFill);
}

TEST(BelowScopeCustom, SelectsTheBelowCustomDistinctFromTheLayerCustom) {
    Sprite s{.key = "s"};
    ScreenSpaceEffect layerCustom{.kind = ScreenSpaceEffectKind::Custom,
                                  .customShader = static_cast<PostProcessStageId>(4)};   // Layer (default scope)
    ScreenSpaceEffect belowCustom = below(ScreenSpaceEffectKind::Custom);
    belowCustom.customShader = static_cast<PostProcessStageId>(9);                       // Below
    s.effects = {layerCustom, belowCustom};
    // Each scope selects its own pipeline: the Layer-inline picks the layer custom, the below pass the below custom.
    const auto layerH = spriteInlineCustomShader(s);
    const auto belowH = spriteBelowInlineCustomShader(s);
    ASSERT_TRUE(layerH.has_value());
    ASSERT_TRUE(belowH.has_value());
    EXPECT_EQ(static_cast<std::size_t>(*layerH), 4u);
    EXPECT_EQ(static_cast<std::size_t>(*belowH), 9u);
    // A Below Custom is never packed into the built-in below records (it routes through its own pipeline).
    EXPECT_TRUE(buildSpriteBelowRecords(s).empty());
    EXPECT_TRUE(spriteHasBelowEffects(s));
}

TEST(BelowScopeCustom, NoBelowCustomIsNullopt) {
    Sprite s{.key = "s"};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Custom,          // Layer custom only
                                   .customShader = static_cast<PostProcessStageId>(2)},
                 below(ScreenSpaceEffectKind::ColorFill)};                          // a built-in Below, not Custom
    EXPECT_FALSE(spriteBelowInlineCustomShader(s).has_value());
    // The Layer custom is not a below effect; the built-in ColorFill packs into the below run.
    ASSERT_EQ(buildSpriteBelowRecords(s).size(), 1u);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(buildSpriteBelowRecords(s)[0].kind),
              ScreenSpaceEffectKind::ColorFill);
}

// ── Part 2: compositing (device-backed) ────────────────────────────────────────────────────────

constexpr int kW = 64, kH = 64, kSX = 24, kSY = 20;   // an 8×8 sprite at (24, 20)

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

class SpriteBelow : public ::testing::Test {
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
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the scope "
                                "classification is the device-free part, and the D3D12 path is covered by "
                                "Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

struct Art { AtlasId atlas{}; PaletteId palette{}; };

// A uniform single-color background (every tile the same solid color) — a predictable scene to grade.
Art uploadSolidBg(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idx{};   // all index 0
    const AtlasId a = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> pal{{colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}
// A background split into a left half (color A) and right half (color B) — for the displacement edge test.
Art uploadSplitBg(Renderer& r, Rgba8 a, Rgba8 b) {
    std::array<std::uint8_t, 8 * 8> idx{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) idx[static_cast<std::size_t>(y) * 8 + x] = x < 4 ? 0 : 1;  // left / right
    const AtlasId at = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> pal{{a, b}};
    return {at, r.uploadPalette(std::span<const Rgba8>(pal))};
}
DrawLayer solidBgLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (auto& c : keep) c = TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}
DrawLayer splitBgLayer(const Art& art, std::vector<TileCell>& keep) {
    // One 8×8 tile scaled to fill: the tile's own 8×8 art is the split, so tile the map with tile 0 stretched.
    keep.assign(1, TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette});
    DrawLayer l{.key = "bg"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 1, .heightInTiles = 1, .cells = std::span<const TileCell>(keep)};
    return l;
}
// A sprite whose solid palette carries alpha `a` (0..255) — the coverage mask AND the top-art opacity.
Art uploadSemiSprite(Renderer& r, Rgba8 rgb, std::uint8_t a) {
    std::array<std::uint8_t, 8 * 8> idx{};
    const AtlasId at = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> pal{{Rgba8{rgb.r, rgb.g, rgb.b, a}}};
    return {at, r.uploadPalette(std::span<const Rgba8>(pal))};
}

TEST_F(SpriteBelow, ColorFillGradesTheSceneOnTheSilhouetteAndLeavesTheSurroundIntact) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Rgba8 BG{40, 60, 90, 255};
    const Rgba8 ART{200, 60, 60};
    const std::uint8_t A = 255;   // an OPAQUE mask ⇒ coverage 1 ⇒ the Below fill fully replaces the scene
    const Rgba8 FILL{250, 250, 40, 255};   // the Below fill — well clear of the background
    const Art bg    = uploadSolidBg(r, BG);
    const Art sprite = uploadSemiSprite(r, ART, A);

    std::vector<TileCell> cB, cN, cG;
    // B: the background alone (no sprite) — the off-silhouette ground truth.
    FrameDrawState fB;
    fB.layers.push_back(solidBgLayer(bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(fB);

    auto scene = [&](bool withBelow, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(solidBgLayer(bg, cells));
        Sprite s{.key = "lens", .x = kSX, .y = kSY, .atlas = sprite.atlas, .tile = 0, .palette = sprite.palette};
        if (withBelow) {
            ScreenSpaceEffect e = below(ScreenSpaceEffectKind::ColorFill);
            e.fill = FILL;
            s.effects = {e};
        }
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(false, cN, sN);
    FrameDrawState fG = scene(true, cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);   // a plain sprite (draws its opaque art)
    const std::vector<Rgba8> G = r.captureViewport(fG);   // the sprite as a Below-scope lens (no art; grades the scene)
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    // A lens draws no art; with an opaque mask (coverage 1) the Below ColorFill fully replaces the scene on
    // the silhouette, so a covered pixel is exactly the fill. (N drew the opaque disc art there — a different
    // color — so N ≠ B marks the silhouette, and G ≠ N confirms the lens replaced the art draw with the grade.)
    const Rgba8 want{FILL.r, FILL.g, FILL.b, 255};

    int covered = 0, changed = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {   // the disc covers here (N drew art)
            ++covered;
            EXPECT_LE(chDelta(G[i], want), 3) << "covered pixel " << i << " Below-graded value";
            if (chDelta(G[i], N[i]) > 2) ++changed;
        } else {
            EXPECT_TRUE(exactEq(G[i], B[i])) << "off-silhouette pixel " << i << " leaked (Below reached beyond the art)";
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_GT(changed, 0) << "the Below effect changed no covered pixel";
}

TEST_F(SpriteBelow, RowDisplacementMovesTheSceneEdgeWithinTheSilhouette) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Rgba8 LEFT{230, 40, 40, 255}, RIGHT{40, 40, 230, 255};
    const Art bg    = uploadSplitBg(r, LEFT, RIGHT);
    const Art sprite = uploadSemiSprite(r, Rgba8{255, 255, 255}, 255);   // opaque mask ⇒ full-strength refraction

    std::vector<TileCell> cB, cN, cG;
    FrameDrawState fB;
    fB.layers.push_back(splitBgLayer(bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(fB);

    auto scene = [&](bool withBelow, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(splitBgLayer(bg, cells));
        // Place the sprite straddling the scene's color seam (viewport x = 32) so a horizontal shift of the
        // scene visibly swaps which side shows under the silhouette.
        Sprite s{.key = "lens", .x = 28, .y = kSY, .atlas = sprite.atlas, .tile = 0, .palette = sprite.palette};
        if (withBelow) {
            ScreenSpaceEffect e = below(ScreenSpaceEffectKind::RowDisplacement);
            e.amplitude = 6.0f;         // viewport px — shift the scene right under the silhouette
            e.frequency = 0.0f;         // a constant (DC) shift, no wave — every row moves the same
            e.axis      = Axis::Horizontal;
            e.phase     = 0.25f;        // sin(2π·0.25) = 1 ⇒ full +amplitude
            s.effects = {e};
        }
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(false, cN, sN);
    FrameDrawState fG = scene(true, cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);
    ASSERT_EQ(N.size(), B.size());

    int covered = 0, moved = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {   // the sprite covers here
            ++covered;
            if (chDelta(G[i], N[i]) > 8) ++moved;   // the scene beneath the lens shifted (vs the plain art in N)
        } else {
            EXPECT_TRUE(exactEq(G[i], B[i])) << "off-silhouette pixel " << i << " leaked";
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_GT(moved, 0) << "the Below displacement moved no covered pixel (the scene edge did not shift)";
}

TEST_F(SpriteBelow, CustomGradesTheSceneThroughTheSilhouetteViaAGameShader) {
    // A Below-scope Custom effect runs a registered game shader whose sampleSource() reads the SCENE (not the
    // art), confined to the silhouette — the refraction lens driven by a custom shader. The probe computes
    // saturate(scene * tint + lift), exactly computable against the solid background.
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/sprite_custom_probe.frag.hlsl");
    const Rgba8 BG{40, 60, 90, 255};
    const Vec3  tint{0.5f, 0.75f, 1.0f};
    const float lift = 0.05f;
    const Art bg     = uploadSolidBg(r, BG);
    const Art sprite = uploadSemiSprite(r, Rgba8{200, 60, 60}, 255);   // opaque mask ⇒ coverage 1

    std::vector<TileCell> cB, cN, cG;
    FrameDrawState fB;
    fB.layers.push_back(solidBgLayer(bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(fB);   // the scene alone — the off-silhouette ground truth

    auto scene = [&](bool withBelow, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(solidBgLayer(bg, cells));
        Sprite s{.key = "lens", .x = kSX, .y = kSY, .atlas = sprite.atlas, .tile = 0, .palette = sprite.palette};
        if (withBelow) {
            ScreenSpaceEffect e = below(ScreenSpaceEffectKind::Custom);
            e.customShader = probe;
            e.tint = tint;
            e.lift = lift;
            s.effects = {e};
        }
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    FrameDrawState fN = scene(false, cN, sN);
    FrameDrawState fG = scene(true, cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);   // a plain sprite (draws its opaque art) — marks the silhouette
    const std::vector<Rgba8> G = r.captureViewport(fG);   // the sprite as a below-custom lens (grades the scene)
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    // Covered pixels: saturate(scene * tint + lift), opaque (coverage 1). This differs from both the raw scene
    // (a no-op passthrough would fail) and the art N drew, so it is a real behavioral gate.
    const Vec4  s = Vec4{BG.r / 255.0f, BG.g / 255.0f, BG.b / 255.0f, 1.0f};
    const Rgba8 want{quant(s.x * tint.x + lift), quant(s.y * tint.y + lift), quant(s.z * tint.z + lift), 255};

    int covered = 0, changed = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {   // the disc covers here (N drew art)
            ++covered;
            EXPECT_LE(chDelta(G[i], want), 3) << "covered pixel " << i << " below-custom graded value";
            if (chDelta(G[i], N[i]) > 2) ++changed;
        } else {
            EXPECT_TRUE(exactEq(G[i], B[i])) << "off-silhouette pixel " << i << " leaked (below-custom reached beyond the art)";
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_GT(changed, 0) << "the below-custom effect changed no covered pixel";
}

}  // namespace
