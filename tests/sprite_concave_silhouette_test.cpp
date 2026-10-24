// Concave silhouette under a whole-silhouette effect (device-backed). A whole-silhouette effect
// (ColorFill here) is a per-pixel color rewrite of the pixels that survive transparency — it never
// bridges an OPEN concavity, and it never enlarges the drawn footprint beyond the sprite's transformed
// quad. A horseshoe with an open mouth (an index-0 channel contiguous with the outside through the bottom
// edge) is filled and drawn across four container configurations — plain, partial-alpha Add, scaled, and
// partial-alpha Add + scaled — and checked on three axes per variant:
//
//   coverage — the mouth interior and its deep (near-edge) reach stay background: the effect painted only
//              surviving pixels, the open channel is never sealed.
//   geometry — the drawn content's bounding box matches the sprite's transformed quad within a pixel: the
//              silhouette is not realized as a filled hull or a traced polygon that spills past the art.
//   color   — a leg (interior) pixel matches applyBlendMode(scene, {fill, alpha}, mode): the partial-alpha
//              Add composite lands at straight-alpha weight, the fill's intended strength.
//
// The horseshoe touches all four quad edges (top bar row 0, both legs down cols 0 and 23, leg feet on row
// 23), so its content bounding box is the full transformed quad — a clean geometry oracle.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/image.h"        // TransparentIndices
#include "retropp/postprocess.h"  // applyBlendMode, Vec4
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 96;
constexpr int kH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class ConcaveSilhouette : public ::testing::Test {
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
        if (device_) { SDL_DestroyGPUDevice(device_); device_ = nullptr; }
        SDL_Quit();
    }
    void SetUp() override {
        if (!device_) {
            if (kDeviceOptional) {
                GTEST_SKIP() << "Windows on ARM is a courtesy runner with no production-representative GPU "
                                "backend in CI; the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This concave-silhouette harness requires a GPU device on every "
                      "production-representative platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a "
                      "software rasterizer (lavapipe / WARP) suffices; on a headless runner set "
                      "SDL_VIDEODRIVER=offscreen.";
        }
    }
};

std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b))});
}

TEST_F(ConcaveSilhouette, OpenMouthStaysUncoveredAndFillGrades) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    // Background: flat dark blue.
    const Rgba8 bgCol{10, 10, 40, 255};
    std::array<std::uint8_t, 8 * 8> bgIdx{};  // all index 0
    const AtlasId bgAtlas = r.uploadAtlas(bgIdx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> bgPal{{bgCol, bgCol, bgCol, bgCol}};
    const PaletteId bgPalId = r.uploadPalette(std::span<const Rgba8>(bgPal));

    // Horseshoe 24×24: top bar rows 0..7 (all cols); legs cols 0..7 and 16..23, rows 8..23; mouth cols
    // 8..15 rows 8..23 = index 0, open through the bottom edge and contiguous with the outside.
    std::array<std::uint8_t, 24 * 24> hs{};
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const bool body = (y < 8) || (x < 8) || (x >= 16);
            hs[static_cast<std::size_t>(y) * 24 + x] = body ? 1 : 0;
        }
    const AtlasId hsAtlas = r.uploadAtlas(hs.data(), 24, 24, TransparentIndices::of({0})).atlasId;
    const std::array<Rgba8, 4> hsPal{{{0, 0, 0, 0}, {80, 200, 120, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}}};
    const PaletteId hsPalId = r.uploadPalette(std::span<const Rgba8>(hsPal));

    const Rgba8 fillCol{240, 40, 40, 255};
    const ScreenSpaceEffect fill{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fillCol};
    const Transform scale15 = Transform::scale(1.5f, 1.5f);  // s.pivot = {12,12} centers it (no baked pivot)

    std::vector<TileCell> cells(static_cast<std::size_t>((kW / 8) * (kH / 8)),
                               TileCell{.atlas = bgAtlas, .tile = 0, .palette = bgPalId});
    DrawLayer bg{.key = "bg"};
    bg.z = 0; bg.size = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = kW / 8, .heightInTiles = kH / 8,
                             .cells = std::span<const TileCell>(cells)};

    // Straight-alpha color oracles for a leg (fully-covered interior) pixel: the plain/scaled variants draw
    // the fill opaquely (Normal), the Add variants grade it over the background at alpha 0.5.
    const Rgba8 legPlain = fillCol;
    const Vec4  addV = applyBlendMode(Vec4{bgCol.r / 255.0f, bgCol.g / 255.0f, bgCol.b / 255.0f, 1.0f},
                                      Vec4{fillCol.r / 255.0f, fillCol.g / 255.0f, fillCol.b / 255.0f, 0.5f},
                                      BlendMode::Add);
    const Rgba8 legAdd{quant(addV.x), quant(addV.y), quant(addV.z), 255};

    // Each variant renders alone over the background, centered with margin, so the content bounding box scan
    // has no neighbor to catch. The horseshoe sits at (sx0, sy0); a scaled variant inflates about pivot
    // (12,12), a footprint that stays inside the viewport.
    constexpr int sx0 = 36, sy0 = 16;
    auto checkVariant = [&](const char* name, bool add, bool xform) {
        Sprite s{.key = name, .x = sx0, .y = sy0, .atlas = hsAtlas, .tile = 0, .palette = hsPalId};
        s.size = AssetDimensions{24, 24};
        s.effects = {fill};
        if (add)   { s.blend = BlendMode::Add; s.alpha = 0.5f; }
        if (xform) { s.transform = scale15; s.pivot = Point{12, 12}; }
        std::vector<Sprite> sprites{s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};

        FrameDrawState f;
        f.layers.push_back(bg);
        f.layers.push_back(sp);

        for (const int cs : {1, 4}) {
            const std::vector<Rgba8> cap = r.captureViewport(f, cs);
            ASSERT_EQ(cap.size(), static_cast<std::size_t>(kW * cs * kH * cs));
            const int W = kW * cs;
            auto at   = [&](int x, int y) { return cap[static_cast<std::size_t>(y) * W + x]; };
            auto isBg = [&](Rgba8 c) { return c.r == bgCol.r && c.g == bgCol.g && c.b == bgCol.b; };

            // Art point a → viewport, then to the center of the scaled output cell.
            auto vx = [&](float ax) { return xform ? sx0 + 12.0f + 1.5f * (ax - 12.0f) : sx0 + ax; };
            auto vy = [&](float ay) { return xform ? sy0 + 12.0f + 1.5f * (ay - 12.0f) : sy0 + ay; };
            auto ox = [&](float v) { return static_cast<int>(v * cs) + cs / 2; };

            // coverage — the open mouth stays background.
            const Rgba8 mouth = at(ox(vx(12.0f)), ox(vy(16.0f)));  // mouth center
            const Rgba8 deep  = at(ox(vx(12.0f)), ox(vy(22.0f)));  // deep mouth, near the open edge
            EXPECT_TRUE(isBg(mouth)) << name << " cs=" << cs << ": mouth centre covered";
            EXPECT_TRUE(isBg(deep))  << name << " cs=" << cs << ": deep mouth covered";

            // color — a leg pixel matches the straight-alpha oracle for its configuration.
            const Rgba8 leg  = at(ox(vx(4.0f)), ox(vy(16.0f)));  // left leg interior
            const Rgba8 want = add ? legAdd : legPlain;
            EXPECT_FALSE(isBg(leg)) << name << " cs=" << cs << ": leg not drawn";
            EXPECT_LE(chDelta(leg, want), 2)
                << name << " cs=" << cs << ": leg colour " << int(leg.r) << "," << int(leg.g) << ","
                << int(leg.b) << " want " << int(want.r) << "," << int(want.g) << "," << int(want.b);

            // geometry — the drawn content's bounding box is the transformed quad, within a pixel. Scan the
            // whole (single-sprite) frame; the horseshoe touches all four quad edges, so its content bbox is
            // the transformed quad. A hull fill or a bridged silhouette would spill past it.
            int minX = std::numeric_limits<int>::max(), maxX = -1;
            int minY = std::numeric_limits<int>::max(), maxY = -1;
            for (int y = 0; y < kH * cs; ++y)
                for (int x = 0; x < W; ++x)
                    if (!isBg(at(x, y))) {
                        minX = std::min(minX, x); maxX = std::max(maxX, x);
                        minY = std::min(minY, y); maxY = std::max(maxY, y);
                    }
            ASSERT_GE(maxX, 0) << name << " cs=" << cs << ": no content drawn";
            const int expLo = ox(vx(0.0f)) - cs / 2,  expHi = ox(vx(23.0f)) - cs / 2 + (cs - 1);
            const int expTop = ox(vy(0.0f)) - cs / 2, expBot = ox(vy(23.0f)) - cs / 2 + (cs - 1);
            const int tol = 2 * cs;  // nearest-sampling + cell-center + inflation/trim slack
            EXPECT_NEAR(minX, expLo, tol) << name << " cs=" << cs << ": content left edge";
            EXPECT_NEAR(maxX, expHi, tol) << name << " cs=" << cs << ": content right edge";
            EXPECT_NEAR(minY, expTop, tol) << name << " cs=" << cs << ": content top edge";
            EXPECT_NEAR(maxY, expBot, tol) << name << " cs=" << cs << ": content bottom edge";
        }
    };

    checkVariant("plain", false, false);
    checkVariant("blend", true,  false);
    checkVariant("xform", false, true);
    checkVariant("full",  true,  true);
}

}  // namespace
