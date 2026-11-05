// A sprite's own halo as a field — which sprites share one, how large it is, and where its composite draws.
//
// A whole-silhouette Bloom / Glow spills past the sprite's quad, so its field is the quad grown by the
// spread. Sprites of one kind and reach whose grown footprints overlap share a field, so their light sums
// before the blur and the composite's coverage rule runs once per pixel. These tests pin the grouping, the
// growth, the composite record's compose-grid quad, and that the halo writes nothing into the art record.
//
// Device-free: the arithmetic decides where a halo lands and which halos combine, so the arithmetic is
// asserted.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/emission_atlas.h>
#include <retropp/postprocess.h>

#include <cstddef>
#include <cstring>
#include <vector>

using namespace retropp;

namespace {

SpriteEmissionStep bloomStep(float reach) {
    return SpriteEmissionStep{.recordIndex = 0, .glow = false, .reach = reach};
}

SpriteEmissionStep glowStep(float reach) {
    return SpriteEmissionStep{.recordIndex = 0, .glow = true, .reach = reach};
}

constexpr PixelBox box(int x, int y, int w, int h) {
    return PixelBox{.x = x, .y = y, .w = w, .h = h};
}

}  // namespace

// ── Which sprites share a field ─────────────────────────────────────────────────────────────

TEST(SpriteHaloField, TwoSpritesWhoseGrownFootprintsOverlapShareACluster) {
    // 4 px apart at reach 6: the margin is 9 a side, so the grown boxes overlap while the quads do not touch.
    const std::vector<SpriteEmissionStep> steps{bloomStep(6.0f), bloomStep(6.0f)};
    const std::vector<PixelBox>           footprints{box(10, 10, 16, 16), box(30, 10, 16, 16)};

    const std::vector<SpriteHaloCluster> clusters = clusterSpriteHaloEmission(steps, footprints);

    ASSERT_EQ(clusters.size(), 1u);
    EXPECT_FALSE(clusters[0].glow);
    EXPECT_EQ(clusters[0].blurReach, 6.0f);
    EXPECT_EQ(clusters[0].members, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(clusters[0].box, box(10, 10, 36, 16));
}

TEST(SpriteHaloField, TwoSpritesOutOfReachKeepTheirOwnClusters) {
    const std::vector<PixelBox> footprints{box(10, 10, 16, 16), box(66, 10, 16, 16)};  // 40 px apart

    const std::vector<SpriteEmissionStep> near{bloomStep(6.0f), bloomStep(6.0f)};
    const std::vector<SpriteHaloCluster>  apart = clusterSpriteHaloEmission(near, footprints);
    ASSERT_EQ(apart.size(), 2u);
    EXPECT_EQ(apart[0].members, (std::vector<std::size_t>{0}));
    EXPECT_EQ(apart[0].box, footprints[0]);
    EXPECT_EQ(apart[1].members, (std::vector<std::size_t>{1}));
    EXPECT_EQ(apart[1].box, footprints[1]);

    // At reach 20 the spread is 24 and the margin 26 a side, so the same two boxes reach each other.
    const std::vector<SpriteEmissionStep> wide{bloomStep(20.0f), bloomStep(20.0f)};
    const std::vector<SpriteHaloCluster>  joined = clusterSpriteHaloEmission(wide, footprints);
    ASSERT_EQ(joined.size(), 1u);
    EXPECT_EQ(joined[0].members, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(joined[0].box, box(10, 10, 72, 16));
}

TEST(SpriteHaloField, AChainOfOverlapsIsOneCluster) {
    // A reaches B and B reaches C, while A and C are 24 px apart, past the 18 px two margins span.
    const std::vector<SpriteEmissionStep> steps{bloomStep(6.0f), bloomStep(6.0f), bloomStep(6.0f)};
    const std::vector<PixelBox> footprints{box(0, 0, 16, 16), box(20, 0, 16, 16), box(40, 0, 16, 16)};

    const std::vector<SpriteHaloCluster> clusters = clusterSpriteHaloEmission(steps, footprints);

    ASSERT_EQ(clusters.size(), 1u);
    EXPECT_EQ(clusters[0].members, (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_EQ(clusters[0].box, box(0, 0, 56, 16));
}

TEST(SpriteHaloField, DifferentKindsOrReachesNeverShareACluster) {
    const std::vector<PixelBox> sameplace{box(10, 10, 16, 16), box(10, 10, 16, 16)};

    const std::vector<SpriteEmissionStep> kinds{bloomStep(6.0f), glowStep(6.0f)};
    const std::vector<SpriteHaloCluster>  byKind = clusterSpriteHaloEmission(kinds, sameplace);
    ASSERT_EQ(byKind.size(), 2u);
    EXPECT_FALSE(byKind[0].glow);
    EXPECT_EQ(byKind[0].members, (std::vector<std::size_t>{0}));
    EXPECT_TRUE(byKind[1].glow);
    EXPECT_EQ(byKind[1].members, (std::vector<std::size_t>{1}));

    const std::vector<SpriteEmissionStep> reaches{bloomStep(6.0f), bloomStep(6.5f)};
    const std::vector<SpriteHaloCluster>  byReach = clusterSpriteHaloEmission(reaches, sameplace);
    ASSERT_EQ(byReach.size(), 2u);
    EXPECT_EQ(byReach[0].blurReach, 6.0f);
    EXPECT_EQ(byReach[0].members, (std::vector<std::size_t>{0}));
    EXPECT_EQ(byReach[1].blurReach, 6.5f);
    EXPECT_EQ(byReach[1].members, (std::vector<std::size_t>{1}));
}

TEST(SpriteHaloField, AnEmptyFootprintJoinsNoCluster) {
    const std::vector<SpriteEmissionStep> steps{bloomStep(6.0f), bloomStep(6.0f), bloomStep(6.0f)};
    const std::vector<PixelBox>           footprints{box(10, 10, 16, 16), PixelBox{}, box(30, 10, 16, 16)};

    const std::vector<SpriteHaloCluster> clusters = clusterSpriteHaloEmission(steps, footprints);

    ASSERT_EQ(clusters.size(), 1u);
    EXPECT_EQ(clusters[0].members, (std::vector<std::size_t>{0, 2}));
    EXPECT_EQ(clusters[0].box, box(10, 10, 36, 16));
}

// ── How large a field is ────────────────────────────────────────────────────────────────────

TEST(SpriteHaloField, TheDemandGrowsTheBoxByTheSpreadAndCarriesIt) {
    // Reach 6 blurs at full resolution: the spread is the reach plus one pixel.
    const SpriteHaloCluster fine{.glow = false, .blurReach = 6.0f, .box = box(40, 24, 32, 16), .members = {0}};
    const EmissionDemand    d6 = spriteHaloDemand(fine);
    EXPECT_EQ(d6.x, 33);
    EXPECT_EQ(d6.y, 17);
    EXPECT_EQ(d6.w, 46);
    EXPECT_EQ(d6.h, 30);
    EXPECT_EQ(d6.reach, 7.0f);

    // Reach 12 blurs reduced: the spread is the reach plus the reduction factor, 16.
    const SpriteHaloCluster wide{.glow = true, .blurReach = 12.0f, .box = box(40, 24, 32, 16), .members = {0}};
    const EmissionDemand    d12 = spriteHaloDemand(wide);
    EXPECT_EQ(d12.x, 24);
    EXPECT_EQ(d12.y, 8);
    EXPECT_EQ(d12.w, 64);
    EXPECT_EQ(d12.h, 48);
    EXPECT_EQ(d12.reach, 16.0f);
}

// ── Where the composite draws ───────────────────────────────────────────────────────────────

TEST(SpriteHaloField, TheCompositeRecordMapsTheQuadToComposeUv) {
    const EmissionDemand d{.x = 10, .y = 20, .w = 30, .h = 40, .reach = 7.0f};

    const GpuHaloComposite one = haloCompositeRecord(d, 5, false, 1, 160, 144);
    EXPECT_FLOAT_EQ(one.uvBox[0], 10.0f / 160.0f);
    EXPECT_FLOAT_EQ(one.uvBox[1], 20.0f / 144.0f);
    EXPECT_FLOAT_EQ(one.uvBox[2], 40.0f / 160.0f);
    EXPECT_FLOAT_EQ(one.uvBox[3], 60.0f / 144.0f);
    EXPECT_EQ(one.rowGlow[0], 5.0f);
    EXPECT_EQ(one.rowGlow[1], 0.0f);
    EXPECT_EQ(one.rowGlow[2], 0.0f);
    EXPECT_EQ(one.rowGlow[3], 0.0f);

    // At scale 2 the quad's viewport pixels are doubled onto a compose grid twice as wide.
    const GpuHaloComposite two = haloCompositeRecord(d, 9, true, 2, 320, 288);
    EXPECT_FLOAT_EQ(two.uvBox[0], 20.0f / 320.0f);
    EXPECT_FLOAT_EQ(two.uvBox[1], 40.0f / 288.0f);
    EXPECT_FLOAT_EQ(two.uvBox[2], 80.0f / 320.0f);
    EXPECT_FLOAT_EQ(two.uvBox[3], 120.0f / 288.0f);
    EXPECT_EQ(two.rowGlow[0], 9.0f);
    EXPECT_EQ(two.rowGlow[1], 1.0f);
}

// ── What the art record carries ─────────────────────────────────────────────────────────────

TEST(SpriteHaloField, AHaloSeatsNothingIntoTheArtRecord) {
    Sprite s{.key = "lamp"};
    s.x       = 40;
    s.y       = 24;
    s.size    = AssetDimensions{.width = 16, .height = 16};
    s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 6.0f, .intensity = 255}};

    std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_FALSE(recs.empty());

    // The layer's collection and plan for this sprite's halo.
    const std::vector<SpriteEmissionStep> steps = spriteEmissionSteps(s, spriteLinearScale(s, Transform{}));
    ASSERT_EQ(steps.size(), 1u);
    const GpuSprite                      rec = makeGpuSprite(s, 160, 144, 0.0f, 0.0f);
    const std::vector<PixelBox>          footprints{spriteFootprint(rec, 160, 144)};
    const std::vector<SpriteHaloCluster> clusters = clusterSpriteHaloEmission(steps, footprints);
    ASSERT_EQ(clusters.size(), 1u);
    const EmissionDemand demand = spriteHaloDemand(clusters[0]);
    EXPECT_FALSE(demand.w <= 0 || demand.h <= 0);

    const std::vector<SpriteFxRecord> fresh = buildSpriteFxRecords(s);
    ASSERT_EQ(recs.size(), fresh.size());
    EXPECT_EQ(std::memcmp(recs.data(), fresh.data(), recs.size() * sizeof(SpriteFxRecord)), 0);
}
