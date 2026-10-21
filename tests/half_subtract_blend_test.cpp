// BlendMode::HalfSubtract — the halved difference, B(dst, src) = (dst − src) / 2, at every place a blend
// sits. Two halves:
//
//   1. The operator (device-free). blendChannel returns the raw halved difference, which goes negative when
//      the source is the brighter of the two; applyBlendMode clamps the combine, so the result floors at 0
//      exactly as Subtract's does. Source alpha weights it like every other mode.
//
//   2. The composite (device-backed, via captureViewport). One case per container that carries a blend: a
//      sprite, a blended layer, the whole-frame overlay, a lone region, a run of regions drawn together, a
//      Below-scope grade on a content-less layer, and a region on a sprite — graded over the sprite's own
//      pixels at its own scope, and over the scene beneath its silhouette at Below scope. A blended sprite
//      composites through the same operator as a layer; a region on a sprite is graded inside the sprite
//      shaders, which carry their own copy. Each captures a baseline of the scene without the
//      blended element, captures the scene with it, and compares the graded pixels against the CPU mirror,
//      applyBlendMode(baseline, source, HalfSubtract), at 2 8-bit steps: the mirror's destination is the
//      8-bit captured baseline, one quantization step more than the renderer's float16 scene carries, on
//      top of the composite's own rounding. Where a container's source is drawn art rather than a fill, the
//      source color is read from a Normal capture of the same element, so palette → float → 8-bit rounding
//      cancels. Pixels outside a confined element are byte-identical to the baseline.
//
// Device-backed, compose-only and windowless: a GPU device with no display, so it runs on a software
// rasterizer in CI — lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows, Metal on the Mac. A device is
// required on every production-representative platform; the one skip is Windows on ARM, whose CI runner
// has no GPU backend.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"        // TransparentIndices
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode, blendChannel, colorFillParams
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: the operator (device-free) ───────────────────────────────────────────────────────────

// A blend at full source coverage (src.a = 1): out.rgb is exactly the operator B(dst, src), out.a = 1.
constexpr Vec4 opaqueBlend(Vec4 dst, Vec4 src, BlendMode mode) {
    return applyBlendMode(dst, Vec4{src.x, src.y, src.z, 1.0f}, mode);
}

TEST(HalfSubtractOperator, IsHalfTheDifference) {
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::HalfSubtract, 0.5f, 0.25f), 0.125f);  // (0.5 − 0.25) / 2
    const Vec4 got = opaqueBlend(Vec4{0.5f, 0.5f, 0.5f, 1.0f}, Vec4{0.25f, 0, 0, 1}, BlendMode::HalfSubtract);
    EXPECT_FLOAT_EQ(got.x, 0.125f);
    EXPECT_FLOAT_EQ(got.w, 1.0f);
}

TEST(HalfSubtractOperator, FloorsAtZeroAsSubtractDoes) {
    // The operator itself does not clamp: a brighter source leaves a negative halved difference.
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::HalfSubtract, 0.25f, 0.5f), -0.125f);
    // applyBlendMode clamps the combine, so the output floors at 0.
    EXPECT_FLOAT_EQ(opaqueBlend(Vec4{0.25f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::HalfSubtract).x, 0.0f);
    // Subtract on the same two inputs: the same placement of the clamp.
    EXPECT_FLOAT_EQ(blendChannel(BlendMode::Subtract, 0.25f, 0.5f), -0.25f);
    EXPECT_FLOAT_EQ(opaqueBlend(Vec4{0.25f, 0, 0, 1}, Vec4{0.5f, 0, 0, 1}, BlendMode::Subtract).x, 0.0f);
}

TEST(HalfSubtractOperator, SourceAlphaWeightsIt) {
    // (1 − 0.5)·0.8 + 0.5·((0.8 − 0.4) / 2) = 0.4 + 0.1 = 0.5
    const Vec4 got = applyBlendMode(Vec4{0.8f, 0, 0, 1}, Vec4{0.4f, 0, 0, 0.5f}, BlendMode::HalfSubtract);
    EXPECT_FLOAT_EQ(got.x, 0.5f);
}

// ── Part 2: the composite (device-backed) ────────────────────────────────────────────────────────

constexpr int kW = 64;
constexpr int kH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class HalfSubtractBlendTest : public ::testing::Test {
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
                                "backend in CI; its production path (D3D12 + DXIL) is covered by the Windows "
                                "x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This device-backed test requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); install a GPU driver (a "
                      "software rasterizer such as lavapipe suffices) and, on a headless runner, set "
                      "SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }
};

// ── Pixels and the mirror ────────────────────────────────────────────────────────────────────────

Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
int  chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}

// The color a ColorFill of `fill` under `mode` at alpha `alpha` grades over an opaque scene pixel — the
// applyBlendMode / colorFillParams authority, quantized.
Rgba8 expectFillBlend(Rgba8 scenePx, Rgba8 fill, float intensity, float alpha, BlendMode mode) {
    const ColorFillParams fp =
        colorFillParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill,
                                          .fill = fill, .fillIntensity = intensity});
    const Vec4 out = applyBlendMode(norm(scenePx), Vec4{fp.r, fp.g, fp.b, alpha}, mode);
    return Rgba8{quant(out.x), quant(out.y), quant(out.z), quant(out.w)};
}

// Every pixel of a graded capture against the mirror's prediction from the captured baseline.
::testing::AssertionResult
matchesGrade(const std::vector<Rgba8>& got, const std::vector<Rgba8>& baseline, Rgba8 fill,
             float intensity, float alpha, BlendMode mode, int maxDelta = 2) {
    if (got.size() != baseline.size())
        return ::testing::AssertionFailure() << "size mismatch " << got.size() << " vs " << baseline.size();
    for (std::size_t i = 0; i < got.size(); ++i) {
        const Rgba8 want = expectFillBlend(baseline[i], fill, intensity, alpha, mode);
        if (chDelta(got[i], want) > maxDelta) {
            const int x = int(i) % kW, y = int(i) / kW;
            return ::testing::AssertionFailure()
                   << "grade mismatch at (" << x << ", " << y << "): got " << int(got[i].r) << ","
                   << int(got[i].g) << "," << int(got[i].b) << "," << int(got[i].a) << "  mirror wants "
                   << int(want.r) << "," << int(want.g) << "," << int(want.b) << "," << int(want.a)
                   << "  (allowed ±" << maxDelta << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

// ── Shared art ───────────────────────────────────────────────────────────────────────────────────

struct Art {
    AtlasId   atlas{};
    PaletteId palette{};
};

// A 4-color opaque tile background filling the viewport — the scene every case blends over.
Art uploadBgArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16).atlasId;  // opaque (default None)
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// `keep` outlives the returned layer's span.
DrawLayer bgLayer(std::int32_t z, const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas, .tile = static_cast<std::uint16_t>((tx + ty) % 4),
                         .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z    = z;
    l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

// A solid opaque 8×8 sheet (one index) and a palette carrying `color` at that index, so tile 0 draws a flat,
// known color.
Art uploadSolid(Renderer& r, Rgba8 color) {
    std::array<std::uint8_t, 8 * 8> idx{};  // all index 0
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;  // opaque
    const std::array<Rgba8, 4> pal{{color, color, color, color}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// A whole-reach Region whose ColorFill is graded by the region's `blend` at `scope`.
Region gradeRegion(Rgba8 fill, float intensity, float alpha, BlendMode blend, ScreenSpaceEffectScope scope) {
    Region reg{.key = "grade"};
    reg.blend   = blend;
    reg.alpha   = alpha;
    reg.effects = {ScreenSpaceEffect{
        .kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = fill, .fillIntensity = intensity}};
    return reg;
}

// A Region filling `shape` with `fill`, combined over the scene under HalfSubtract.
Region halfSubtractFill(const char* key, ShapePoints shape, Rgba8 fill) {
    return Region{.key     = key,
                  .shape   = std::move(shape),
                  .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fill}},
                  .blend   = BlendMode::HalfSubtract};
}

// Each covered pixel (`N` differs from `B`) of graded capture `G` against
// applyBlendMode(B, {N's color, 1}, HalfSubtract); every other pixel of `G` equal to `B`.
void expectDrawnSourceGrade(const std::vector<Rgba8>& B, const std::vector<Rgba8>& N,
                            const std::vector<Rgba8>& G, const char* name) {
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());
    int covered = 0, leaked = 0, graded = 0, mismatched = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {
            ++covered;
            const Vec4  want4 = applyBlendMode(norm(B[i]), Vec4{norm(N[i]).x, norm(N[i]).y, norm(N[i]).z, 1.0f},
                                               BlendMode::HalfSubtract);
            const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
            if (chDelta(G[i], want) > 2) {
                if (mismatched++ == 0) {
                    const int x = int(i) % kW, y = int(i) / kW;
                    ADD_FAILURE() << name << " grade mismatch at (" << x << "," << y << "): got " << int(G[i].r)
                                  << "," << int(G[i].g) << "," << int(G[i].b) << "  want " << int(want.r) << ","
                                  << int(want.g) << "," << int(want.b);
                }
            } else if (chDelta(G[i], B[i]) > 2) {
                ++graded;
            }
        } else if (!exactEq(G[i], B[i])) {
            ++leaked;
        }
    }
    EXPECT_EQ(mismatched, 0) << name << ": " << mismatched << " covered pixels disagree with the mirror";
    EXPECT_GT(covered, 0) << name << ": the element covered no pixel";
    EXPECT_EQ(leaked, 0) << name << ": " << leaked << " pixels changed outside the element";
    EXPECT_GT(graded, 0) << name << ": the blend moved no covered pixel off the baseline";
}

// ── The six containers ───────────────────────────────────────────────────────────────────────────

// A solid sprite over the scene — the sprite shaders' copy of the operator.
TEST_F(HalfSubtractBlendTest, ASpriteHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadBgArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> c0, c1, c2;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto spriteScene = [&](BlendMode blend, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(0, bg, cells));
        keepS = {Sprite{.key = "fx", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0,
                        .palette = solid.palette, .blend = blend}};
        DrawLayer sp{.key = "sprites"};
        sp.z       = 10;
        sp.size    = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    const FrameDrawState fN = spriteScene(BlendMode::Normal, c1, sN);
    const FrameDrawState fG = spriteScene(BlendMode::HalfSubtract, c2, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);
    expectDrawnSourceGrade(B, N, G, "sprite");
}

// A full-viewport opaque layer whose own blend is HalfSubtract, over the scene.
TEST_F(HalfSubtractBlendTest, ABlendedLayerHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadBgArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> c0, c1, c2;
    std::vector<TileCell> solidCells(8 * 8, TileCell{.atlas = solid.atlas, .tile = 0, .palette = solid.palette});

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto layerScene = [&](BlendMode blend, std::vector<TileCell>& cells) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(0, bg, cells));
        DrawLayer top{.key = "top"};
        top.z       = 10;
        top.size    = PixelSize{kW, kH};
        top.blend   = blend;
        top.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                  .cells = std::span<const TileCell>(solidCells)};
        f.layers.push_back(top);
        return f;
    };
    const std::vector<Rgba8> N = r.captureViewport(layerScene(BlendMode::Normal, c1));
    const std::vector<Rgba8> G = r.captureViewport(layerScene(BlendMode::HalfSubtract, c2));
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    int graded = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        const Vec4  want4 = applyBlendMode(norm(B[i]), Vec4{norm(N[i]).x, norm(N[i]).y, norm(N[i]).z, 1.0f},
                                           BlendMode::HalfSubtract);
        const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
        if (chDelta(G[i], want) > 2) {
            const int x = int(i) % kW, y = int(i) / kW;
            FAIL() << "blended layer grade mismatch at (" << x << "," << y << "): got " << int(G[i].r) << ","
                   << int(G[i].g) << "," << int(G[i].b) << "  want " << int(want.r) << "," << int(want.g) << ","
                   << int(want.b);
        }
        if (chDelta(G[i], B[i]) > 2) ++graded;
    }
    EXPECT_GT(graded, 0) << "the blended layer moved no pixel off the baseline";
}

// The whole-frame overlay: a frame-level ColorFill combined over the composited image under the frame's blend.
TEST_F(HalfSubtractBlendTest, AWholeFrameOverlayHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   bg = uploadBgArt(r);
    const Rgba8 fill{60, 40, 20, 255};
    std::vector<TileCell> c0, c1;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    frame.layers.push_back(bgLayer(0, bg, c1));
    frame.blend = BlendMode::HalfSubtract;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fill});
    const std::vector<Rgba8> got = r.captureViewport(frame);

    EXPECT_TRUE(matchesGrade(got, baseline, fill, 1.0f, 1.0f, BlendMode::HalfSubtract)) << "frame overlay";
}

// One confined frame region: inside its circle the halved difference, outside it the scene untouched.
TEST_F(HalfSubtractBlendTest, ALoneRegionHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   bg = uploadBgArt(r);
    const Rgba8 fill{40, 40, 40, 255};
    const Point center{32, 32};
    std::vector<TileCell> c0, c1;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    frame.layers.push_back(bgLayer(0, bg, c1));
    frame.regions.push_back(halfSubtractFill("circle", ShapePoints::circle(center, 20.0f), fill));
    const std::vector<Rgba8> got = r.captureViewport(frame);
    ASSERT_EQ(got.size(), baseline.size());

    int inside = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        const int   x = int(i) % kW, y = int(i) / kW;
        const float dx = static_cast<float>(x) + 0.5f - center.x, dy = static_cast<float>(y) + 0.5f - center.y;
        const float d  = std::sqrt(dx * dx + dy * dy);
        if (d <= 18.5f) {
            ++inside;
            const Rgba8 want = expectFillBlend(baseline[i], fill, 1.0f, 1.0f, BlendMode::HalfSubtract);
            ASSERT_LE(chDelta(got[i], want), 2)
                << "inside the circle at (" << x << "," << y << "): got " << int(got[i].r) << "," << int(got[i].g)
                << "," << int(got[i].b) << "  want " << int(want.r) << "," << int(want.g) << "," << int(want.b);
        } else if (d >= 21.5f) {
            ASSERT_TRUE(exactEq(got[i], baseline[i])) << "outside the circle at (" << x << "," << y << ") changed";
        }
    }
    EXPECT_GT(inside, 0) << "no pixel lies inside the circle";
}

// Three disjoint regions with one blend, drawn together: each rectangle against its own fill's mirror, the
// gutters between them untouched.
TEST_F(HalfSubtractBlendTest, ARunOfRegionsHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art bg = uploadBgArt(r);
    struct Rect {
        const char* key;
        int         x;
        Rgba8       fill;
    };
    constexpr int                 kTop = 4, kRectW = 16, kRectH = 56;
    const std::array<Rect, 3>     rects{{{"left", 4, Rgba8{40, 40, 40, 255}},
                                         {"middle", 24, Rgba8{80, 30, 30, 255}},
                                         {"right", 44, Rgba8{30, 30, 80, 255}}}};
    std::vector<TileCell>         c0, c1;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    frame.layers.push_back(bgLayer(0, bg, c1));
    for (const Rect& rc : rects)
        frame.regions.push_back(halfSubtractFill(
            rc.key, ShapePoints::rectangle(Point{static_cast<float>(rc.x), static_cast<float>(kTop)}, kRectW, kRectH),
            rc.fill));
    const std::vector<Rgba8> got = r.captureViewport(frame);
    ASSERT_EQ(got.size(), baseline.size());

    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x);
            bool              claimed = false;
            for (std::size_t k = 0; k < rects.size(); ++k) {
                const Rect& rc = rects[k];
                const bool  onRect = x >= rc.x && x < rc.x + kRectW && y >= kTop && y < kTop + kRectH;
                if (!onRect) continue;
                claimed = true;
                const bool interior = x >= rc.x + 1 && x < rc.x + kRectW - 1 && y >= kTop + 1 && y < kTop + kRectH - 1;
                if (!interior) continue;
                const Rgba8 want = expectFillBlend(baseline[i], rc.fill, 1.0f, 1.0f, BlendMode::HalfSubtract);
                ASSERT_LE(chDelta(got[i], want), 2)
                    << rc.key << " at (" << x << "," << y << "): got " << int(got[i].r) << "," << int(got[i].g)
                    << "," << int(got[i].b) << "  want " << int(want.r) << "," << int(want.g) << "," << int(want.b);
            }
            // The gutters between and around the rectangles.
            if (!claimed)
                ASSERT_TRUE(exactEq(got[i], baseline[i])) << "outside every rectangle at (" << x << "," << y << ") changed";
        }
    }
}

// A content-less layer above the scene whose Below-scope ColorFill grades the composited image beneath it.
TEST_F(HalfSubtractBlendTest, ABelowScopeGradeHalfSubtracts) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art   bg = uploadBgArt(r);
    const Rgba8 fill{40, 30, 20, 255};
    std::vector<TileCell> c0, c1;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    FrameDrawState frame;
    frame.layers.push_back(bgLayer(0, bg, c1));
    DrawLayer fx{.key = "fx"};
    fx.z       = 5;
    fx.size    = PixelSize{kW, kH};
    fx.regions = {gradeRegion(fill, 1.0f, 1.0f, BlendMode::HalfSubtract, ScreenSpaceEffectScope::Below)};
    frame.layers.push_back(fx);
    const std::vector<Rgba8> got = r.captureViewport(frame);

    EXPECT_TRUE(matchesGrade(got, baseline, fill, 1.0f, 1.0f, BlendMode::HalfSubtract)) << "below_half_subtract";
}

// A solid sprite carrying one whole-silhouette region whose ColorFill is graded under HalfSubtract at
// `scope`. Covered pixels (where the plain sprite differs from the scene) are checked against
// applyBlendMode(dst, {fill, 1}, HalfSubtract): `dst` is the sprite's own color at Layer scope and the scene
// beneath it at Below scope. Off the sprite the frame equals the scene.
void runSpriteRegionCell(SDL_GPUDevice* dev, const char* name, ScreenSpaceEffectScope scope) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art   bg    = uploadBgArt(r);
    const Art   solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    const Rgba8 fill{40, 30, 20, 255};
    std::vector<TileCell> c0, c1, c2;

    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto spriteScene = [&](bool withRegion, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(0, bg, cells));
        Sprite s{.key = "fx", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        if (withRegion) {
            Region rg{.key = "grade"};  // no shape: the whole silhouette
            rg.blend   = BlendMode::HalfSubtract;
            rg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = fill}};
            s.regions  = {rg};
        }
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z       = 10;
        sp.size    = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sN, sG;
    const FrameDrawState fN = spriteScene(false, c1, sN);
    const FrameDrawState fG = spriteScene(true, c2, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    const bool below   = scope == ScreenSpaceEffectScope::Below;
    int        covered = 0, leaked = 0, mismatched = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {
            ++covered;
            const Rgba8 want = expectFillBlend(below ? B[i] : N[i], fill, 1.0f, 1.0f, BlendMode::HalfSubtract);
            if (chDelta(G[i], want) > 2 && mismatched++ == 0) {
                const int x = int(i) % kW, y = int(i) / kW;
                ADD_FAILURE() << name << " mismatch at (" << x << "," << y << "): got " << int(G[i].r) << ","
                              << int(G[i].g) << "," << int(G[i].b) << "  want " << int(want.r) << ","
                              << int(want.g) << "," << int(want.b);
            }
        } else if (!exactEq(G[i], B[i])) {
            ++leaked;
        }
    }
    EXPECT_EQ(mismatched, 0) << name << ": " << mismatched << " covered pixels disagree with the mirror";
    EXPECT_GT(covered, 0) << name << ": the sprite covered no pixel";
    EXPECT_EQ(leaked, 0) << name << ": " << leaked << " pixels changed off the sprite";
}

TEST_F(HalfSubtractBlendTest, ARegionOnASpriteHalfSubtracts) {
    runSpriteRegionCell(device_, "sprite_region_layer", ScreenSpaceEffectScope::Layer);
}

TEST_F(HalfSubtractBlendTest, ABelowScopeRegionOnASpriteHalfSubtracts) {
    runSpriteRegionCell(device_, "sprite_region_below", ScreenSpaceEffectScope::Below);
}

}  // namespace
