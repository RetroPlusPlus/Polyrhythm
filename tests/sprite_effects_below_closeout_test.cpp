// Below-scope sprite effects — the close-out surface: Transparency, region-confined grading, and the N-flat
// pass-count guarantee. A Below-scope effect turns a sprite into a lens over the composited SCENE beneath its
// layer, confined to the silhouette (the art's alpha coverage). Coverage:
//
//   1. groupSpriteBelowRuns (device-free). The below pass draws one instanced pass per contiguous
//      same-pipeline run of lenses, so N lenses on one pipeline coalesce into ONE run — the pass count tracks
//      the authored pipeline mix, never the sprite count.
//
//   2. Record packing (device-free). buildSpriteBelowRecords packs a whole-silhouette Transparency (a built-in
//      below kind) and a Below-scope region's color/transparency effects; a region's displacing / Custom kinds
//      and a curve boundary are excluded.
//
//   3. Compositing (device-backed, via captureViewport). A whole-silhouette Below Transparency
//      (TransparentInside) scales the lens strength to zero, revealing the untouched scene through the
//      silhouette — it dials out a co-resident ColorFill grade. A Below-scope region grades the scene only
//      where its shape covers (∩ silhouette), leaving the rest of the silhouette and the surround byte-identical.

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

// ── Part 1: the N-flat run grouping (device-free) ───────────────────────────────────────────────

TEST(BelowRunGrouping, ManyLensesOnOnePipelineAreOneRun) {
    // Eight lenses all on the built-in below pipeline (key 0) — one run, not eight passes.
    const std::array<int, 8> keys{0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<SpriteBelowRun> runs = groupSpriteBelowRuns(std::span<const int>(keys));
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].first, 0);
    EXPECT_EQ(runs[0].count, 8);
    EXPECT_EQ(runs[0].pipelineKey, 0);
}

TEST(BelowRunGrouping, RunCountTracksThePipelineMixNotTheSpriteCount) {
    // Twelve lenses, three pipelines, contiguous by pipeline — three runs regardless of how many lenses.
    const std::array<int, 12> keys{0, 0, 0, 0, 0, 3, 3, 3, 7, 7, 7, 7};
    const std::vector<SpriteBelowRun> runs = groupSpriteBelowRuns(std::span<const int>(keys));
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].count, 5); EXPECT_EQ(runs[0].pipelineKey, 0); EXPECT_EQ(runs[0].first, 0);
    EXPECT_EQ(runs[1].count, 3); EXPECT_EQ(runs[1].pipelineKey, 3); EXPECT_EQ(runs[1].first, 5);
    EXPECT_EQ(runs[2].count, 4); EXPECT_EQ(runs[2].pipelineKey, 7); EXPECT_EQ(runs[2].first, 8);
}

TEST(BelowRunGrouping, EqualKeysSplitByADifferentKeyDoNotCoalesceAcross) {
    // Same key on both ends but interrupted — three runs, not two (order-preserving, no reordering to merge).
    const std::array<int, 5> keys{0, 0, 4, 0, 0};
    const std::vector<SpriteBelowRun> runs = groupSpriteBelowRuns(std::span<const int>(keys));
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].pipelineKey, 0); EXPECT_EQ(runs[0].count, 2);
    EXPECT_EQ(runs[1].pipelineKey, 4); EXPECT_EQ(runs[1].count, 1);
    EXPECT_EQ(runs[2].pipelineKey, 0); EXPECT_EQ(runs[2].count, 2);
}

TEST(BelowRunGrouping, NoLensesYieldNoRuns) {
    const std::vector<SpriteBelowRun> runs = groupSpriteBelowRuns(std::span<const int>{});
    EXPECT_TRUE(runs.empty());
}

// ── Part 2: record packing (device-free) ────────────────────────────────────────────────────────

TEST(BelowCloseoutRecords, WholeSilhouetteTransparencyPacksAsABuiltInKind) {
    Sprite s{.key = "s"};
    s.effects = {below(ScreenSpaceEffectKind::ColorFill), below(ScreenSpaceEffectKind::Transparency)};
    const std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[1].kind), ScreenSpaceEffectKind::Transparency);
}

TEST(BelowCloseoutRecords, ARegionPacksItsColourAndTransparencyEffectsAfterTheChain) {
    Sprite s{.key = "s"};
    s.effects = {below(ScreenSpaceEffectKind::ColorFill)};   // one whole-silhouette chain step
    Region rg{.key = "r", .shape = ShapePoints::circle(Point{4, 4}, 3)};
    rg.effects = {below(ScreenSpaceEffectKind::Gleam), below(ScreenSpaceEffectKind::Transparency)};
    s.regions = {rg};
    const std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 3u);                              // chain ColorFill, then the two region records
    EXPECT_EQ(recs[0].flags & kSpriteFxIsRegion, 0u);        // the chain step is not a region
    EXPECT_NE(recs[1].flags & kSpriteFxIsRegion, 0u);        // the region Gleam
    EXPECT_NE(recs[2].flags & kSpriteFxIsRegion, 0u);        // the region Transparency
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[1].kind), ScreenSpaceEffectKind::Gleam);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[2].kind), ScreenSpaceEffectKind::Transparency);
}

TEST(BelowCloseoutRecords, ARegionsDisplacingAndCustomKindsAreExcluded) {
    Sprite s{.key = "s"};
    Region rg{.key = "r", .shape = ShapePoints::circle(Point{4, 4}, 3)};
    rg.effects = {below(ScreenSpaceEffectKind::Ripple), below(ScreenSpaceEffectKind::Custom),
                  below(ScreenSpaceEffectKind::ColorFill)};   // only the ColorFill is region-confinable
    s.regions = {rg};
    const std::vector<SpriteFxRecord> recs = buildSpriteBelowRecords(s);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(static_cast<ScreenSpaceEffectKind>(recs[0].kind), ScreenSpaceEffectKind::ColorFill);
    EXPECT_NE(recs[0].flags & kSpriteFxIsRegion, 0u);
}

// ── Part 3: compositing (device-backed) ─────────────────────────────────────────────────────────

constexpr int kW = 64, kH = 64, kSX = 24, kSY = 20;   // an 8×8 sprite at (24, 20)

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

class SpriteBelowCloseout : public ::testing::Test {
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
                GTEST_SKIP() << "Windows on ARM has no production-representative GPU backend in CI; the run "
                                "grouping and record packing are the device-free part, and the D3D12 path is "
                                "covered by Windows x64. (" << initError_ << ")";
            FAIL() << "no GPU device reachable — " << initError_;
        }
    }
};

struct Art { AtlasId atlas{}; PaletteId palette{}; };

Art uploadSolidBg(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idx{};   // all index 0
    const AtlasId a = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> pal{{colour}};
    return {a, r.uploadPalette(std::span<const Rgba8>(pal))};
}
DrawLayer solidBgLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (auto& c : keep) c = TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z = 0; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}
// An opaque 8×8 sprite — the coverage mask is fully solid (coverage 1 across the whole tile).
Art uploadOpaqueSprite(Renderer& r, Rgba8 rgb) {
    std::array<std::uint8_t, 8 * 8> idx{};
    const AtlasId at = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> pal{{Rgba8{rgb.r, rgb.g, rgb.b, 255}}};
    return {at, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// A frame with the solid background and, optionally, one lens sprite carrying `effects` / `regions`.
FrameDrawState buildScene(const Art& bg, const Art& sprite, std::vector<TileCell>& cells,
                          std::vector<Sprite>& keepS, bool withSprite,
                          std::vector<ScreenSpaceEffect> effects, std::vector<Region> regions) {
    FrameDrawState f;
    f.layers.push_back(solidBgLayer(bg, cells));
    if (withSprite) {
        Sprite s{.key = "lens", .x = kSX, .y = kSY, .atlas = sprite.atlas, .tile = 0, .palette = sprite.palette};
        s.effects = std::move(effects);
        s.regions = std::move(regions);
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
    }
    return f;
}

TEST_F(SpriteBelowCloseout, WholeSilhouetteTransparencyRevealsTheSceneDiallingOutAColorFill) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Rgba8 BG{40, 60, 90, 255};
    const Rgba8 FILL{250, 250, 40, 255};   // the Below fill — well clear of the background
    const Art bg     = uploadSolidBg(r, BG);
    const Art sprite = uploadOpaqueSprite(r, Rgba8{200, 60, 60});   // opaque mask ⇒ coverage 1

    std::vector<TileCell> cB, cN, cF, cT;
    // B: the background alone — the off-silhouette + revealed-scene ground truth.
    std::vector<Sprite> _s0;
    FrameDrawState fB = buildScene(bg, sprite, cB, _s0, /*withSprite=*/false, {}, {});
    const std::vector<Rgba8> B = r.captureViewport(fB);

    // N: a plain opaque sprite (draws art) — its coverage marks the silhouette.
    std::vector<Sprite> sN;
    FrameDrawState fN = buildScene(bg, sprite, cN, sN, true, {}, {});
    const std::vector<Rgba8> N = r.captureViewport(fN);

    // F: a lens with only a Below ColorFill — the silhouette shows the fill.
    ScreenSpaceEffect fill = below(ScreenSpaceEffectKind::ColorFill);
    fill.fill = FILL;
    std::vector<Sprite> sF;
    FrameDrawState fF = buildScene(bg, sprite, cF, sF, true, {fill}, {});
    const std::vector<Rgba8> F = r.captureViewport(fF);

    // T: the same ColorFill THEN a whole-silhouette Transparency (TransparentInside) — the lens strength drops
    // to zero, so the silhouette reveals the untouched scene (the grade is dialled fully out).
    ScreenSpaceEffect clear = below(ScreenSpaceEffectKind::Transparency);
    clear.stencil = StencilMode::TransparentInside;
    std::vector<Sprite> sT;
    FrameDrawState fT = buildScene(bg, sprite, cT, sT, true, {fill, clear}, {});
    const std::vector<Rgba8> T = r.captureViewport(fT);
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(F.size(), B.size());
    ASSERT_EQ(T.size(), B.size());

    const Rgba8 wantFill{FILL.r, FILL.g, FILL.b, 255};
    int covered = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {                 // the disc covers here (N drew art)
            ++covered;
            EXPECT_LE(chDelta(F[i], wantFill), 3) << "covered pixel " << i << " should show the ColorFill";
            EXPECT_TRUE(exactEq(T[i], B[i])) << "covered pixel " << i << " Transparency did not reveal the scene";
        } else {
            EXPECT_TRUE(exactEq(F[i], B[i])) << "off-silhouette pixel " << i << " leaked (ColorFill)";
            EXPECT_TRUE(exactEq(T[i], B[i])) << "off-silhouette pixel " << i << " leaked (Transparency)";
        }
    }
    EXPECT_GT(covered, 0);
}

TEST_F(SpriteBelowCloseout, ARegionConfinesTheSceneGradeToItsShapeIntersectTheSilhouette) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Rgba8 BG{40, 60, 90, 255};
    const Rgba8 FILL{250, 250, 40, 255};
    const Art bg     = uploadSolidBg(r, BG);
    const Art sprite = uploadOpaqueSprite(r, Rgba8{200, 60, 60});

    std::vector<TileCell> cB, cN, cR;
    std::vector<Sprite> _s0;
    FrameDrawState fB = buildScene(bg, sprite, cB, _s0, false, {}, {});
    const std::vector<Rgba8> B = r.captureViewport(fB);

    std::vector<Sprite> sN;
    FrameDrawState fN = buildScene(bg, sprite, cN, sN, true, {}, {});
    const std::vector<Rgba8> N = r.captureViewport(fN);   // marks the silhouette

    // A lens whose region (a small centered circle, sprite-quad px) carries a Below ColorFill: the scene is
    // recolored only inside the circle ∩ silhouette; the rest of the silhouette shows the untouched scene.
    Region rg{.key = "r", .shape = ShapePoints::circle(Point{4, 4}, 2)};
    ScreenSpaceEffect fill = below(ScreenSpaceEffectKind::ColorFill);
    fill.fill = FILL;
    rg.effects = {fill};
    std::vector<Sprite> sR;
    FrameDrawState fR = buildScene(bg, sprite, cR, sR, true, {}, {rg});
    const std::vector<Rgba8> R = r.captureViewport(fR);
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(R.size(), B.size());

    const Rgba8 wantFill{FILL.r, FILL.g, FILL.b, 255};
    int covered = 0, inRegion = 0, outRegionCovered = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {                 // covered by the silhouette
            ++covered;
            if (chDelta(R[i], wantFill) <= 3) {
                ++inRegion;                          // the region recolored the scene here
            } else {
                EXPECT_TRUE(exactEq(R[i], B[i]))     // covered but outside the region → untouched scene
                    << "covered-but-out-of-region pixel " << i << " changed";
                ++outRegionCovered;
            }
        } else {
            EXPECT_TRUE(exactEq(R[i], B[i])) << "off-silhouette pixel " << i << " leaked (region)";
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_GT(inRegion, 0) << "the region recoloured no covered pixel";
    EXPECT_GT(outRegionCovered, 0) << "the region was not confined — it covered the whole silhouette";
}

}  // namespace
