// The sprite mask family — a sprite's image as a mask, in three forms (mask borrow / freezeMask /
// maskShape geometry) across two spaces (Quad / Layer).
//
// Two layers of coverage:
//   * Headless — the trace core, the containment invariant, and the coverage/orientation/placement maps are
//     pure CPU. traceSilhouette runs on a hand-built ArtMask; the map math is exercised through a
//     directly-built FrozenSpriteMask (it owns its mask, so it needs no renderer).
//   * Device-backed — the three Sprite verbs resolve a sprite's coverage from its `atlas` against the
//     engine renderer's uploaded pixels (Renderer::instance()), so they are exercised on a real GPU device
//     (a software rasterizer in CI), mirroring the golden harness's bootstrap.
//
// The load-bearing invariant is CONTAINMENT: a Conservative maskShape() polygon covers every visible art
// pixel at every point budget. The suite rasterizes each polygon and asserts coverage per pixel.

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"       // ShapeTrace, TransparentIndices
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/sprite_mask.h"
#include "retropp/transform.h"
#include "retropp/viewport.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace retropp {
namespace {

// Build an ArtMask from ASCII rows ('#' = visible, anything else = hole).
ArtMask maskFromRows(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    const int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;
    ArtMask m{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h, 0)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (rows[y][static_cast<std::size_t>(x)] == '#')
                m.visible[static_cast<std::size_t>(y) * w + x] = 1;
    return m;
}

// A FrozenSpriteMask reading `mask` under an orientation and placement — the headless stand-in for the
// coverage/orientation/placement maps a live SpriteMask runs (a live borrow reads the same math off the
// renderer's pixels; the frozen form owns its mask, so it needs no device).
FrozenSpriteMask frozenOf(const ArtMask& mask, Space space, Rotation rot = Rotation::None,
                           bool flipX = false, bool flipY = false, Transform quadToLayer = {}) {
    return FrozenSpriteMask{.mask = mask, .rotation = rot, .flipX = flipX, .flipY = flipY,
                             .space = space, .quadToLayer = quadToLayer};
}

// Even-odd point-in-polygon; a point on an edge counts as inside (containment must not fail on a boundary).
bool inPolygon(Point p, const std::vector<Point>& poly) {
    const std::size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& a = poly[i];
        const Point& b = poly[j];
        const bool straddles = (a.y > p.y) != (b.y > p.y);
        if (straddles) {
            const float xCross = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x <= xCross + 1e-4f) inside = !inside;
        }
    }
    return inside;
}

// Every visible pixel's center lies inside the polygon (the containment proof).
bool coversAll(const std::vector<Point>& poly, const ArtMask& mask) {
    for (int y = 0; y < mask.height; ++y)
        for (int x = 0; x < mask.width; ++x)
            if (mask.at(x, y) &&
                !inPolygon(Point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f}, poly))
                return false;
    return true;
}

// The furthest a polygon vertex sits from the nearest visible pixel center — a proxy for how far the
// boundary bulges out. Conservative apexes bulge; Balanced vertices ride the true contour.
double maxVertexSlack(const std::vector<Point>& poly, const ArtMask& mask) {
    double worst = 0.0;
    for (const Point& v : poly) {
        double nearest = std::numeric_limits<double>::max();
        for (int y = 0; y < mask.height; ++y)
            for (int x = 0; x < mask.width; ++x)
                if (mask.at(x, y)) {
                    const double dx = v.x - (x + 0.5), dy = v.y - (y + 0.5);
                    nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
                }
        worst = std::max(worst, nearest);
    }
    return worst;
}

const std::vector<std::string> kFull8 = {
    "########", "########", "########", "########",
    "########", "########", "########", "########"};
const std::vector<std::string> kL16 = {
    "########........", "########........", "########........", "########........",
    "########........", "########........", "########........", "########........",
    "################", "################", "################", "################",
    "################", "################", "################", "################"};
const std::vector<std::string> kDonut8 = {
    "########", "########", "##....##", "##....##",
    "##....##", "##....##", "########", "########"};
const std::vector<std::string> kTwoBlob16 = {
    "###.........###.", "###.........###.", "###.........###.", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................"};

// ── Trace shapes ──────────────────────────────────────────────────────────────────────────────

TEST(TraceSilhouette, EmptyMaskIsEmptyShape) {
    EXPECT_TRUE(traceSilhouette(ArtMask{}, 16).empty());
    EXPECT_TRUE(traceSilhouette(maskFromRows({"........", "........"}), 16).empty());
}

TEST(TraceSilhouette, FullTileIsAFourPointRectAtGenerousBudgets) {
    const ArtMask m = maskFromRows(kFull8);
    for (int budget : {4, 8, 16, 64}) {
        const auto poly = traceSilhouette(m, budget);
        EXPECT_EQ(poly.size(), 4u) << "budget " << budget;
        EXPECT_TRUE(coversAll(poly, m));
    }
}

TEST(TraceSilhouette, SinglePixel) {
    const ArtMask m = maskFromRows({"........", "...#....", "........"});
    const auto poly = traceSilhouette(m, 8);
    EXPECT_EQ(poly.size(), 4u);
    EXPECT_TRUE(coversAll(poly, m));
}

TEST(TraceSilhouette, BudgetBelowThreeThrows) {
    EXPECT_THROW((void)traceSilhouette(maskFromRows(kFull8), 2), std::invalid_argument);
    EXPECT_THROW((void)traceSilhouette(maskFromRows(kFull8), 0), std::invalid_argument);
}

// ── Containment golden — Conservative covers every visible pixel at every budget ────────────────

TEST(Containment, ConservativeCoversAllPixels) {
    struct Case { const char* name; std::vector<std::string> rows; };
    const std::vector<Case> cases = {
        {"full", kFull8}, {"L", kL16}, {"donut", kDonut8}, {"twoBlob", kTwoBlob16}};
    for (const Case& c : cases) {
        const ArtMask m = maskFromRows(c.rows);
        for (int budget : {3, 4, 8, 16, 64}) {
            const auto poly = traceSilhouette(m, budget, ShapeTrace::Conservative);
            EXPECT_TRUE(coversAll(poly, m)) << c.name << " @ budget " << budget;
            // The bounding box (4 corners) is the coarsest Conservative form, so a budget below 4 still
            // returns the box — the cap is max(budget, 4).
            EXPECT_LE(static_cast<int>(poly.size()), std::max(budget, 4)) << c.name << " @ budget " << budget;
            EXPECT_GE(poly.size(), 3u) << c.name << " @ budget " << budget;
        }
    }
}

TEST(Containment, DonutHoleIsBridgedNoInteriorBoundary) {
    // Outer boundary only: the hole is filled, so the shape is a single loop covering the ring AND the hole.
    const ArtMask m = maskFromRows(kDonut8);
    const auto poly = traceSilhouette(m, 32);
    EXPECT_TRUE(coversAll(poly, m));
    EXPECT_TRUE(inPolygon(Point{4.0f, 4.0f}, poly));  // the hole center is inside the bridged shape
}

TEST(Containment, TwoBlobsBecomeOneSpanningShape) {
    const ArtMask m = maskFromRows(kTwoBlob16);
    const auto poly = traceSilhouette(m, 32);
    EXPECT_TRUE(coversAll(poly, m));
    // The gap between the blobs is spanned (the merged shape is one polygon over both).
    EXPECT_TRUE(inPolygon(Point{7.5f, 1.5f}, poly));
}

// ── Balanced bound — never bulges further out than Conservative at the same budget ──────────────

TEST(Balanced, HugsNoLooserThanConservative) {
    for (const auto& rows : {kL16, kDonut8, kTwoBlob16}) {
        const ArtMask m = maskFromRows(rows);
        for (int budget : {4, 6, 8, 12}) {
            const double consv = maxVertexSlack(traceSilhouette(m, budget, ShapeTrace::Conservative), m);
            const double bal   = maxVertexSlack(traceSilhouette(m, budget, ShapeTrace::Balanced), m);
            EXPECT_LE(bal, consv + 1e-3) << "budget " << budget;
        }
    }
}

// ── Coverage maps (headless via FrozenSpriteMask) ──────────────────────────────────────────────

TEST(ExactMask, ContainsMatchesCoverageUnderEveryOrientation) {
    const ArtMask base = maskFromRows(kL16);  // asymmetric — orientation is observable
    for (Rotation rot : {Rotation::None, Rotation::Rot90, Rotation::Rot180, Rotation::Rot270}) {
        for (bool fx : {false, true}) {
            for (bool fy : {false, true}) {
                const FrozenSpriteMask shape = frozenOf(base, Space::Quad, rot, fx, fy);
                // A quad point that is the oriented image of an art pixel center must report the pixel's
                // own coverage — the inverse-orientation round trip.
                for (int y = 0; y < base.height; ++y) {
                    for (int x = 0; x < base.width; ++x) {
                        const Point q = orientPoint(Point{x + 0.5f, y + 0.5f}, 16, 16, rot, fx, fy);
                        EXPECT_EQ(shape.contains(q), base.at(x, y))
                            << "rot " << static_cast<int>(rot) << " fx " << fx << " fy " << fy
                            << " @ (" << x << "," << y << ")";
                    }
                }
            }
        }
    }
}

// A placed sprite whose quad→layer map is nontrivial (scale + rotation about a pivot, an offset origin).
Sprite placedSprite() {
    Sprite s{.key = "s"};
    s.size      = AssetDimensions{16, 16};
    s.x         = 40;
    s.y         = 24;
    s.origin    = {8.0f, 8.0f};
    s.pivot     = {8.0f, 8.0f};
    s.transform = Transform::scale(1.5f, 0.75f).then(Transform::rotation(20.0f));
    return s;
}

TEST(LayerSpace, ContainsLayerMatchesContainsQuadThroughToLayer) {
    const ArtMask base = maskFromRows(kL16);
    const Sprite  s    = placedSprite();
    const FrozenSpriteMask quad  = frozenOf(base, Space::Quad);
    const FrozenSpriteMask layer = frozenOf(base, Space::Layer, Rotation::None, false, false,
                                             spriteQuadToLayer(s));
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x) {
            const Point q = Point{x + 0.5f, y + 0.5f};
            EXPECT_EQ(layer.contains(s.toLayer(q)), quad.contains(q)) << "@ (" << x << "," << y << ")";
        }
}

TEST(Bounds, QuadBoundsAreTheVisibleArtExtent) {
    const FrozenSpriteMask shape = frozenOf(maskFromRows(kL16), Space::Quad);  // the L touches all 4 edges
    EXPECT_EQ(shape.bounds(), (IntRect{0, 0, 16, 16}));

    const FrozenSpriteMask inset =
        frozenOf(maskFromRows({"........", "..##....", "..##....", "........",
                               "........", "........", "........", "........"}),
                 Space::Quad);
    EXPECT_EQ(inset.bounds(), (IntRect{2, 1, 2, 2}));
}

// ── Device-backed: the Sprite verbs resolve coverage from the engine renderer ───────────────────

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its production
// path (D3D12 + DXIL) is covered by the Windows x64 job, so a missing device there is an out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class SpriteMaskDevice : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
    }

    static void TearDownTestSuite() {
        if (device_) {
            SDL_DestroyGPUDevice(device_);
            device_ = nullptr;
        }
        SDL_Quit();
    }

    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; the shape geometry is covered device-free above. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". The sprite-mask verbs resolve coverage from the renderer's uploaded pixels and so "
                      "need a GPU device on every production-representative platform (a software rasterizer "
                      "suffices; on a headless runner set SDL_VIDEODRIVER=offscreen).";
        }
    }

    // A 16×16 index atlas carrying the kL16 pattern (index 1 = body, index 0 = a GameBoy hole), uploaded to
    // a compose-only, windowless renderer. A sprite drawn from it answers the mask query off its `atlas`.
    static AtlasId uploadL(Renderer& r) {
        const ArtMask base = maskFromRows(kL16);
        std::array<std::uint8_t, 16 * 16> idx{};
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                idx[static_cast<std::size_t>(y) * 16 + x] = base.at(x, y) ? 1 : 0;
        return r.uploadAtlas(idx.data(), 16, 16, TransparentIndices::GameBoy).atlasId;
    }

    static Sprite spriteOn(AtlasId atlas) {
        Sprite s{.key = "s"};
        s.atlas = atlas;
        s.tile  = 0;
        s.size  = AssetDimensions{16, 16};
        return s;
    }
};

TEST_F(SpriteMaskDevice, MaskContainsMatchesUploadedCoverage) {
    Renderer     r{device_, nullptr};  // compose-only (no window)
    const AtlasId atlas = uploadL(r);
    const Sprite  s     = spriteOn(atlas);
    const ArtMask base  = maskFromRows(kL16);

    const SpriteMask shape = s.mask(Space::Quad);
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x)
            EXPECT_EQ(shape.contains(Point{x + 0.5f, y + 0.5f}), base.at(x, y)) << "@ (" << x << "," << y << ")";
    EXPECT_EQ(shape.bounds(), (IntRect{0, 0, 16, 16}));
}

TEST_F(SpriteMaskDevice, FreezeMaskSnapshotsTheCurrentCoverage) {
    Renderer     r{device_, nullptr};
    const AtlasId atlas = uploadL(r);
    const Sprite  s     = spriteOn(atlas);
    const ArtMask base  = maskFromRows(kL16);

    const FrozenSpriteMask frozen = s.freezeMask(Space::Quad);
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x)
            EXPECT_EQ(frozen.contains(Point{x + 0.5f, y + 0.5f}), base.at(x, y)) << "@ (" << x << "," << y << ")";
}

TEST_F(SpriteMaskDevice, MaskShapeContainsEveryVisiblePixel) {
    Renderer     r{device_, nullptr};
    const AtlasId atlas = uploadL(r);
    const Sprite  s     = spriteOn(atlas);
    const ArtMask base  = maskFromRows(kL16);

    for (int budget : {4, 8, 16, 64}) {
        const ShapePoints poly = s.maskShape(budget, Space::Quad, ShapeTrace::Conservative);
        EXPECT_TRUE(coversAll(poly.points, base)) << "budget " << budget;
    }
    EXPECT_THROW((void)s.maskShape(2, Space::Quad), std::invalid_argument);
}

TEST_F(SpriteMaskDevice, MaskShapeGeometryAnswersContainsInItsOwnSpace) {
    // The geometry form under a flipped + rotated placement, tested through ShapePoints::contains in BOTH
    // spaces — each polygon answers points in the space it was asked for. Conservative geometry contains
    // the mask, so every visible pixel center (mapped into that space) tests true.
    Renderer     r{device_, nullptr};
    const AtlasId atlas = uploadL(r);
    Sprite        s     = spriteOn(atlas);
    s.x = 40;
    s.y = 24;
    s.origin   = {8.0f, 8.0f};
    s.pivot    = {8.0f, 8.0f};
    s.rotation = Rotation::Rot90;
    s.flipX    = true;
    const ArtMask base = maskFromRows(kL16);

    const ShapePoints quad  = s.maskShape(64, Space::Quad, ShapeTrace::Conservative);
    const ShapePoints layer = s.maskShape(64, Space::Layer, ShapeTrace::Conservative);
    for (int y = 0; y < base.height; ++y)
        for (int x = 0; x < base.width; ++x) {
            if (!base.at(x, y)) continue;
            const Point art{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            const Point q = orientPoint(art, 16, 16, s.rotation, s.flipX, s.flipY);
            EXPECT_TRUE(quad.contains(q)) << "quad @ (" << x << "," << y << ")";
            EXPECT_TRUE(layer.contains(s.toLayer(q))) << "layer @ (" << x << "," << y << ")";
        }
}

TEST_F(SpriteMaskDevice, MaskShapeLayerIsToLayerOfMaskShapeQuad) {
    Renderer     r{device_, nullptr};
    const AtlasId atlas = uploadL(r);
    Sprite        s     = spriteOn(atlas);
    s.x = 40;
    s.y = 24;
    s.origin    = {8.0f, 8.0f};
    s.pivot     = {8.0f, 8.0f};
    s.transform = Transform::scale(1.5f, 0.75f).then(Transform::rotation(20.0f));

    const ShapePoints quad  = s.maskShape(16, Space::Quad);
    const ShapePoints layer = s.maskShape(16, Space::Layer);
    ASSERT_EQ(quad.points.size(), layer.points.size());
    for (std::size_t i = 0; i < quad.points.size(); ++i) {
        const Point expect = s.toLayer(quad.points[i]);
        EXPECT_NEAR(layer.points[i].x, expect.x, 1e-3f);
        EXPECT_NEAR(layer.points[i].y, expect.y, 1e-3f);
    }
}

}  // namespace
}  // namespace retropp
