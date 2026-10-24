#include "retropp/draw_state.h"
#include "retropp/postprocess.h"

#include <gtest/gtest.h>

#include "retropp/curve.h"
#include "retropp/geometry.h"
#include "retropp/transform.h"

// ShapePoints::contains — the game-facing point test on the shape type every Region draws. It routes
// through the region gate's CPU mirrors (curveRegionContains / regionContains), so a drawn shape and this
// test agree by construction; the one deliberate departure is the empty shape, which contains NOTHING
// (the free-function mirrors keep their applies-everywhere default — an effect with no region covers the
// frame). Device-free: every case is pure CPU geometry, and each shape field the gate honors — radius,
// strokeWidth, transform, invert, a curve boundary — is exercised through the member verb.

namespace retropp {
namespace {

TEST(ShapePointsContains, EmptyShapeContainsNothing) {
    const ShapePoints empty{};
    EXPECT_FALSE(empty.contains(Point{0.0f, 0.0f}));
    EXPECT_FALSE(empty.contains(Point{80.0f, 72.0f}));
    // The free-function mirror keeps the opposite default — an effect with no region applies everywhere.
    // The member verb is a mask test; the mirror is the gate. Both are correct for their job.
    EXPECT_TRUE(regionContains(Point{80.0f, 72.0f}, empty));
}

TEST(ShapePointsContains, RadiusInflatesTheContainedDisc) {
    const ShapePoints disc = ShapePoints::circle(Point{80.0f, 72.0f}, 10.0f);
    EXPECT_TRUE(disc.contains(Point{80.0f, 72.0f}));    // center
    EXPECT_TRUE(disc.contains(Point{89.0f, 72.0f}));    // just inside the rim
    EXPECT_FALSE(disc.contains(Point{92.0f, 72.0f}));   // just past it
    EXPECT_FALSE(disc.contains(Point{140.0f, 72.0f}));  // far outside
}

TEST(ShapePointsContains, StrokeConfinesToTheBoundaryBand) {
    ShapePoints box = ShapePoints::rectangle(Point{40.0f, 40.0f}, 60.0f, 40.0f);
    ASSERT_TRUE(box.contains(Point{70.0f, 60.0f}));  // filled: the interior is in
    box.strokeWidth = 4.0f;
    EXPECT_FALSE(box.contains(Point{70.0f, 60.0f}));  // stroked: the deep interior is out
    EXPECT_TRUE(box.contains(Point{70.0f, 41.0f}));   // within ±2 px of the top edge
    EXPECT_TRUE(box.contains(Point{70.0f, 39.0f}));   // the band straddles the boundary
    EXPECT_FALSE(box.contains(Point{70.0f, 20.0f}));  // far outside
}

TEST(ShapePointsContains, InvertFlipsInsideAndOutside) {
    ShapePoints box = ShapePoints::rectangle(Point{40.0f, 40.0f}, 60.0f, 40.0f);
    const Point inside{70.0f, 60.0f};
    const Point outside{10.0f, 10.0f};
    ASSERT_TRUE(box.contains(inside));
    ASSERT_FALSE(box.contains(outside));
    box.invert = true;
    EXPECT_FALSE(box.contains(inside));  // the inside is now the OUTSIDE
    EXPECT_TRUE(box.contains(outside));
}

TEST(ShapePointsContains, TransformMapsThePointBackThroughItsInverse) {
    ShapePoints box = ShapePoints::rectangle(Point{40.0f, 40.0f}, 60.0f, 40.0f);
    const Point atRest{70.0f, 60.0f};
    const Point atMoved{150.0f, 60.0f};
    ASSERT_TRUE(box.contains(atRest));
    ASSERT_FALSE(box.contains(atMoved));
    box.transform = Transform::translation(80.0f, 0.0f);
    EXPECT_TRUE(box.contains(atMoved));  // the moved shape is hit where it now sits
    EXPECT_FALSE(box.contains(atRest));  // and no longer where it was authored
}

TEST(ShapePointsContains, ACurveBoundaryRoutesThroughTheCurvePath) {
    // A closed quadratic diamond about (80, 72) — `points` stays EMPTY, so a dispatch that ignored the
    // curve would fall into the empty branch and answer false everywhere (or, forwarded to the polygon
    // mirror unguarded, true everywhere). The curve path answers the geometry.
    Curve diamond = Curve::quadratic(Vec2{80.0f, 42.0f}, Vec2{110.0f, 42.0f}, Vec2{110.0f, 72.0f});
    diamond.quadraticTo(Vec2{110.0f, 102.0f}, Vec2{80.0f, 102.0f})
        .quadraticTo(Vec2{50.0f, 102.0f}, Vec2{50.0f, 72.0f})
        .quadraticTo(Vec2{50.0f, 42.0f}, Vec2{80.0f, 42.0f});
    diamond.closed = true;
    const ShapePoints shape = ShapePoints::fromCurve(diamond);
    ASSERT_TRUE(shape.points.empty());
    EXPECT_TRUE(shape.contains(Point{80.0f, 72.0f}));   // center
    EXPECT_FALSE(shape.contains(Point{10.0f, 10.0f}));  // far outside
    EXPECT_FALSE(shape.contains(Point{52.0f, 44.0f}));  // inside the corner's bounding box, outside the arc
}

}  // namespace
}  // namespace retropp
