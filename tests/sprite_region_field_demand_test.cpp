// Region-Bloom demands — what a layer asks the atlas for, and what it does with the answer.
//
// A field only has to cover the pixels that read it, and a region Bloom writes inside the sprite's drawn
// footprint. So a demand is that footprint plus the step's reach, and a layer collects every sprite's
// demands before planning once — an atlas plan spans the layer, so no record can learn where its field
// landed until all of them exist.
//
// These tests pin both halves of that: which steps produce a demand and what a demand carries, then what
// the writeback does with a plan's verdict. Device-free — the arithmetic decides, so the arithmetic is
// asserted.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/emission_atlas.h>
#include <retropp/postprocess.h>

#include <cstddef>
#include <span>
#include <vector>

using namespace retropp;

namespace {

ScreenSpaceEffect bloom(float radius, std::uint8_t threshold = 0, std::uint8_t intensity = 255) {
    return ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                             .radius    = radius,
                             .threshold = threshold,
                             .intensity = intensity};
}

ScreenSpaceEffect fill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}

Region regionWith(std::string key, std::vector<ScreenSpaceEffect> effects) {
    return Region{.key     = std::move(key),
                  .shape   = ShapePoints{.points = {{0.0f, 0.0f}, {8.0f, 0.0f}, {8.0f, 8.0f}, {0.0f, 8.0f}}},
                  .effects = std::move(effects)};
}

// Where a kind sits in a packed slice — the independent answer an emission index is checked against.
std::size_t recordIndexOf(std::span<const SpriteFxRecord> recs, ScreenSpaceEffectKind kind, bool isRegion,
                          std::size_t nth = 0) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const bool region = (recs[i].flags & kSpriteFxIsRegion) != 0u;
        if (recs[i].kind != static_cast<std::uint32_t>(kind) || region != isRegion) continue;
        if (seen++ == nth) return i;
    }
    return recs.size();
}

constexpr PixelBox kQuad{.x = 40, .y = 24, .w = 32, .h = 16};

std::vector<SpriteRegionEmissionDemand> demandsFor(std::vector<SpriteFxRecord>& recs, float linearScale = 1.0f,
                                                   std::size_t storeOffset = 0) {
    return collectSpriteRegionEmissionDemands(recs, linearScale, storeOffset, kQuad);
}

}  // namespace

// ── What produces a demand ──────────────────────────────────────────────────────────────────

TEST(RegionFieldDemand, ARealizedRegionBloomAsksForItsOwnFootprint) {
    Sprite s{.key = "glow"};
    s.regions = {regionWith("halo", {bloom(6.0f)})};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);

    const std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs);

    ASSERT_EQ(demands.size(), 1u);
    EXPECT_EQ(demands[0].field.x, kQuad.x);
    EXPECT_EQ(demands[0].field.y, kQuad.y);
    EXPECT_EQ(demands[0].field.w, kQuad.w);
    EXPECT_EQ(demands[0].field.h, kQuad.h);
}

TEST(RegionFieldDemand, TheReachIsTheArtRadiusThroughThePlacementsLinearScale) {
    // A reach is authored in the sprite's own art pixels and blurs in viewport pixels.
    Sprite s{.key = "glow"};
    s.regions = {regionWith("halo", {bloom(6.0f)})};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);

    const std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs, 2.5f);

    ASSERT_EQ(demands.size(), 1u);
    EXPECT_FLOAT_EQ(demands[0].blurReach, 15.0f);
}

TEST(RegionFieldDemand, ThePackersSpreadCoversHowFarTheBlurActuallySteps) {
    // The packer isolates a rect by its spread, not by the reach: a blur rounds its tap count up, and on
    // the reduced path it rounds up to a multiple of the reduction. A margin built from the bare reach
    // would be shorter than the blur's own travel.
    for (const float reach : {0.5f, 3.0f, 7.5f, 8.0f, 9.0f, 20.0f, 31.0f}) {
        ScreenSpaceEffect       probe{.kind = ScreenSpaceEffectKind::Bloom, .intensity = 255};
        const EmissionChainPlan plan   = emissionChainPlan(probe, reach);
        const float             spread = emissionAtlasSpread(reach);
        const float             travel = static_cast<float>(plan.taps) * plan.stepPx;
        EXPECT_GT(static_cast<float>(emissionMargin(spread)), travel)
            << "reach " << reach << " blurs " << travel << " px past its content";
    }
}

TEST(RegionFieldDemand, TheSpreadRisesWithTheReachSoOnlyEqualKernelsSharePage) {
    // Pages group by exact spread, and a page blurs once. Two steps may only share one when they resolve
    // the same kernel — which holds because the spread is strictly increasing in the reach.
    EXPECT_LT(emissionAtlasSpread(7.5f), emissionAtlasSpread(8.0f));
    EXPECT_LT(emissionAtlasSpread(8.0f), emissionAtlasSpread(8.25f));
    EXPECT_FLOAT_EQ(emissionAtlasSpread(6.0f), emissionAtlasSpread(6.0f));
}

TEST(RegionFieldDemand, AStepWithNoStrengthAsksForNothingAndIsZeroed) {
    Sprite s{.key = "glow"};
    s.regions = {regionWith("halo", {bloom(6.0f, 0, 0)})};  // intensity 0 — the chain does not engage
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    const std::size_t           at   = recordIndexOf(recs, ScreenSpaceEffectKind::Bloom, true);
    ASSERT_LT(at, recs.size());

    const std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs);

    EXPECT_TRUE(demands.empty());
    EXPECT_FLOAT_EQ(recs[at].params[2], 0.0f) << "an unengaged step must grade with the pixel it started from";
}

TEST(RegionFieldDemand, AChainBloomAsksForNothingHere) {
    // A whole-silhouette Bloom is an additive halo through the shared bucket, not a graded source.
    Sprite s{.key = "glow"};
    s.effects = {bloom(6.0f)};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);

    EXPECT_TRUE(demandsFor(recs).empty());
}

TEST(RegionFieldDemand, ANonBloomRegionEffectAsksForNothing) {
    Sprite s{.key = "graded"};
    s.regions = {regionWith("tint", {fill(Rgba8{.r = 255, .g = 0, .b = 0, .a = 255})})};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);

    EXPECT_TRUE(demandsFor(recs).empty());
}

// ── Indices ─────────────────────────────────────────────────────────────────────────────────

TEST(RegionFieldDemand, TheRecordIndexIsThePackedSlicePositionAndTheStoreIndexOffsetsIt) {
    // The hazard the index convention exists for: a chain step ahead of the region shifts its position, and
    // the raster walks the color chain up to that index. It is checked against the packer's own output.
    Sprite s{.key = "glow"};
    s.effects = {fill(Rgba8{.r = 10, .g = 20, .b = 30, .a = 255})};
    s.regions = {regionWith("halo", {bloom(6.0f)})};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    const std::size_t           at   = recordIndexOf(recs, ScreenSpaceEffectKind::Bloom, true);
    ASSERT_LT(at, recs.size());

    const std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs, 1.0f, 100u);

    ASSERT_EQ(demands.size(), 1u);
    EXPECT_EQ(demands[0].recordIndex, at);
    EXPECT_EQ(demands[0].storeIndex, 100u + at);
}

TEST(RegionFieldDemand, EveryRegionBloomOnASpriteAsksSeparately) {
    Sprite s{.key = "glow"};
    s.regions = {regionWith("a", {bloom(4.0f)}), regionWith("b", {bloom(9.0f)})};
    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);

    const std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs);

    ASSERT_EQ(demands.size(), 2u);
    EXPECT_FLOAT_EQ(demands[0].blurReach, 4.0f);
    EXPECT_FLOAT_EQ(demands[1].blurReach, 9.0f);
    EXPECT_LT(demands[0].recordIndex, demands[1].recordIndex) << "demands follow packed-record order";
}

// ── The writeback ───────────────────────────────────────────────────────────────────────────

TEST(RegionFieldDemand, APlacedDemandPointsItsRecordAtItsOwnFieldIndex) {
    Sprite s{.key = "glow"};
    s.regions = {regionWith("a", {bloom(4.0f)}), regionWith("b", {bloom(9.0f)})};
    std::vector<SpriteFxRecord>             recs    = buildSpriteFxRecords(s);
    std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs);
    ASSERT_EQ(demands.size(), 2u);

    std::vector<EmissionDemand> fields;
    for (const SpriteRegionEmissionDemand& d : demands) fields.push_back(d.field);
    const EmissionAtlasPlan plan = planEmissionAtlas(std::span<const EmissionDemand>{fields}, 1024);

    const int dropped = seatPlannedRegionEmissionFields(recs, demands, plan);

    EXPECT_EQ(dropped, 0);
    EXPECT_FLOAT_EQ(recs[demands[0].storeIndex].params[3], 0.0f);
    EXPECT_FLOAT_EQ(recs[demands[1].storeIndex].params[3], 1.0f);
    EXPECT_GT(recs[demands[0].storeIndex].params[2], 0.0f) << "a placed step keeps its strength";
    EXPECT_GT(recs[demands[1].storeIndex].params[2], 0.0f);
}

TEST(RegionFieldDemand, ADemandThePlanCouldNotPlaceIsZeroedAndCounted) {
    Sprite s{.key = "glow"};
    s.regions = {regionWith("halo", {bloom(6.0f)})};
    std::vector<SpriteFxRecord>             recs    = buildSpriteFxRecords(s);
    std::vector<SpriteRegionEmissionDemand> demands = demandsFor(recs);
    ASSERT_EQ(demands.size(), 1u);

    // An atlas far too small for the footprint: the demand cannot be placed at all.
    std::vector<EmissionDemand> fields{demands[0].field};
    const EmissionAtlasPlan     plan = planEmissionAtlas(std::span<const EmissionDemand>{fields}, 8);
    ASSERT_EQ(plan.placements[0].page, -1);

    const int dropped = seatPlannedRegionEmissionFields(recs, demands, plan);

    EXPECT_EQ(dropped, 1);
    EXPECT_FLOAT_EQ(recs[demands[0].storeIndex].params[2], 0.0f) << "a dropped step must add no light";
    EXPECT_FLOAT_EQ(recs[demands[0].storeIndex].params[3], 0.0f);
}

TEST(RegionFieldDemand, ALayersDemandsAcrossSpritesIndexOneSharedPlan) {
    // Two sprites in one layer: their records live in one store and their demands index one plan, which is
    // why collection has to finish before any record learns its field.
    Sprite a{.key = "a"};
    a.regions = {regionWith("halo", {bloom(4.0f)})};
    Sprite b{.key = "b"};
    b.regions = {regionWith("halo", {bloom(4.0f)})};

    std::vector<SpriteFxRecord> store = buildSpriteFxRecords(a);
    const std::size_t           base  = store.size();
    std::vector<SpriteFxRecord> second = buildSpriteFxRecords(b);
    store.insert(store.end(), second.begin(), second.end());

    std::vector<SpriteRegionEmissionDemand> demands =
        collectSpriteRegionEmissionDemands(std::span<SpriteFxRecord>{store}.first(base), 1.0f, 0, kQuad);
    for (const SpriteRegionEmissionDemand& d : collectSpriteRegionEmissionDemands(
             std::span<SpriteFxRecord>{store}.subspan(base), 1.0f, base, kQuad))
        demands.push_back(d);
    ASSERT_EQ(demands.size(), 2u);
    EXPECT_GE(demands[1].storeIndex, base) << "the second sprite's record sits past the first's slice";

    std::vector<EmissionDemand> fields;
    for (const SpriteRegionEmissionDemand& d : demands) fields.push_back(d.field);
    const EmissionAtlasPlan plan = planEmissionAtlas(std::span<const EmissionDemand>{fields}, 1024);

    // One reach across both sprites, so both land on one page and one blur pass serves them.
    ASSERT_EQ(plan.pages.size(), 1u);
    EXPECT_EQ(seatPlannedRegionEmissionFields(store, demands, plan), 0);
    EXPECT_FLOAT_EQ(store[demands[0].storeIndex].params[3], 0.0f);
    EXPECT_FLOAT_EQ(store[demands[1].storeIndex].params[3], 1.0f);
}
