// A sprite's whole-silhouette Bloom / Glow halo, as the renderer draws it.
//
// The halo lives in a field on the layer's emission atlas — the sprite's footprint grown by the reach — and
// is composited over the layer's art across the field's pixels. These cases pin what a reader of the frame
// can see of that: the halo spills past the quad and fades to nothing within the reach, at full and at
// reduced resolution; two halos that meet sum before the composite applies its coverage rule, so their
// result is linear in the two halos; a Glow saturates the coverage where a Bloom only fills what is left;
// on the Viewport evaluation grid a scale-2 frame is the scale-1 frame with every pixel doubled; and every
// pixel outside the fields is the scene without the halo.
//
// Device-backed: a compose-only, windowless Renderer on a real GPU device (a software rasterizer in CI),
// bootstrapped per file as the other device-backed suites do. The field arithmetic is pinned device-free by
// sprite_halo_field_test.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"        // TransparentIndices
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode, emissionAtlasSpread
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace golden_scenes {
// The sprite_halo golden scene (golden_readback_test.cpp): two Bloom sprites on a translucent Multiply layer,
// a Glow sprite and an edge-hanging Bloom sprite on a Normal layer above, over a pale-then-dark backdrop.
retropp::FrameDrawState spriteHaloScene(retropp::Renderer& r, std::vector<retropp::TileCell>& cells,
                                        std::vector<retropp::Sprite>& translucent,
                                        std::vector<retropp::Sprite>& opaque);
}  // namespace golden_scenes

namespace {
using namespace retropp;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class SpriteHaloDevice : public ::testing::Test {
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
                                "backend in CI; the field arithmetic is pinned device-free by SpriteHaloField, "
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

constexpr int kW = 128;
constexpr int kH = 128;

struct Art {
    AtlasId   atlas{};
    PaletteId palette{};
};

// A solid sheet of one color, `side` × `side`.
Art uploadSolid(Renderer& r, Rgba8 color, int side) {
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), 0);
    const AtlasId atlas = r.uploadAtlas(idx.data(), side, side, TransparentIndices::None).atlasId;
    const std::array<Rgba8, 1> pal{{color}};
    return {.atlas = atlas, .palette = r.uploadPalette(std::span<const Rgba8>(pal))};
}

// An opaque backdrop of one color across the whole viewport.
DrawLayer backdrop(const Art& art, std::vector<TileCell>& keep) {
    keep.assign(static_cast<std::size_t>(kW / 8) * static_cast<std::size_t>(kH / 8),
                TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette});
    DrawLayer l{.key = "backdrop"};
    l.z       = 0;
    l.size    = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = kW / 8, .heightInTiles = kH / 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

Sprite solidSprite(const char* key, int x, int y, int side, const Art& art, const ScreenSpaceEffect& effect) {
    Sprite s{.key = key, .x = x, .y = y, .atlas = art.atlas, .tile = 0, .palette = art.palette};
    s.size    = AssetDimensions{side, side};
    s.effects = {effect};
    return s;
}

DrawLayer spriteLayer(const char* key, const std::vector<Sprite>& sprites, float alpha, BlendMode blend) {
    DrawLayer l{.key = key};
    l.z       = 10;
    l.size    = PixelSize{kW, kH};
    l.alpha   = alpha;
    l.blend   = blend;
    l.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
    return l;
}

Rgba8 at(const std::vector<Rgba8>& px, int width, int x, int y) {
    return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

int channelSum(Rgba8 c) { return int{c.r} + int{c.g} + int{c.b}; }

constexpr Rgba8 kDark{.r = 8, .g = 10, .b = 14, .a = 255};
constexpr Rgba8 kPale{.r = 230, .g = 230, .b = 240, .a = 255};
constexpr Rgba8 kWarm{.r = 120, .g = 110, .b = 90, .a = 255};

ScreenSpaceEffect bloom(float radius) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = radius, .intensity = 255};
}

// ── Cases ───────────────────────────────────────────────────────────────────────────────────────────

// One 16×16 Bloom sprite on an opaque Normal layer over a dark backdrop. The halo lights pixels past the
// quad, is gone well beyond the reach, and lands over the art rather than under it: an art pixel on the
// quad's edge reads brighter than the art's own color. The reach-12 half runs on the reduced path.
TEST_F(SpriteHaloDevice, AHaloSpillsPastTheQuad) {
    struct Probe {
        float radius;
        int   farOffset;
    };
    for (const Probe probe : {Probe{6.0f, 20}, Probe{12.0f, 40}}) {
        Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
        const Art dark = uploadSolid(r, kDark, 8);
        const Art warm = uploadSolid(r, kWarm, 16);
        std::vector<TileCell> cells;
        const std::vector<Sprite> sprites{solidSprite("lamp", 40, 56, 16, warm, bloom(probe.radius))};
        FrameDrawState frame;
        frame.layers.push_back(backdrop(dark, cells));
        frame.layers.push_back(spriteLayer("lamp", sprites, 1.0f, BlendMode::Normal));
        const std::vector<Rgba8> px = r.captureViewport(frame);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);

        const int right = 40 + 16 - 1;  // the quad's last column
        const int row   = 56 + 8;
        EXPECT_GT(channelSum(at(px, kW, right + 3, row)), channelSum(kDark) + 6)
            << "radius " << probe.radius << ": no light 3 px past the quad — the field does not hold the spill";
        const Rgba8 far = at(px, kW, right + probe.farOffset, row);
        EXPECT_EQ(far.r, kDark.r);
        EXPECT_EQ(far.g, kDark.g);
        EXPECT_EQ(far.b, kDark.b) << "radius " << probe.radius << ": light far beyond the reach";
        const Rgba8 edge = at(px, kW, right, row);
        EXPECT_GT(channelSum(edge), channelSum(kWarm) + 6)
            << "radius " << probe.radius << ": the art's edge pixel carries no halo — the art covers it";
    }
}

// Two Bloom sprites 2 px apart on a translucent Multiply layer, rendered alone and together. Between them
// the layer's isolated image is the halo alone (no art), so its coverage is what the composite's rule gives.
// The halos sum in one field and the rule is applied once: the pair's result is linear in the two halos,
// out(pair) + backdrop == out(left) + out(right). Composited one after the other, the coverage would be
// a + b·(1 − a), and the pair would read darker by backdrop · a · b.
TEST_F(SpriteHaloDevice, TwoOverlappingHalosOnATranslucentLayerFollowTheCoverageRule) {
    const ScreenSpaceEffect effect = bloom(6.0f);
    auto capture = [&](bool left, bool right) {
        Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
        const Art pale   = uploadSolid(r, kPale, 8);
        const Art bright = uploadSolid(r, Rgba8{.r = 255, .g = 240, .b = 200, .a = 255}, 16);
        std::vector<TileCell> cells;
        std::vector<Sprite>   sprites;
        if (left) sprites.push_back(solidSprite("left", 30, 40, 16, bright, effect));
        if (right) sprites.push_back(solidSprite("right", 48, 40, 16, bright, effect));
        FrameDrawState frame;
        frame.layers.push_back(backdrop(pale, cells));
        frame.layers.push_back(spriteLayer("pair", sprites, 0.5f, BlendMode::Multiply));
        return r.captureViewport(frame);
    };
    const std::vector<Rgba8> both = capture(true, true);
    const std::vector<Rgba8> left = capture(true, false);
    const std::vector<Rgba8> right = capture(false, true);

    // The gap between the two quads, at their mid-height.
    for (const int x : {46, 47}) {
        const int   y = 48;
        const Rgba8 b = at(both, kW, x, y), l = at(left, kW, x, y), rr = at(right, kW, x, y);
        ASSERT_NE(channelSum(l), channelSum(kPale)) << "the left halo does not reach the gap";
        ASSERT_NE(channelSum(rr), channelSum(kPale)) << "the right halo does not reach the gap";
        const std::array<int, 3> lhs{int{b.r} + kPale.r, int{b.g} + kPale.g, int{b.b} + kPale.b};
        const std::array<int, 3> rhs{int{l.r} + rr.r, int{l.g} + rr.g, int{l.b} + rr.b};
        for (std::size_t c = 0; c < 3; ++c)
            EXPECT_LE(std::abs(lhs[c] - rhs[c]), 2)
                << "(" << x << ", " << y << ") channel " << c << ": the pair is not the sum of its halos ("
                << lhs[c] << " vs " << rhs[c] << ") — the coverage rule was applied once per halo";
    }
}

// A Glow lifts coverage directly and saturates it; a Bloom only fills what the art leaves. At the center of a
// 16×16 sprite every tap of a radius-6 kernel lands on the art, so the field there is the sprite's whole
// emission, and the layer's alpha scales both art and emission: art (0.6·c, 0.6), emission (0.6·tint, 0.6).
// The Glow's coverage is saturate(0.6 + 0.6) = 1; the Multiply layer then reads backdrop · rgb.
TEST_F(SpriteHaloDevice, AGlowSaturatesWhereABloomWouldNot) {
    constexpr float kLayerAlpha = 0.6f;
    constexpr Rgba8 kDim{.r = 26, .g = 26, .b = 26, .a = 255};
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art pale = uploadSolid(r, kPale, 8);
    const Art dim  = uploadSolid(r, kDim, 16);
    std::vector<TileCell> cells;
    const ScreenSpaceEffect glow{.kind      = ScreenSpaceEffectKind::Glow,
                                 .fill      = Rgba8{.r = 255, .g = 255, .b = 255, .a = 255},
                                 .radius    = 6.0f,
                                 .threshold = 0,
                                 .intensity = 255};
    const std::vector<Sprite> sprites{solidSprite("ember", 56, 56, 16, dim, glow)};
    FrameDrawState frame;
    frame.layers.push_back(backdrop(pale, cells));
    frame.layers.push_back(spriteLayer("ember", sprites, kLayerAlpha, BlendMode::Multiply));
    const std::vector<Rgba8> px = r.captureViewport(frame);

    auto channel = [](std::uint8_t v) { return static_cast<float>(v) / 255.0f; };
    const Vec4 d{channel(kPale.r), channel(kPale.g), channel(kPale.b), 1.0f};
    auto expect = [&](bool glowRule) {
        const float rgb = kLayerAlpha * channel(kDim.r) + kLayerAlpha;
        const float a   = glowRule ? std::min(1.0f, kLayerAlpha + kLayerAlpha)
                                   : kLayerAlpha + kLayerAlpha * (1.0f - kLayerAlpha);
        return applyBlendMode(d, Vec4{rgb / a, rgb / a, rgb / a, a}, BlendMode::Multiply);
    };
    const Vec4  want      = expect(true);
    const Vec4  bloomRule = expect(false);
    const Rgba8 got       = at(px, kW, 56 + 8, 56 + 8);
    EXPECT_NEAR(got.r, want.x * 255.0f, 2.0f) << "the Glow's coverage did not saturate";
    EXPECT_NEAR(got.g, want.y * 255.0f, 2.0f);
    EXPECT_NEAR(got.b, want.z * 255.0f, 2.0f);
    EXPECT_GT(std::abs(bloomRule.x - want.x) * 255.0f, 10.0f) << "the scene does not separate the two rules";
}

// On the Viewport evaluation grid a field is read at the viewport cell's center, so a scale-2 capture of the
// sprite_halo scene is the scale-1 capture with every pixel doubled — halos, art and backdrop alike.
TEST_F(SpriteHaloDevice, TheHaloAtScaleTwoIsTheScaleOnePixelUpsampled) {
    constexpr int kSceneW = 64;
    Renderer r{device_, nullptr, ViewportResolution{kSceneW, kSceneW}};
    std::vector<TileCell> cells;
    std::vector<Sprite>   translucent, opaque;
    const FrameDrawState  frame = golden_scenes::spriteHaloScene(r, cells, translucent, opaque);
    const std::vector<Rgba8> one = r.captureViewport(frame, 1);
    const std::vector<Rgba8> two = r.captureViewport(frame, 2);
    ASSERT_EQ(one.size(), static_cast<std::size_t>(kSceneW) * kSceneW);
    ASSERT_EQ(two.size(), static_cast<std::size_t>(kSceneW) * kSceneW * 4);
    int mismatches = 0;
    for (int y = 0; y < kSceneW * 2; ++y) {
        for (int x = 0; x < kSceneW * 2; ++x) {
            const Rgba8 a = at(one, kSceneW, x / 2, y / 2);
            const Rgba8 b = at(two, kSceneW * 2, x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
                if (mismatches++ < 4)
                    ADD_FAILURE() << "(" << x << ", " << y << ") at scale 2 reads " << int{b.r} << "," << int{b.g}
                                  << "," << int{b.b} << "; the scale-1 pixel is " << int{a.r} << "," << int{a.g}
                                  << "," << int{a.b};
            }
        }
    }
    EXPECT_EQ(mismatches, 0);
}

// The halo composite writes its fields' pixels and nothing else: every pixel outside the sprite's quad grown
// by ceil(emissionAtlasSpread(radius)) is exactly the scene drawn without the halo, and some pixel inside it
// is not.
TEST_F(SpriteHaloDevice, PixelsOutsideTheFieldsAreTheSceneWithoutTheHalo) {
    constexpr int   kX = 50, kY = 30, kSide = 16;
    constexpr float kRadius = 6.0f;
    auto capture = [&](bool halo) {
        Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
        const Art dark = uploadSolid(r, kDark, 8);
        const Art warm = uploadSolid(r, kWarm, kSide);
        std::vector<TileCell> cells;
        Sprite s = solidSprite("lamp", kX, kY, kSide, warm, bloom(kRadius));
        if (!halo) s.effects.clear();
        const std::vector<Sprite> sprites{s};
        FrameDrawState frame;
        frame.layers.push_back(backdrop(dark, cells));
        frame.layers.push_back(spriteLayer("lamp", sprites, 1.0f, BlendMode::Normal));
        return r.captureViewport(frame);
    };
    const std::vector<Rgba8> lit   = capture(true);
    const std::vector<Rgba8> plain = capture(false);
    const int g = static_cast<int>(std::ceil(emissionAtlasSpread(kRadius)));
    int outsideDiffers = 0, insideDiffers = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool  inside = x >= kX - g && x < kX + kSide + g && y >= kY - g && y < kY + kSide + g;
            const Rgba8 a = at(lit, kW, x, y), b = at(plain, kW, x, y);
            const bool  same = a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
            if (!same) ++(inside ? insideDiffers : outsideDiffers);
        }
    }
    EXPECT_EQ(outsideDiffers, 0) << "the halo reached a pixel outside its field";
    EXPECT_GT(insideDiffers, 0) << "the halo lit nothing";
}

}  // namespace
