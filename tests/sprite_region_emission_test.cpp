// Region-Bloom emission fields — the graded-source path's CPU half.
//
// A Bloom inside a Layer-scope sprite REGION is the region's source color, not an additive halo, so it
// cannot ride a shared bucket the way a chain Bloom does: it needs THIS sprite's own light, and it gets a
// field of its own — a rect of the atlas sized to the sprite's footprint. These tests pin what the renderer
// builds from: which steps ask for a field, what each is given, and that the emission index a step carries
// addresses the RIGHT record in the sprite's packed slice however the sprite's chain and regions are
// authored.
//
// That last one is the whole hazard: the index is what the emission raster walks the color chain up to, so
// an index off by one rasters a different step's light. It is asserted against buildSpriteFxRecords' own
// output rather than against a hand-counted number, so the two cannot drift apart.
//
// The device-backed half (a region Bloom actually reading its field through a shape) lives in
// golden_readback_test.cpp.

#include <gtest/gtest.h>

#include <retropp/draw_state.h>
#include <retropp/emission_atlas.h>
#include <retropp/postprocess.h>

#include <cmath>
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

// A region carrying `effects` over a unit square — a shape the sprite path supports (no curve).
Region regionWith(std::string key, std::vector<ScreenSpaceEffect> effects) {
    return Region{.key     = std::move(key),
                  .shape   = ShapePoints{.points = {{0.0f, 0.0f}, {8.0f, 0.0f}, {8.0f, 8.0f}, {0.0f, 8.0f}}},
                  .effects = std::move(effects)};
}

constexpr PixelBox kQuad{.x = 32, .y = 16, .w = 32, .h = 32};

// One sprite's records, and what its region Blooms ask the atlas for.
struct Asked {
    std::vector<SpriteFxRecord>             records;
    std::vector<SpriteRegionEmissionDemand> demands;
};

Asked ask(const Sprite& s, float linearScale = 1.0f) {
    Asked out;
    out.records = buildSpriteFxRecords(s);
    out.demands = collectSpriteRegionEmissionDemands(out.records, linearScale, 0, kQuad);
    return out;
}

EmissionAtlasPlan planFor(const std::vector<SpriteRegionEmissionDemand>& demands, int maxSize = 1024) {
    std::vector<EmissionDemand> fields;
    fields.reserve(demands.size());
    for (const SpriteRegionEmissionDemand& d : demands) fields.push_back(d.field);
    return planEmissionAtlas(std::span<const EmissionDemand>{fields}, maxSize);
}

// Where a kind sits in a packed slice — the independent answer the emission index is checked against.
std::size_t recordIndexOf(std::span<const SpriteFxRecord> recs, ScreenSpaceEffectKind kind, bool isRegion,
                          std::size_t nth = 0) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const bool region = (recs[i].flags & kSpriteFxIsRegion) != 0u;
        if (recs[i].kind != static_cast<std::uint32_t>(kind) || region != isRegion) continue;
        if (seen++ == nth) return i;
    }
    return recs.size();  // not found — an assertion below will show it
}

}  // namespace

// ── The emission index ──────────────────────────────────────────────────────────────────────

TEST(SpriteRegionEmission, AStepsIndexIsItsOwnRecordInThePackedSlice) {
    // The simplest shape: no chain, one region Bloom. Index 0 is the only record there is.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(4.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(a.demands[0].recordIndex, recordIndexOf(a.records, ScreenSpaceEffectKind::Bloom, true));
}

TEST(SpriteRegionEmission, ChainStepsBeforeARegionShiftTheEmissionIndex) {
    // buildSpriteFxRecords packs the chain first, then the regions. A step that assumed the region was at
    // index 0 would raster the color chain up to the WRONG step — the whole reason this is asserted against
    // the packed slice rather than a literal.
    Sprite s{.key = "s"};
    s.effects = {fill(Rgba8{.r = 255, .g = 0, .b = 0, .a = 255}),
                 ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 128}};
    s.regions = {regionWith("rg", {bloom(4.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(a.demands[0].recordIndex, 2u);   // the two chain steps come first
    EXPECT_EQ(a.demands[0].recordIndex, recordIndexOf(a.records, ScreenSpaceEffectKind::Bloom, true));
}

TEST(SpriteRegionEmission, ARegionKindThePackerDropsDoesNotConsumeAnIndex) {
    // A Swirl / Glow / Custom / displacing kind inside a region is skipped at pack time (its params need the
    // lanes the region's shape occupies). It therefore occupies no record, and the Bloom that follows must
    // NOT be indexed as though it did.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Glow,
                                                     .radius    = 3.0f,
                                                     .intensity = 255},
                                   ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                                     .amplitude = 2.0f},
                                   bloom(4.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(a.records.size(), 1u);          // only the Bloom survived packing
    EXPECT_EQ(a.demands[0].recordIndex, 0u);
}

TEST(SpriteRegionEmission, AnEarlierRegionsOwnEffectsShiftTheIndexToo) {
    // Regions pack in order, each contributing all of its packable effects.
    Sprite s{.key = "s"};
    s.regions = {regionWith("first", {fill(Rgba8{.r = 0, .g = 0, .b = 255, .a = 255})}),
                 regionWith("second", {bloom(4.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(a.demands[0].recordIndex, 1u);
    EXPECT_EQ(a.demands[0].recordIndex, recordIndexOf(a.records, ScreenSpaceEffectKind::Bloom, true));
}

TEST(SpriteRegionEmission, ACurveBoundaryRegionContributesNoRecordAndNoField) {
    // A curve-boundary region is unsupported inline and skipped whole (the renderer warns) — so it neither
    // takes a field nor shifts the index of anything after it.
    Sprite s{.key = "s"};
    Region curved = regionWith("curved", {bloom(4.0f)});
    curved.shape.curve = {{0.0f, 0.0f}, {4.0f, 4.0f}, {8.0f, 0.0f}};
    s.regions = {curved, regionWith("plain", {bloom(4.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(a.demands[0].recordIndex, 0u);
}

// ── Which steps ask for a field ─────────────────────────────────────────────────────────────

TEST(SpriteRegionEmission, EachRegionBloomGetsItsOwnRectNeverASharedOne) {
    // Two region Blooms authored identically still take TWO fields: a field holds one sprite's own light,
    // and this one is consumed as a graded source, so sharing would mix a neighbor's light into it. They
    // share a PAGE — one reach, one blur pass — while keeping rects of their own.
    Sprite a{.key = "a"};
    a.regions = {regionWith("rg", {bloom(4.0f)})};
    Sprite b{.key = "b"};
    b.regions = {regionWith("rg", {bloom(4.0f)})};

    std::vector<SpriteFxRecord> store  = buildSpriteFxRecords(a);
    const std::size_t           base   = store.size();
    std::vector<SpriteFxRecord> second = buildSpriteFxRecords(b);
    store.insert(store.end(), second.begin(), second.end());

    std::vector<SpriteRegionEmissionDemand> demands =
        collectSpriteRegionEmissionDemands(std::span<SpriteFxRecord>{store}.first(base), 1.0f, 0, kQuad);
    for (const SpriteRegionEmissionDemand& d : collectSpriteRegionEmissionDemands(
             std::span<SpriteFxRecord>{store}.subspan(base), 1.0f, base, kQuad))
        demands.push_back(d);
    ASSERT_EQ(demands.size(), 2u);

    const EmissionAtlasPlan plan = planFor(demands);
    ASSERT_EQ(plan.pages.size(), 1u) << "one authored reach is one blur pass, however many fields share it";
    const EmissionPlacement& p0 = plan.placements[0];
    const EmissionPlacement& p1 = plan.placements[1];
    ASSERT_GE(p0.page, 0);
    ASSERT_GE(p1.page, 0);
    const bool disjoint = p0.rectX + p0.rectW <= p1.rectX || p1.rectX + p1.rectW <= p0.rectX ||
                          p0.rectY + p0.rectH <= p1.rectY || p1.rectY + p1.rectH <= p0.rectY;
    EXPECT_TRUE(disjoint) << "two fields sharing texels would mix one sprite's light into the other's";
}

TEST(SpriteRegionEmission, TheFieldIndexIsWrittenToTheRecordsSpareLane) {
    // params[3] carried the gather's invNorm; with the gather gone it carries the field index the fragment
    // looks its rect up by.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(4.0f)})};

    Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(seatPlannedRegionEmissionFields(a.records, a.demands, planFor(a.demands)), 0);
    EXPECT_FLOAT_EQ(a.records[a.demands[0].recordIndex].params[3], 0.0f);
}

TEST(SpriteRegionEmission, TheFieldIndexIsOffsetByWhereTheLayersFieldsBegin) {
    // A plan spans one layer; the rect table spans the frame. The record has to name the table row, so a
    // layer whose fields start part-way through the table adds that base.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(4.0f)})};

    Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_EQ(seatPlannedRegionEmissionFields(a.records, a.demands, planFor(a.demands), 7), 0);
    EXPECT_FLOAT_EQ(a.records[a.demands[0].recordIndex].params[3], 7.0f);
}

TEST(SpriteRegionEmission, AChainBloomAsksForNothingHere) {
    // A whole-silhouette Bloom rasters through the shared (kind, reach) bucket — a different path entirely.
    Sprite s{.key = "s"};
    s.effects = {bloom(4.0f)};

    EXPECT_TRUE(ask(s).demands.empty());
}

TEST(SpriteRegionEmission, ARegionBloomWithNoIntensityAsksForNothing) {
    // No strength is an identity: it costs no field, and the step grades with the pixel it started from.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(4.0f, 0, 0)})};

    const Asked a = ask(s);
    EXPECT_TRUE(a.demands.empty());
    ASSERT_EQ(a.records.size(), 1u);
    EXPECT_FLOAT_EQ(a.records[0].params[2], 0.0f);
    EXPECT_FLOAT_EQ(a.records[0].params[3], 0.0f);
}

TEST(SpriteRegionEmission, ARegionBloomAtZeroRadiusStillAsks) {
    // Bloom's identity gate is its intensity alone — at zero reach it still adds its own un-blurred
    // brightpass, so the step is realized and takes a field.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(0.0f)})};

    const Asked a = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);
    EXPECT_FLOAT_EQ(a.demands[0].blurReach, 0.0f);
}

// ── The reach ───────────────────────────────────────────────────────────────────────────────

TEST(SpriteRegionEmission, TheReachIsTheArtRadiusThroughThePlacementsLinearScale) {
    // The radius is authored in the sprite's own art pixels; the field blurs in viewport pixels. A sprite
    // drawn at double size radiates twice as far, which is what the art-space gather did too.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(5.0f)})};

    EXPECT_FLOAT_EQ(ask(s, 1.0f).demands[0].blurReach, 5.0f);
    EXPECT_FLOAT_EQ(ask(s, 2.0f).demands[0].blurReach, 10.0f);
    EXPECT_FLOAT_EQ(ask(s, 0.5f).demands[0].blurReach, 2.5f);
}

TEST(SpriteRegionEmission, AWiderReachAsksForAWiderRect) {
    // A rect is the footprint plus room for the light to spread, so reach is what makes one field cost more
    // atlas than another — the property that turns capacity into area rather than a count.
    Sprite near{.key = "near"};
    near.regions = {regionWith("rg", {bloom(2.0f)})};
    Sprite far{.key = "far"};
    far.regions = {regionWith("rg", {bloom(20.0f)})};

    const EmissionAtlasPlan a = planFor(ask(near).demands);
    const EmissionAtlasPlan b = planFor(ask(far).demands);
    EXPECT_GT(b.placements[0].rectW, a.placements[0].rectW);
    EXPECT_GT(b.placements[0].rectH, a.placements[0].rectH);
}

// ── Capacity ────────────────────────────────────────────────────────────────────────────────

TEST(SpriteRegionEmission, ManyMoreFieldsThanTheOldArrayCouldHoldAllFit) {
    // The point of the atlas: a layer's capacity is area, not a count. Sixty-four fields at one reach is
    // eight times what an eight-layer store could hold, and they all get rects.
    std::vector<SpriteRegionEmissionDemand> demands;
    std::vector<SpriteFxRecord>             store;
    for (int i = 0; i < 64; ++i) {
        Sprite s{.key = "s"};
        s.regions                        = {regionWith("rg", {bloom(4.0f)})};
        std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
        const std::size_t           base = store.size();
        store.insert(store.end(), recs.begin(), recs.end());
        for (const SpriteRegionEmissionDemand& d : collectSpriteRegionEmissionDemands(
                 std::span<SpriteFxRecord>{store}.subspan(base), 1.0f, base, kQuad))
            demands.push_back(d);
    }
    ASSERT_EQ(demands.size(), 64u);

    const EmissionAtlasPlan plan = planFor(demands, kEmissionAtlasMaxSize);
    EXPECT_EQ(plan.dropped, 0);
    EXPECT_EQ(seatPlannedRegionEmissionFields(store, demands, plan), 0);
    for (std::size_t i = 0; i < demands.size(); ++i)
        EXPECT_FLOAT_EQ(store[demands[i].storeIndex].params[3], static_cast<float>(i));
}

TEST(SpriteRegionEmission, PastTheAtlasAStepIsReportedAndAddsNoLight) {
    // Capacity is now an allocation property of the atlas, so overflow is reported to the caller (which logs
    // it with the layer's key) and the step is zeroed — never drawn wrong.
    Sprite s{.key = "s"};
    s.regions = {regionWith("rg", {bloom(4.0f)})};
    Asked a   = ask(s);
    ASSERT_EQ(a.demands.size(), 1u);

    const EmissionAtlasPlan plan = planFor(a.demands, 8);   // far too small for the footprint
    ASSERT_EQ(plan.placements[0].page, -1);

    EXPECT_EQ(seatPlannedRegionEmissionFields(a.records, a.demands, plan), 1);
    EXPECT_FLOAT_EQ(a.records[a.demands[0].recordIndex].params[2], 0.0f);
    EXPECT_FLOAT_EQ(a.records[a.demands[0].recordIndex].params[3], 0.0f);
}

TEST(SpriteRegionEmission, AtCapacityTheSameFieldIsDroppedEveryTime) {
    // A scene sitting exactly at the boundary must lose the SAME field each frame; a rotating loser would
    // show as a halo flickering on and off. The packer is a pure function of the demand list, and demands
    // follow packed-record order, so identical input drops identically.
    std::vector<SpriteRegionEmissionDemand> demands;
    std::vector<SpriteFxRecord>             store;
    for (int i = 0; i < 40; ++i) {
        Sprite s{.key = "s"};
        s.regions                        = {regionWith("rg", {bloom(6.0f)})};
        std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
        const std::size_t           base = store.size();
        store.insert(store.end(), recs.begin(), recs.end());
        for (const SpriteRegionEmissionDemand& d : collectSpriteRegionEmissionDemands(
                 std::span<SpriteFxRecord>{store}.subspan(base), 1.0f, base, kQuad))
            demands.push_back(d);
    }

    const EmissionAtlasPlan first  = planFor(demands, 256);
    const EmissionAtlasPlan second = planFor(demands, 256);
    ASSERT_GT(first.dropped, 0) << "the atlas must actually be at capacity for this to mean anything";
    ASSERT_EQ(first.placements.size(), second.placements.size());
    for (std::size_t i = 0; i < first.placements.size(); ++i) {
        EXPECT_EQ(first.placements[i].page >= 0, second.placements[i].page >= 0) << "field " << i;
        EXPECT_EQ(first.placements[i].rectX, second.placements[i].rectX) << "field " << i;
        EXPECT_EQ(first.placements[i].rectY, second.placements[i].rectY) << "field " << i;
    }
}

// ── Sheets ──────────────────────────────────────────────────────────────────────────────────

// The renderer's own strategy, isolated: plan, re-plan whatever did not fit onto the next sheet, and stop
// when nothing is left. A sheet's area bounds that sheet, never the layer, so no field count is a ceiling.
namespace {

struct Sheeted {
    std::vector<int>  sheetOf;
    int               sheets     = 0;
    int               unplaceable = 0;
};

Sheeted planOntoSheets(const std::vector<EmissionDemand>& all, int sheetSize) {
    Sheeted out;
    out.sheetOf.assign(all.size(), -1);
    std::vector<std::size_t> remaining(all.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
    while (!remaining.empty()) {
        std::vector<EmissionDemand> fields;
        for (const std::size_t i : remaining) fields.push_back(all[i]);
        const EmissionAtlasPlan  plan = planEmissionAtlas(std::span<const EmissionDemand>{fields}, sheetSize);
        std::vector<std::size_t> leftover;
        for (std::size_t k = 0; k < remaining.size(); ++k) {
            if (plan.placements[k].page < 0) { leftover.push_back(remaining[k]); continue; }
            out.sheetOf[remaining[k]] = out.sheets;
        }
        if (leftover.size() == remaining.size()) {
            out.unplaceable = static_cast<int>(leftover.size());
            break;
        }
        ++out.sheets;
        remaining = std::move(leftover);
    }
    return out;
}

std::vector<EmissionDemand> uniformDemands(int count, int side, float reach) {
    std::vector<EmissionDemand> d;
    for (int i = 0; i < count; ++i)
        d.push_back(EmissionDemand{.x = i * 3, .y = i * 5, .w = side, .h = side, .reach = reach});
    return d;
}

}  // namespace

TEST(SpriteRegionEmission, FieldsThatOverflowOneSheetLandOnTheNext) {
    // A sheet far too small for all of them: every field still gets a home, spread across sheets, and none is
    // dropped. This is what makes capacity memory rather than one texture's dimensions.
    const std::vector<EmissionDemand> all = uniformDemands(40, 40, 4.0f);

    const Sheeted s = planOntoSheets(all, 256);

    EXPECT_GT(s.sheets, 1) << "a 256-square sheet cannot hold forty 50-square rects — this must page";
    EXPECT_EQ(s.unplaceable, 0);
    for (std::size_t i = 0; i < all.size(); ++i)
        EXPECT_GE(s.sheetOf[i], 0) << "field " << i << " was left without a sheet";
}

TEST(SpriteRegionEmission, EverythingFittingOneSheetTakesOnlyOne) {
    const std::vector<EmissionDemand> all = uniformDemands(8, 16, 2.0f);

    const Sheeted s = planOntoSheets(all, 1024);

    EXPECT_EQ(s.sheets, 1);
    EXPECT_EQ(s.unplaceable, 0);
}

TEST(SpriteRegionEmission, AFieldBiggerThanAWholeSheetIsReportedNotRetriedForever) {
    // The one case paging cannot rescue: a single field wider than a sheet. It must be counted and abandoned,
    // because handing it a fresh sheet would loop without end.
    const std::vector<EmissionDemand> all = uniformDemands(3, 400, 4.0f);

    const Sheeted s = planOntoSheets(all, 256);

    EXPECT_EQ(s.sheets, 0);
    EXPECT_EQ(s.unplaceable, 3);
}

// ── The read ────────────────────────────────────────────────────────────────────────────────
//
// A field is stored per VIEWPORT pixel and read from the COMPOSE grid, so the read decides whether the halo
// comes back crisp or smooth. The shader mirrors emissionFieldSamplePoint; these cases pin that arithmetic,
// and above all the crisp identity: on the Viewport grid the point lands exactly on the anchor of the texel
// holding the fragment's cell, where a filtered tap returns the stored texel bit for bit.

namespace {

// A placed field: content 32×32 at atlas (100, 60), covering the viewport quad at (32, 16) — kQuad's origin.
EmissionRectEntry placedRect() {
    return EmissionRectEntry{.offsetX = 100.0f - 32.0f, .offsetY = 60.0f - 16.0f,
                             .innerX  = 100.0f,        .innerY  = 60.0f,
                             .innerW  = 32.0f,         .innerH  = 32.0f,
                             .sheet   = 0.0f};
}

// The anchor of the texel holding a fragment's viewport cell: floor(viewport position) + offset + 0.5.
Point cellTexelAnchor(const EmissionRectEntry& rect, Point fragPx, float composeScale) {
    return Point{std::floor(fragPx.x / composeScale) + rect.offsetX + 0.5f,
                 std::floor(fragPx.y / composeScale) + rect.offsetY + 0.5f};
}

}  // namespace

TEST(SpriteRegionEmissionRead, OnTheViewportGridThePointIsTheCellsTexelAnchor) {
    // The crisp identity, over compose scales and every sub-pixel offset within a cell: a linear tap at a
    // texel's exact anchor returns that texel unchanged, so this equality IS the byte-identical crisp image.
    const EmissionRectEntry rect = placedRect();
    for (const float scale : {1.0f, 2.0f, 3.0f, 4.0f}) {
        for (int step = 0; step < 8; ++step) {
            const float frac = static_cast<float>(step) / 8.0f;
            const Point frag{(40.0f + frac) * scale, (24.0f + frac) * scale};
            const Point p   = emissionFieldSamplePoint(rect, frag, scale, true);
            const Point ref = cellTexelAnchor(rect, frag, scale);
            EXPECT_FLOAT_EQ(p.x, ref.x) << "scale " << scale << " step " << step;
            EXPECT_FLOAT_EQ(p.y, ref.y) << "scale " << scale << " step " << step;
        }
    }
}

TEST(SpriteRegionEmissionRead, OnTheViewportGridEveryComposePixelOfOneCellReadsOnePoint) {
    // The crisp quantization, stated directly: a viewport cell magnified three times is one halo value, not a
    // gradient across it.
    const EmissionRectEntry rect = placedRect();
    const Point             a    = emissionFieldSamplePoint(rect, Point{120.5f, 72.5f}, 3.0f, true);
    const Point             b    = emissionFieldSamplePoint(rect, Point{121.5f, 73.5f}, 3.0f, true);
    const Point             c    = emissionFieldSamplePoint(rect, Point{122.5f, 74.5f}, 3.0f, true);

    EXPECT_FLOAT_EQ(a.x, b.x);
    EXPECT_FLOAT_EQ(a.x, c.x);
    EXPECT_FLOAT_EQ(a.y, b.y);
    EXPECT_FLOAT_EQ(a.y, c.y);
}

TEST(SpriteRegionEmissionRead, OnTheOutputGridThePointAdvancesWithTheFragment) {
    // The smooth read: the same three compose pixels now sample three places, in order, so the halo
    // reconstructs across them instead of stepping.
    const EmissionRectEntry rect = placedRect();
    const Point             a    = emissionFieldSamplePoint(rect, Point{120.5f, 72.5f}, 3.0f, false);
    const Point             b    = emissionFieldSamplePoint(rect, Point{121.5f, 73.5f}, 3.0f, false);
    const Point             c    = emissionFieldSamplePoint(rect, Point{122.5f, 74.5f}, 3.0f, false);

    EXPECT_LT(a.x, b.x);
    EXPECT_LT(b.x, c.x);
    EXPECT_LT(a.y, b.y);
    EXPECT_LT(b.y, c.y);
}

TEST(SpriteRegionEmissionRead, AtComposeScaleOneBothGridsGiveTheSamePoint) {
    // Native resolution: a compose pixel IS a viewport pixel, its center is already the cell's anchor, and
    // the grid stops being a distinction at all.
    const EmissionRectEntry rect = placedRect();
    for (int i = 0; i < 6; ++i) {
        const Point frag{40.5f + static_cast<float>(i), 24.5f + static_cast<float>(i)};
        const Point snapped = emissionFieldSamplePoint(rect, frag, 1.0f, true);
        const Point smooth  = emissionFieldSamplePoint(rect, frag, 1.0f, false);
        EXPECT_FLOAT_EQ(snapped.x, smooth.x);
        EXPECT_FLOAT_EQ(snapped.y, smooth.y);
    }
}

TEST(SpriteRegionEmissionRead, TheOffsetLandsAViewportPixelOnItsOwnTexel) {
    // The offset is the whole indirection: the quad's origin in viewport pixels reads the content's first
    // texel, wherever the packer put the rect.
    const EmissionRectEntry rect = placedRect();

    const Point p = emissionFieldSamplePoint(rect, Point{32.5f, 16.5f}, 1.0f, true);

    EXPECT_FLOAT_EQ(p.x, rect.innerX + 0.5f);
    EXPECT_FLOAT_EQ(p.y, rect.innerY + 0.5f);
}

TEST(SpriteRegionEmissionRead, ThePointIsQuantizedToTheReadsOwnSteps) {
    // Float noise on the way to an anchor is what would cost the crisp identity on one backend and not
    // another, so the point is held to a fixed grid. Every coordinate is a whole number of steps.
    const EmissionRectEntry rect = placedRect();
    for (int i = 1; i < 20; ++i) {
        const float scale = 1.0f + static_cast<float>(i) * 0.37f;
        const Point p     = emissionFieldSamplePoint(rect, Point{131.7f, 77.3f}, scale, false);
        EXPECT_FLOAT_EQ(p.x * kEmissionSampleSteps, std::floor(p.x * kEmissionSampleSteps));
        EXPECT_FLOAT_EQ(p.y * kEmissionSampleSteps, std::floor(p.y * kEmissionSampleSteps));
    }
}

TEST(SpriteRegionEmissionRead, TheContentBoxHoldsThePointWithOneRingTexelOfSupport) {
    // A tap at the quad's edge needs the ring outside the content box, and emissionMargin sizes the rect so
    // that ring is this field's own light. One texel out is as far as the read may go.
    const EmissionRectEntry rect = placedRect();

    const Point before = emissionFieldSamplePoint(rect, Point{-400.0f, -400.0f}, 1.0f, false);
    const Point after  = emissionFieldSamplePoint(rect, Point{4000.0f, 4000.0f}, 1.0f, false);

    EXPECT_FLOAT_EQ(before.x, rect.innerX - 0.5f);
    EXPECT_FLOAT_EQ(before.y, rect.innerY - 0.5f);
    EXPECT_FLOAT_EQ(after.x, rect.innerX + rect.innerW + 0.5f);
    EXPECT_FLOAT_EQ(after.y, rect.innerY + rect.innerH + 0.5f);
}

TEST(SpriteRegionEmissionRead, AZeroedRowCollapsesTheReadToOneTexel) {
    // What a step that named a field the atlas could not hold reads: the row is zeroes, so every point lands
    // in the single texel of the idle store — transparent — and the step adds no light rather than reading
    // somewhere arbitrary.
    const EmissionRectEntry idle{};

    for (const Point frag : {Point{0.5f, 0.5f}, Point{300.5f, 200.5f}, Point{-90.0f, 40.0f}}) {
        const Point p = emissionFieldSamplePoint(idle, frag, 1.0f, false);
        EXPECT_GE(p.x, -0.5f);
        EXPECT_LE(p.x, 0.5f);
        EXPECT_GE(p.y, -0.5f);
        EXPECT_LE(p.y, 0.5f);
    }
}
