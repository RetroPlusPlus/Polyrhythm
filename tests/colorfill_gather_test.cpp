#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// ColorFill gathering (postprocess.h): the pure CPU side of the built-in ColorFill fast path that
// collapses N ColorFill-confined regions into ONE render pass. Three device-free units, tested here
// headlessly (the renderer feeds them and consumes their output):
//   colorFillGatherEligible    — the routing predicate (deliberately wider than the custom predicates:
//                                alpha / blend / invert / stroke / transform all ride the record)
//   colorFillGatherStrideFloat4s + colorFillGatherRecordBytes — the per-run stride and the per-region
//                                record layout the colorfill_gather shader indexes (pinned per lane)
//   groupGatherRuns            — the existing grouping over the reserved kColorFillGatherStage id
//                                (contiguity + the ≥2 rule apply unchanged; a custom stage or any
//                                ineligible step is a hard run boundary)

namespace retropp {
namespace {

constexpr int kW = 160;  // viewport 160×144 → compose grid at scale 1
constexpr int kH = 144;

ScreenSpaceEffect fillEffect(Rgba8 colour = Rgba8{255, 140, 0, 255}) {
    ScreenSpaceEffect e;
    e.kind = ScreenSpaceEffectKind::ColorFill;
    e.fill = colour;
    return e;
}

// Read one float lane out of a packed record.
float lane(const std::vector<std::byte>& bytes, std::size_t floatIndex) {
    float v = 0.0f;
    std::memcpy(&v, bytes.data() + floatIndex * sizeof(float), sizeof(float));
    return v;
}

// ── Eligibility: every fill kind qualifies; only curve boundaries (and non-ColorFill kinds) do not ──

TEST(ColorFillGather, RectangleEligible) {
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), ShapePoints::rectangle({8, 6}, 40, 12)));
}

TEST(ColorFillGather, CircleAndTriangleEligible) {
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), ShapePoints::circle({80, 72}, 20)));
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), ShapePoints::triangle({10, 10}, {50, 10}, {10, 40})));
}

TEST(ColorFillGather, InvertStrokeTransformEligible) {
    // The record carries the full gate state, so none of these fall back to the per-region path.
    ShapePoints s = ShapePoints::rectangle({8, 6}, 40, 12);
    s.invert      = true;
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), s));
    s.invert      = false;
    s.strokeWidth = 3.0f;
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), s));
    s.strokeWidth = 0.0f;
    s.transform   = Transform::rotation(30.0f, 28.0f, 12.0f);
    EXPECT_TRUE(colorFillGatherEligible(fillEffect(), s));
}

TEST(ColorFillGather, NonColorFillKindsIneligible) {
    ScreenSpaceEffect custom;
    custom.kind = ScreenSpaceEffectKind::Custom;
    EXPECT_FALSE(colorFillGatherEligible(custom, ShapePoints::rectangle({8, 6}, 40, 12)));
    ScreenSpaceEffect gleam;
    gleam.kind = ScreenSpaceEffectKind::Gleam;
    EXPECT_FALSE(colorFillGatherEligible(gleam, ShapePoints::rectangle({8, 6}, 40, 12)));
}

TEST(ColorFillGather, CurveBoundaryIneligible) {
    ShapePoints s = ShapePoints::rectangle({20, 20}, 40, 40);
    s.curve       = {CurveSegment{.p0 = {20, 72}, .p1 = {40, 52}, .p2 = {60, 72},
                                  .degree = CurveDegree::Quadratic}};
    EXPECT_FALSE(colorFillGatherEligible(fillEffect(), s));
}

TEST(ColorFillGather, EmptyShapeIneligible) {
    EXPECT_FALSE(colorFillGatherEligible(fillEffect(), ShapePoints{}));
}

// ── Stride: 6 header float4s + two vertices per float4, capped at the region cbuffer's 64 ─────────

TEST(ColorFillGather, StrideByVertexCount) {
    EXPECT_EQ(colorFillGatherStrideFloat4s(1), 7u);    // circle: one vertex → one half-filled float4
    EXPECT_EQ(colorFillGatherStrideFloat4s(2), 7u);    // capsule packs into the same float4
    EXPECT_EQ(colorFillGatherStrideFloat4s(4), 8u);    // rectangle
    EXPECT_EQ(colorFillGatherStrideFloat4s(64), 38u);  // the cap
    EXPECT_EQ(colorFillGatherStrideFloat4s(100), 38u); // beyond the cap truncates to 64
}

// ── Record layout: every lane pinned against the shader's indexing ────────────────────────────────

TEST(ColorFillGather, RectangleRecordLanes) {
    const ShapePoints shape = ShapePoints::rectangle({8, 6}, 40, 12);
    const ScreenSpaceEffect eff = fillEffect(Rgba8{255, 140, 0, 255});
    const RegionBatchInstance in = regionBatchInstance(shape, 1, kW, kH);
    const std::uint32_t stride = colorFillGatherStrideFloat4s(shape.points.size());
    const std::vector<std::byte> rec = colorFillGatherRecordBytes(
        shape, eff, /*regionAlpha=*/1.0f, BlendMode::Normal, PixelSize{kW, kH}, in, kW, kH, stride);
    ASSERT_EQ(rec.size(), static_cast<std::size_t>(stride) * 16u);

    // float4 0 — the covering quad px→uv (regionScissorRect through regionBatchInstance, the single
    // box authority).
    const IntRect box = regionScissorRect(shape, 1, kW, kH);
    EXPECT_FLOAT_EQ(lane(rec, 0), static_cast<float>(box.x) / kW);
    EXPECT_FLOAT_EQ(lane(rec, 1), static_cast<float>(box.y) / kH);
    EXPECT_FLOAT_EQ(lane(rec, 2), static_cast<float>(box.x + box.width) / kW);
    EXPECT_FLOAT_EQ(lane(rec, 3), static_cast<float>(box.y + box.height) / kH);

    // float4 1 — colorFillParams (normalized × fillIntensity) + the region alpha.
    const ColorFillParams cf = colorFillParams(eff);
    EXPECT_FLOAT_EQ(lane(rec, 4), cf.r);
    EXPECT_FLOAT_EQ(lane(rec, 5), cf.g);
    EXPECT_FLOAT_EQ(lane(rec, 6), cf.b);
    EXPECT_FLOAT_EQ(lane(rec, 7), 1.0f);

    // float4s 2–4 — the identity inverse homography, invert 0 / stroke 0 / blend Normal in the w lanes.
    EXPECT_FLOAT_EQ(lane(rec, 8), 1.0f);   EXPECT_FLOAT_EQ(lane(rec, 9), 0.0f);
    EXPECT_FLOAT_EQ(lane(rec, 10), 0.0f);  EXPECT_FLOAT_EQ(lane(rec, 11), 0.0f);  // invert flag
    EXPECT_FLOAT_EQ(lane(rec, 12), 0.0f);  EXPECT_FLOAT_EQ(lane(rec, 13), 1.0f);
    EXPECT_FLOAT_EQ(lane(rec, 14), 0.0f);  EXPECT_FLOAT_EQ(lane(rec, 15), 0.0f);  // strokeWidth
    EXPECT_FLOAT_EQ(lane(rec, 16), 0.0f);  EXPECT_FLOAT_EQ(lane(rec, 17), 0.0f);
    EXPECT_FLOAT_EQ(lane(rec, 18), 1.0f);  EXPECT_FLOAT_EQ(lane(rec, 19), 0.0f);  // blend mode

    // float4 5 — vertex count + radius.
    EXPECT_FLOAT_EQ(lane(rec, 20), 4.0f);
    EXPECT_FLOAT_EQ(lane(rec, 21), 0.0f);

    // float4s 6–7 — the four rectangle vertices, two per float4.
    for (std::size_t i = 0; i < shape.points.size(); ++i) {
        EXPECT_FLOAT_EQ(lane(rec, 24 + 2 * i), shape.points[i].x) << "vertex " << i;
        EXPECT_FLOAT_EQ(lane(rec, 24 + 2 * i + 1), shape.points[i].y) << "vertex " << i;
    }
}

TEST(ColorFillGather, FlagLanesCarryInvertStrokeBlendAlpha) {
    ShapePoints shape = ShapePoints::rectangle({8, 6}, 40, 12);
    shape.invert      = true;
    shape.strokeWidth = 3.0f;
    const RegionBatchInstance in = regionBatchInstance(shape, 1, kW, kH);
    const std::uint32_t stride = colorFillGatherStrideFloat4s(shape.points.size());
    const std::vector<std::byte> rec = colorFillGatherRecordBytes(
        shape, fillEffect(), /*regionAlpha=*/0.5f, BlendMode::Multiply, PixelSize{kW, kH}, in, kW, kH,
        stride);
    EXPECT_FLOAT_EQ(lane(rec, 7), 0.5f);                                     // region alpha
    EXPECT_FLOAT_EQ(lane(rec, 11), 1.0f);                                    // invert flag
    EXPECT_FLOAT_EQ(lane(rec, 15), 3.0f);                                    // strokeWidth
    EXPECT_FLOAT_EQ(lane(rec, 19), static_cast<float>(BlendMode::Multiply)); // blend mode
}

TEST(ColorFillGather, RecordPadsToRunStride) {
    // A run's stride comes from its LARGEST shape; a smaller record zero-pads its vertex tail.
    const ShapePoints small = ShapePoints::circle({80, 72}, 10);
    const RegionBatchInstance in = regionBatchInstance(small, 1, kW, kH);
    const std::uint32_t runStride = colorFillGatherStrideFloat4s(4);  // sized by a rectangle peer
    const std::vector<std::byte> rec = colorFillGatherRecordBytes(
        small, fillEffect(), 1.0f, BlendMode::Normal, PixelSize{kW, kH}, in, kW, kH, runStride);
    ASSERT_EQ(rec.size(), static_cast<std::size_t>(runStride) * 16u);
    EXPECT_FLOAT_EQ(lane(rec, 20), 1.0f);   // count: the circle's one vertex
    EXPECT_FLOAT_EQ(lane(rec, 21), 10.0f);  // radius
    EXPECT_FLOAT_EQ(lane(rec, 24), 80.0f);  // the center vertex
    EXPECT_FLOAT_EQ(lane(rec, 25), 72.0f);
    for (std::size_t f = 26; f < runStride * 4u; ++f) {
        EXPECT_FLOAT_EQ(lane(rec, f), 0.0f) << "pad lane " << f;
    }
}

TEST(ColorFillGather, LongPolygonTruncatesAtTheCbufferCap) {
    ShapePoints shape;
    for (int i = 0; i < 100; ++i) {
        shape.points.push_back(Point{static_cast<float>(10 + i), static_cast<float>(20 + (i % 7))});
    }
    const RegionBatchInstance in = regionBatchInstance(shape, 1, kW, kH);
    const std::uint32_t stride = colorFillGatherStrideFloat4s(shape.points.size());
    ASSERT_EQ(stride, 38u);
    const std::vector<std::byte> rec = colorFillGatherRecordBytes(
        shape, fillEffect(), 1.0f, BlendMode::Normal, PixelSize{kW, kH}, in, kW, kH, stride);
    ASSERT_EQ(rec.size(), static_cast<std::size_t>(stride) * 16u);
    EXPECT_FLOAT_EQ(lane(rec, 20), 64.0f);  // the effective (post-truncation) count — matches the
                                            // per-region cbuffer's truncation, so both paths render
                                            // the same shape
    EXPECT_FLOAT_EQ(lane(rec, 24 + 2 * 63), shape.points[63].x);      // the last kept vertex
    EXPECT_FLOAT_EQ(lane(rec, 24 + 2 * 63 + 1), shape.points[63].y);
}

// ── Grouping over the reserved stage id: the existing rules apply unchanged ───────────────────────

TEST(ColorFillGather, SentinelStageGroupsAndCustomStageBounds) {
    // [fill, fill, custom, fill] — the two leading fills gather; the custom step (a different stage)
    // ends the run; the trailing fill is a singleton and stays on the per-region path.
    std::vector<GatherStep> keys(4);
    keys[0] = {true, kColorFillGatherStage};
    keys[1] = {true, kColorFillGatherStage};
    keys[2] = {true, 3u};
    keys[3] = {true, kColorFillGatherStage};
    const GatherGrouping g = groupGatherRuns(keys);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].stage, kColorFillGatherStage);
    EXPECT_EQ(g.runs[0].steps, (std::vector<std::uint32_t>{0, 1}));
    EXPECT_EQ(g.stepRun, (std::vector<int>{0, 0, -1, -1}));
}

TEST(ColorFillGather, ContiguousFillListFormsOneRun) {
    // The input_probe shape: a frame-level list of ~30 contiguous ColorFill regions is ONE pass.
    std::vector<GatherStep> keys(30, GatherStep{true, kColorFillGatherStage});
    const GatherGrouping g = groupGatherRuns(keys);
    ASSERT_EQ(g.runs.size(), 1u);
    EXPECT_EQ(g.runs[0].steps.size(), 30u);
    for (int r : g.stepRun) EXPECT_EQ(r, 0);
}

}  // namespace
}  // namespace retropp
