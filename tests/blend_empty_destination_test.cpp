// A blend that reads its destination keeps the destination's coverage.
//
// Every non-Normal BlendMode (Multiply, Subtract, Add, Screen, Half, HalfSubtract) is a function of the
// pixel beneath it, so where there is no pixel beneath it there is nothing to grade: the combine keeps the
// destination's alpha, and a fully transparent destination stays fully transparent. Normal is alpha-over
// and paints wherever its source has coverage — a Normal ColorFill still fills a layer's holes
// (color_parity_layer_test.cpp pins that).
//
// The rule is one combine — retropp::applyBlendMode on the CPU, blendCombine in blend_ops.hlsli on the GPU
// — and these cases reach it through every site whose destination can be transparent:
//
//   Layer scope   a layer's own region grades the layer's isolated image, whose holes are empty:
//                 the region gate (each mode, partial region alpha, a rectangle wider than a sprite, a
//                 layer with no content at all), the ColorFill gather, and both curve gates.
//   Sprite runs   a non-Normal sprite composited into an isolated layer's scratch over its empty area.
//   Sprite art    a non-Normal sprite region over partly transparent art keeps the art's alpha.
//   Below scope   grades the accumulated image, which is opaque, so it reaches through the holes.
//
// Method: differential. Each case captures a control — the same scene without the grade — and every pixel
// the grade must leave alone is compared to it exactly. A graded pixel is compared to the CPU mirror over
// the control's pixel, within 2 8-bit steps (the mirror's destination is the 8-bit capture; the renderer's
// is float16). Which pixels are holes is computed from the art's own index layout, never from a capture.
//
// Device-backed: a compose-only, windowless Renderer on a real GPU device (a software rasterizer in CI),
// bootstrapped per file as the other device-backed suites do. The device-free half pins the mirror itself.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"        // TransparentIndices
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode, alphaOver, colorFillParams, groupGatherRuns
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

constexpr std::array<BlendMode, 6> kReadingModes{BlendMode::Multiply, BlendMode::Subtract, BlendMode::Add,
                                                 BlendMode::Screen,   BlendMode::Half,     BlendMode::HalfSubtract};

// ── The mirror (device-free) ────────────────────────────────────────────────────────────────────────

TEST(BlendEmptyDestinationMirror, AReadingModeOverNothingLeavesNothing) {
    const Vec4 empty{0.3f, 0.5f, 0.7f, 0.0f};
    const Vec4 fill{0.4f, 0.6f, 0.8f, 1.0f};
    for (const BlendMode mode : kReadingModes) {
        EXPECT_EQ(applyBlendMode(empty, fill, mode).w, 0.0f) << "mode " << static_cast<int>(mode);
    }
}

TEST(BlendEmptyDestinationMirror, AReadingModeKeepsAPartialCoverage) {
    const Vec4 half{0.3f, 0.5f, 0.7f, 0.5f};
    const Vec4 fill{0.4f, 0.6f, 0.8f, 1.0f};
    for (const BlendMode mode : kReadingModes) {
        EXPECT_EQ(applyBlendMode(half, fill, mode).w, 0.5f) << "mode " << static_cast<int>(mode);
    }
}

TEST(BlendEmptyDestinationMirror, NormalPaintsOverNothing) {
    const Vec4 out = applyBlendMode(Vec4{0.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.4f, 0.6f, 0.8f, 1.0f}, BlendMode::Normal);
    EXPECT_EQ(out.w, 1.0f);
    EXPECT_EQ(out.x, 0.4f);
    EXPECT_EQ(applyBlendMode(Vec4{0.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.4f, 0.6f, 0.8f, 0.25f}, BlendMode::Normal).w,
              0.25f);
}

TEST(BlendEmptyDestinationMirror, AReadingModeOverAnOpaquePixelGradesIt) {
    // The opaque destination every accumulator, frame and Below grade composites onto: the full grade.
    const Vec4 out = applyBlendMode(Vec4{0.5f, 0.5f, 0.5f, 1.0f}, Vec4{0.5f, 0.5f, 0.5f, 1.0f}, BlendMode::Multiply);
    EXPECT_EQ(out.x, 0.25f);
    EXPECT_EQ(out.w, 1.0f);
}

TEST(BlendEmptyDestinationMirror, ASpriteRegionKeepsTheArtsAlpha) {
    Sprite s{.key = "s"};
    s.regions = {Region{.key     = "shade",
                        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                                      .fill = Rgba8{128, 128, 200, 255}}},
                        .blend   = BlendMode::Multiply}};
    const std::vector<SpriteFxRecord> recs = buildSpriteFxRecords(s);
    ASSERT_FALSE(recs.empty());
    const std::optional<Vec4> out = evalSpriteFxRecords(Vec4{0.8f, 0.6f, 0.4f, 0.5f}, 0.5f, 0.5f, 8, 8, recs);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->w, 0.5f) << "a Multiply region over half-covered art keeps the art's coverage";
    EXPECT_LT(out->x, 0.8f) << "and still grades its color";
}

// ── Device fixture ──────────────────────────────────────────────────────────────────────────────────

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class BlendEmptyDestination : public ::testing::Test {
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
                                "backend in CI; the combine is pinned device-free by BlendEmptyDestinationMirror, "
                                "and the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This suite requires a GPU device on every production-representative platform "
                      "(macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a software rasterizer (lavapipe / WARP) "
                      "suffices; on a headless runner set SDL_VIDEODRIVER=offscreen.";
        }
    }
};

// ── Scene helpers ───────────────────────────────────────────────────────────────────────────────────

struct Art {
    AtlasId   atlas{};
    PaletteId palette{};
};

// The opaque lower layer's art: a 16×16 four-color index pattern, no transparency.
Art uploadLowerArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16, TransparentIndices::None).atlasId;
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    return {.atlas = atlas, .palette = r.uploadPalette(std::span<const Rgba8>(pal))};
}

// The holed art: one 8×8 tile whose left four columns are index 0 (a hole) and right four index 1, an
// opaque warm color. Tiled across a layer, every pixel with (x mod 8) < 4 is a hole.
constexpr Rgba8 kArtColor{200, 120, 60, 255};
Art uploadHoledArt(Renderer& r) {
    std::array<std::uint8_t, 8 * 8> idx{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            idx[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] = x < 4 ? 0 : 1;
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> pal{{{0, 0, 0, 255}, kArtColor}};
    return {.atlas = atlas, .palette = r.uploadPalette(std::span<const Rgba8>(pal))};
}
bool isHole(int x) { return (x % 8) < 4; }

// A solid 8×8 sheet of one palette color (which may carry partial alpha).
Art uploadSolid(Renderer& r, Rgba8 color) {
    std::array<std::uint8_t, 8 * 8> idx{};
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8, TransparentIndices::None).atlasId;
    const std::array<Rgba8, 1> pal{{color}};
    return {.atlas = atlas, .palette = r.uploadPalette(std::span<const Rgba8>(pal))};
}

DrawLayer lowerLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas, .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
    DrawLayer l{.key = "lower"};
    l.z       = 0;
    l.size    = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

DrawLayer holedLayer(const Art& art, std::vector<TileCell>& keep) {
    keep.assign(8 * 8, TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette});
    DrawLayer l{.key = "holed"};
    l.z       = 10;
    l.size    = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

ScreenSpaceEffect fillEffect(Rgba8 fill, ScreenSpaceEffectScope scope) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = fill};
}

Region gradeRegion(Rgba8 fill, BlendMode blend, ScreenSpaceEffectScope scope, ShapePoints shape = {},
                   float alpha = 1.0f) {
    Region reg{.key = "grade"};
    reg.shape   = std::move(shape);
    reg.effects = {fillEffect(fill, scope)};
    reg.alpha   = alpha;
    reg.blend   = blend;
    return reg;
}

// A distinct fill per mode that visibly moves kArtColor.
Rgba8 fillFor(BlendMode mode) {
    switch (mode) {
        case BlendMode::Multiply:     return Rgba8{128, 128, 200, 255};
        case BlendMode::Subtract:     return Rgba8{40, 30, 20, 255};
        case BlendMode::Add:          return Rgba8{40, 30, 20, 255};
        case BlendMode::Screen:       return Rgba8{80, 80, 120, 255};
        case BlendMode::Half:         return Rgba8{60, 200, 230, 255};
        case BlendMode::HalfSubtract: return Rgba8{40, 40, 40, 255};
        case BlendMode::Normal:
        default:                      return Rgba8{60, 60, 255, 255};
    }
}

// ── Pixel checks ────────────────────────────────────────────────────────────────────────────────────

Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
std::uint8_t quant(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
Rgba8 quant4(Vec4 v) { return Rgba8{quant(v.x), quant(v.y), quant(v.z), quant(v.w)}; }
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)), std::abs(int(a.b) - int(b.b)),
                     std::abs(int(a.a) - int(b.a))});
}
std::size_t at(int x, int y) { return static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x); }
std::string px(Rgba8 c) {
    return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a);
}

// The fill's grade over an opaque control pixel, as the mirror computes it.
Rgba8 mirrorGrade(Rgba8 control, Rgba8 fill, BlendMode mode, float alpha = 1.0f) {
    const ColorFillParams fp = colorFillParams(fillEffect(fill, ScreenSpaceEffectScope::Layer));
    return quant4(applyBlendMode(norm(control), Vec4{fp.r, fp.g, fp.b, alpha}, mode));
}

// Holes exact to the control; art pixels graded to the mirror (and genuinely moved by the grade).
::testing::AssertionResult holesClearArtGraded(const std::vector<Rgba8>& got, const std::vector<Rgba8>& control,
                                               Rgba8 fill, BlendMode mode, float alpha = 1.0f) {
    if (got.size() != control.size()) return ::testing::AssertionFailure() << "size mismatch";
    int moved = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 g = got[at(x, y)], c = control[at(x, y)];
            if (isHole(x)) {
                if (!exactEq(g, c))
                    return ::testing::AssertionFailure()
                           << "hole at (" << x << "," << y << ") is " << px(g) << ", the control shows " << px(c);
                continue;
            }
            const Rgba8 want = mirrorGrade(c, fill, mode, alpha);
            if (chDelta(g, want) > 2)
                return ::testing::AssertionFailure() << "art at (" << x << "," << y << ") is " << px(g)
                                                     << ", the mirror grades it to " << px(want);
            if (chDelta(g, c) > 2) ++moved;
        }
    }
    if (moved == 0) return ::testing::AssertionFailure() << "the grade moved no art pixel";
    return ::testing::AssertionSuccess();
}

// Every hole exact to the control; every art pixel either the control's or the mirror's grade of it, and
// at least one graded — for a shape whose curved edge makes the graded set a matter of the gate.
::testing::AssertionResult holesClearSomeArtGraded(const std::vector<Rgba8>& got, const std::vector<Rgba8>& control,
                                                   Rgba8 fill, BlendMode mode) {
    if (got.size() != control.size()) return ::testing::AssertionFailure() << "size mismatch";
    int graded = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 g = got[at(x, y)], c = control[at(x, y)];
            if (isHole(x)) {
                if (!exactEq(g, c))
                    return ::testing::AssertionFailure()
                           << "hole at (" << x << "," << y << ") is " << px(g) << ", the control shows " << px(c);
                continue;
            }
            if (exactEq(g, c)) continue;
            const Rgba8 want = mirrorGrade(c, fill, mode);
            if (chDelta(g, want) > 2)
                return ::testing::AssertionFailure() << "art at (" << x << "," << y << ") is " << px(g)
                                                     << ", neither the control's " << px(c) << " nor the mirror's "
                                                     << px(want);
            ++graded;
        }
    }
    if (graded == 0) return ::testing::AssertionFailure() << "the grade reached no art pixel";
    return ::testing::AssertionSuccess();
}

// One whole-reach region of `mode` on the holed layer at `scope`, against the same scene without it.
struct Captures {
    std::vector<Rgba8> control;
    std::vector<Rgba8> graded;
};
Captures holedScene(SDL_GPUDevice* dev, const std::vector<Region>& regions) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art lower = uploadLowerArt(r);
    const Art holed = uploadHoledArt(r);
    std::vector<TileCell> lo0, hi0, lo1, hi1;

    FrameDrawState control;
    control.layers.push_back(lowerLayer(lower, lo0));
    control.layers.push_back(holedLayer(holed, hi0));

    FrameDrawState graded;
    graded.layers.push_back(lowerLayer(lower, lo1));
    DrawLayer hi = holedLayer(holed, hi1);
    hi.regions   = regions;
    graded.layers.push_back(hi);

    return {.control = r.captureViewport(control), .graded = r.captureViewport(graded)};
}

void runLayerMode(SDL_GPUDevice* dev, BlendMode mode) {
    const Rgba8    fill = fillFor(mode);
    const Captures c    = holedScene(dev, {gradeRegion(fill, mode, ScreenSpaceEffectScope::Layer)});
    EXPECT_TRUE(holesClearArtGraded(c.graded, c.control, fill, mode)) << "mode " << static_cast<int>(mode);
}

// ── Layer scope: each reading mode leaves a hole as the layers beneath show it ──────────────────────

TEST_F(BlendEmptyDestination, LayerScopeMultiplyLeavesHoles)     { runLayerMode(device_, BlendMode::Multiply); }
TEST_F(BlendEmptyDestination, LayerScopeSubtractLeavesHoles)     { runLayerMode(device_, BlendMode::Subtract); }
TEST_F(BlendEmptyDestination, LayerScopeAddLeavesHoles)          { runLayerMode(device_, BlendMode::Add); }
TEST_F(BlendEmptyDestination, LayerScopeScreenLeavesHoles)       { runLayerMode(device_, BlendMode::Screen); }
TEST_F(BlendEmptyDestination, LayerScopeHalfLeavesHoles)         { runLayerMode(device_, BlendMode::Half); }
TEST_F(BlendEmptyDestination, LayerScopeHalfSubtractLeavesHoles) { runLayerMode(device_, BlendMode::HalfSubtract); }

// A region at half strength over a hole: still nothing there, so no half-darkened hole.
TEST_F(BlendEmptyDestination, PartialRegionAlphaLeavesHoles) {
    const Rgba8    fill = fillFor(BlendMode::Multiply);
    const Captures c =
        holedScene(device_, {gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer, {}, 0.5f)});
    EXPECT_TRUE(holesClearArtGraded(c.graded, c.control, fill, BlendMode::Multiply, 0.5f));
}

// Below scope grades the accumulated image, opaque everywhere, so it reaches through the holes: every
// pixel is the mirror's grade of what the control shows there, and the holes change.
TEST_F(BlendEmptyDestination, BelowScopeGradesThroughHoles) {
    const Rgba8    fill = fillFor(BlendMode::Multiply);
    const Captures c    = holedScene(device_, {gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Below)});
    ASSERT_EQ(c.graded.size(), c.control.size());
    int holesGraded = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 g = c.graded[at(x, y)], ctl = c.control[at(x, y)];
            const Rgba8 want = mirrorGrade(ctl, fill, BlendMode::Multiply);
            ASSERT_LE(chDelta(g, want), 2) << "at (" << x << "," << y << ") got " << px(g) << ", the mirror "
                                           << px(want);
            if (isHole(x) && chDelta(g, ctl) > 2) ++holesGraded;
        }
    }
    EXPECT_GT(holesGraded, 0) << "Below scope left every hole as it was";
}

// Two contiguous ColorFill regions take the gather: one pass walking both records.
TEST_F(BlendEmptyDestination, GatheredRegionsLeaveHoles) {
    const Rgba8             fill = fillFor(BlendMode::Multiply);
    const std::vector<Region> regions = {
        gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer, ShapePoints::rectangle(Point{0, 0}, 32, 64)),
        gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer, ShapePoints::rectangle(Point{32, 0}, 32, 64)),
    };
    // The two steps form one gather run, so the case reaches the gather and not the per-region gate.
    std::vector<GatherStep> steps;
    for (const Region& reg : regions)
        steps.push_back(GatherStep{.eligible = colorFillGatherEligible(reg.effects.front(), reg.shape),
                                   .stage    = kColorFillGatherStage});
    const GatherGrouping grouping = groupGatherRuns(steps);
    ASSERT_EQ(grouping.runs.size(), 1u);
    ASSERT_EQ(grouping.runs.front().steps.size(), 2u);

    const Captures c = holedScene(device_, regions);
    EXPECT_TRUE(holesClearArtGraded(c.graded, c.control, fill, BlendMode::Multiply));
}

// A closed curve as the region's boundary. A boundary of quadratic segments takes the analytic curve gate;
// a cubic one with a baked mask takes the mask gate. Each case asserts its route first.
CurveSegment quadratic(Vec2 a, Vec2 ctrl, Vec2 b) {
    return CurveSegment{.p0 = a, .p1 = ctrl, .p2 = b, .degree = CurveDegree::Quadratic};
}
Curve quadraticLoop() {
    constexpr float cx = 32.0f, cy = 32.0f, r = 24.0f;
    const Vec2 n{cx, cy - r}, e{cx + r, cy}, s{cx, cy + r}, w{cx - r, cy};
    return Curve{.segments = {quadratic(n, Vec2{cx + r, cy - r}, e), quadratic(e, Vec2{cx + r, cy + r}, s),
                              quadratic(s, Vec2{cx - r, cy + r}, w), quadratic(w, Vec2{cx - r, cy - r}, n)},
                 .closed   = true};
}
Curve cubicLoop() {
    const std::array<Vec2, 4> pts{{Vec2{10.0f, 10.0f}, Vec2{54.0f, 10.0f}, Vec2{54.0f, 54.0f}, Vec2{10.0f, 54.0f}}};
    return Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);
}

TEST_F(BlendEmptyDestination, CurveRegionLeavesHoles) {
    const ShapePoints shape = ShapePoints::fromCurve(quadraticLoop());
    ASSERT_EQ(regionCurvePath(shape), CurveRegionPath::Analytic);
    const Rgba8    fill = fillFor(BlendMode::Multiply);
    const Captures c    = holedScene(device_, {gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer, shape)});
    EXPECT_TRUE(holesClearSomeArtGraded(c.graded, c.control, fill, BlendMode::Multiply));
}

TEST_F(BlendEmptyDestination, CurveMaskRegionLeavesHoles) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   lower = uploadLowerArt(r);
    const Art   holed = uploadHoledArt(r);
    const Rgba8 fill  = fillFor(BlendMode::Multiply);
    const ShapePoints masked = r.bakeCurveRegion(cubicLoop());
    ASSERT_NE(masked.curveMask, CurveMaskId{}) << "the region carries no baked mask";
    ASSERT_EQ(regionCurvePath(masked), CurveRegionPath::Mask);
    std::vector<TileCell> lo0, hi0, lo1, hi1;

    FrameDrawState control;
    control.layers.push_back(lowerLayer(lower, lo0));
    control.layers.push_back(holedLayer(holed, hi0));
    FrameDrawState graded;
    graded.layers.push_back(lowerLayer(lower, lo1));
    DrawLayer hi = holedLayer(holed, hi1);
    hi.regions   = {gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer, masked)};
    graded.layers.push_back(hi);

    const std::vector<Rgba8> ctl = r.captureViewport(control);
    const std::vector<Rgba8> got = r.captureViewport(graded);
    EXPECT_TRUE(holesClearSomeArtGraded(got, ctl, fill, BlendMode::Multiply));
}

// A layer with no content at all, holding a Multiply region: there is nothing to grade anywhere, so the
// frame is the frame without that layer.
TEST_F(BlendEmptyDestination, AnEmptyLayersReadingRegionDrawsNothing) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art lower = uploadLowerArt(r);
    std::vector<TileCell> lo0, lo1;

    FrameDrawState without;
    without.layers.push_back(lowerLayer(lower, lo0));

    FrameDrawState with;
    with.layers.push_back(lowerLayer(lower, lo1));
    DrawLayer empty{.key = "empty"};
    empty.z       = 10;
    empty.size    = PixelSize{kW, kH};
    empty.regions = {gradeRegion(fillFor(BlendMode::Multiply), BlendMode::Multiply, ScreenSpaceEffectScope::Layer)};
    with.layers.push_back(std::move(empty));

    const std::vector<Rgba8> want = r.captureViewport(without);
    const std::vector<Rgba8> got  = r.captureViewport(with);
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
        ASSERT_TRUE(exactEq(got[i], want[i])) << "pixel " << i << " is " << px(got[i]) << ", without the layer "
                                              << px(want[i]);
}

// A rectangle region wider than its sprite on every side: on the sprite's opaque pixels it grades, and
// everywhere else — the sprite's own see-through pixels and the margin around it — the frame is the frame
// without the region.
TEST_F(BlendEmptyDestination, ARegionWiderThanItsSpriteLeavesTheSurround) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   lower  = uploadLowerArt(r);
    const Art   sprite = uploadHoledArt(r);  // its left four columns are see-through
    const Rgba8 fill   = fillFor(BlendMode::Multiply);
    constexpr int kSx = 24, kSy = 20;
    std::vector<TileCell> lo0, lo1;
    std::vector<Sprite>   s0, s1;

    auto scene = [&](bool withRegion, std::vector<TileCell>& lo, std::vector<Sprite>& keep) {
        FrameDrawState f;
        f.layers.push_back(lowerLayer(lower, lo));
        keep = {Sprite{.key = "hero", .x = kSx, .y = kSy, .atlas = sprite.atlas, .tile = 0, .palette = sprite.palette}};
        DrawLayer sp{.key = "sprites"};
        sp.z       = 10;
        sp.size    = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        if (withRegion)
            sp.regions = {gradeRegion(fill, BlendMode::Multiply, ScreenSpaceEffectScope::Layer,
                                      ShapePoints::rectangle(Point{kSx - 8, kSy - 8}, 24, 24))};
        f.layers.push_back(sp);
        return f;
    };
    const std::vector<Rgba8> ctl = r.captureViewport(scene(false, lo0, s0));
    const std::vector<Rgba8> got = r.captureViewport(scene(true, lo1, s1));
    ASSERT_EQ(got.size(), ctl.size());

    int graded = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 g = got[at(x, y)], c = ctl[at(x, y)];
            const bool  onArt = x >= kSx + 4 && x < kSx + 8 && y >= kSy && y < kSy + 8;
            if (!onArt) {
                ASSERT_TRUE(exactEq(g, c)) << "off the sprite's art at (" << x << "," << y << "): " << px(g)
                                           << ", without the region " << px(c);
                continue;
            }
            const Rgba8 want = mirrorGrade(c, fill, BlendMode::Multiply);
            ASSERT_LE(chDelta(g, want), 2) << "on the sprite at (" << x << "," << y << "): " << px(g)
                                           << ", the mirror " << px(want);
            if (chDelta(g, c) > 2) ++graded;
        }
    }
    EXPECT_GT(graded, 0) << "the region graded none of the sprite";
}

// A Multiply sprite inside a layer composited on its own (its DrawLayer::blend is Multiply) grades the
// layer's own image; over the layer's empty area there is nothing to grade, so the sprite contributes
// nothing and the frame is the frame without it.
TEST_F(BlendEmptyDestination, AReadingSpriteOverAnEmptyLayerAreaDrawsNothing) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art lower = uploadLowerArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> lo0, lo1;
    std::vector<Sprite>   s0, s1;

    auto scene = [&](bool withSprite, std::vector<TileCell>& lo, std::vector<Sprite>& keep) {
        FrameDrawState f;
        f.layers.push_back(lowerLayer(lower, lo));
        keep.clear();
        if (withSprite)
            keep.push_back(Sprite{.key = "shadow", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0,
                                  .palette = solid.palette, .blend = BlendMode::Multiply});
        DrawLayer sp{.key = "sprites"};
        sp.z       = 10;
        sp.size    = PixelSize{kW, kH};
        sp.blend   = BlendMode::Multiply;  // the layer composites on its own, from its own image
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        f.layers.push_back(sp);
        return f;
    };
    const std::vector<Rgba8> want = r.captureViewport(scene(false, lo0, s0));
    const std::vector<Rgba8> got  = r.captureViewport(scene(true, lo1, s1));
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < got.size(); ++i)
        ASSERT_TRUE(exactEq(got[i], want[i])) << "pixel " << i << " is " << px(got[i]) << ", without the sprite "
                                              << px(want[i]);
}

// A Multiply sprite region over art of alpha 128 grades the art's color and keeps its alpha: the sprite
// still lets half the scene through.
TEST_F(BlendEmptyDestination, ASpriteRegionKeepsPartlyTransparentArtsCoverage) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   lower = uploadLowerArt(r);
    const Rgba8 art{200, 160, 120, 128};
    const Art   solid = uploadSolid(r, art);
    const Rgba8 fill  = fillFor(BlendMode::Multiply);
    constexpr int kSx = 24, kSy = 20;
    std::vector<TileCell> lo0, lo1;
    std::vector<Sprite>   s1;

    FrameDrawState base;
    base.layers.push_back(lowerLayer(lower, lo0));
    const std::vector<Rgba8> scene = r.captureViewport(base);

    FrameDrawState f;
    f.layers.push_back(lowerLayer(lower, lo1));
    Sprite s{.key = "glass", .x = kSx, .y = kSy, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
    s.regions = {Region{.key     = "shade",
                        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fill}},
                        .blend   = BlendMode::Multiply}};
    s1 = {s};
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(s1)};
    f.layers.push_back(sp);
    const std::vector<Rgba8> got = r.captureViewport(f);
    ASSERT_EQ(got.size(), scene.size());

    // The graded color is the mirror's; the coverage is stated outright as the art's own, so the case does
    // not take its expected alpha from the combine it checks.
    const ColorFillParams fp = colorFillParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fill});
    const Vec4 color  = applyBlendMode(norm(art), Vec4{fp.r, fp.g, fp.b, 1.0f}, BlendMode::Multiply);
    const Vec4 graded{color.x, color.y, color.z, norm(art).w};
    for (int y = kSy; y < kSy + 8; ++y) {
        for (int x = kSx; x < kSx + 8; ++x) {
            const Rgba8 want = quant4(alphaOver(norm(scene[at(x, y)]), graded));
            ASSERT_LE(chDelta(got[at(x, y)], want), 2)
                << "at (" << x << "," << y << ") got " << px(got[at(x, y)]) << ", want " << px(want)
                << " — the art's alpha of 128 carried through the region";
        }
    }
}

}  // namespace
