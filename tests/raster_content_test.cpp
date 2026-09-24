// A raster as a layer's content — the submission surface.
//
// The first cases run with no machine and no device: what they pin is that a raster is a content
// alternative like any other, that dispatch over the content variant names it correctly, and that the
// type carries plain values and nothing a console brought with it.
//
// The rest are device-backed (a GPU device, no display — the harness the golden-readback and compose-skip
// tests use, so they run on a software rasterizer in CI): a raster reaches the screen at the pixels its
// source drew, composites at its own z among native layers, re-uploads only when its generation moved,
// and fills the size its `fit` names — its own when the fit is unset, texel for texel.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {

using namespace retropp;

// ── Dispatch over the content variant ───────────────────────────────────────────────────────────

// A raster reports itself as one, and specifically NOT as the alternative that sat last in the
// variant before it did. Dispatch over the content variant runs on this answer everywhere in the
// platform, so an alternative that reports the wrong kind is drawn as the wrong kind.
TEST(RasterContentType, ContentKindReportsRasterNotSprites) {
    const LayerContent content{RasterContent{}};
    EXPECT_EQ(contentKind(content), LayerContentKind::Raster);
    EXPECT_NE(contentKind(content), LayerContentKind::Sprites);
}

// The other two alternatives keep the kind they already reported.
TEST(RasterContentType, ContentKindStillReportsTilesAndSprites) {
    EXPECT_EQ(contentKind(LayerContent{TileContent{}}), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(LayerContent{SpriteContent{}}), LayerContentKind::Sprites);
}

// ── What the type carries ───────────────────────────────────────────────────────────────────────

// Every member is a plain value: a byte span, dimensions, a declared layout, and the platform's answer
// to whether the machine drew something new. Nothing here is a handle to a core, a machine, or a
// console's own vocabulary — which is what keeps a second core implementing this surface rather than
// inheriting the first one's.
TEST(RasterContentType, TheContentTypeNamesNoConsole) {
    static_assert(std::is_same_v<decltype(RasterContent::pixels), std::span<const std::uint8_t>>);
    static_assert(std::is_same_v<decltype(RasterContent::width), int>);
    static_assert(std::is_same_v<decltype(RasterContent::height), int>);
    static_assert(std::is_same_v<decltype(RasterContent::format), RasterPixelFormat>);
    static_assert(std::is_same_v<decltype(RasterContent::generation), std::uint64_t>);
    static_assert(std::is_same_v<decltype(RasterContent::fit), PixelSize>);
    static_assert(std::is_same_v<std::underlying_type_t<RasterPixelFormat>, std::uint8_t>);
    static_assert(std::is_standard_layout_v<RasterContent>);
    SUCCEED();
}

TEST(RasterContentType, ARasterLayerCarriesItsDimensionsAndFormat) {
    const std::vector<std::uint8_t> pixels(4 * 2 * 4, 0x40);
    DrawLayer layer{.key = "screen"};
    layer.content = RasterContent{.pixels = std::span<const std::uint8_t>(pixels),
                                  .width  = 4,
                                  .height = 2,
                                  .format = RasterPixelFormat::Rgba8888,
                                  .generation = 1};

    const RasterContent& gc = std::get<RasterContent>(layer.content);
    EXPECT_EQ(gc.width, 4);
    EXPECT_EQ(gc.height, 2);
    EXPECT_EQ(gc.format, RasterPixelFormat::Rgba8888);
    EXPECT_EQ(gc.pixels.size(), static_cast<std::size_t>(gc.width) *
                                    static_cast<std::size_t>(gc.height) * bytesPerPixel(gc.format));
}

// A default-constructed raster is a valid, degenerate submission — nothing drawn, the same way an
// empty sprite span is.
TEST(RasterContentType, AnEmptyRasterIsAValidSubmission) {
    const RasterContent empty{};
    EXPECT_EQ(empty.width, 0);
    EXPECT_EQ(empty.height, 0);
    EXPECT_TRUE(empty.pixels.empty());
    EXPECT_EQ(empty.generation, 0u);
    EXPECT_EQ(empty.fit, PixelSize{});  // shown at its own size
}

// ── Device-backed: what reaches the screen ──────────────────────────────────────────────────────

constexpr int kW = 16;
constexpr int kH = 8;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

// A picture the width of the viewport whose left half is red and right half is blue — a payload that
// reads differently backwards, so a picture placed mirrored or offset is not mistaken for a match.
std::vector<std::uint8_t> splitPicture() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4;
            const bool        left = x < kW / 2;
            pixels[at + 0] = left ? 0xFF : 0x00;  // red
            pixels[at + 1] = 0x00;                // green
            pixels[at + 2] = left ? 0x00 : 0xFF;  // blue
            pixels[at + 3] = 0xFF;                // alpha
        }
    }
    return pixels;
}

class RasterRenderTest : public ::testing::Test {
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

// The raster reaches the screen as its source drew it: the same colors, at the same places, with no
// filtering between the source's texels and the viewport's pixels.
TEST_F(RasterRenderTest, ARasterLayerRendersThePixelsItWasGiven) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();

    DrawLayer screen{.key = "screen"};
    screen.z    = 0;
    screen.size = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);

    const Rgba8 topLeft  = px[0];
    const Rgba8 topRight = px[static_cast<std::size_t>(kW) - 1];
    EXPECT_EQ(topLeft.r, 0xFF);
    EXPECT_EQ(topLeft.b, 0x00);
    EXPECT_EQ(topRight.r, 0x00);
    EXPECT_EQ(topRight.b, 0xFF);
    // The boundary lands where the machine put it, not a texel either side of it.
    EXPECT_EQ(px[static_cast<std::size_t>(kW) / 2 - 1].r, 0xFF);
    EXPECT_EQ(px[static_cast<std::size_t>(kW) / 2].b, 0xFF);
}

// A picture composites by z among native layers like any other content: a tile layer above it covers
// it, and the same tile layer below it does not.
TEST_F(RasterRenderTest, ARasterLayerComposesAtItsOwnZ) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);

    // A solid one-color 8×8 sheet and its palette — the native layer that argues over the picture.
    std::array<std::uint8_t, 8 * 8> idx{};
    idx.fill(1);
    const AtlasId               atlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2>  pal{{{0, 0, 0}, {0, 255, 0}}};
    const PaletteId             palette = r.uploadPalette(std::span<const Rgba8>(pal));
    std::vector<TileCell>       cells(4 * 2, TileCell{.atlas = atlas, .tile = 0, .palette = palette});

    const std::vector<std::uint8_t> picture = splitPicture();
    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .generation = 1};

    DrawLayer native{.key = "native"};
    native.size    = PixelSize{kW, kH};
    native.content = TileContent{.widthInTiles  = 4,
                                 .heightInTiles = 2,
                                 .cells         = std::span<const TileCell>(cells)};

    // The native layer above: green covers the picture.
    screen.z = 0;
    native.z = 10;
    FrameDrawState above;
    above.layers = {screen, native};
    const std::vector<Rgba8> covered = r.captureViewport(above);
    ASSERT_EQ(covered.size(), static_cast<std::size_t>(kW) * kH);
    EXPECT_EQ(covered[0].g, 0xFF);
    EXPECT_EQ(covered[0].r, 0x00);

    // The same native layer below: the picture is what shows.
    screen.z = 10;
    native.z = 0;
    FrameDrawState below;
    below.layers = {native, screen};
    const std::vector<Rgba8> showing = r.captureViewport(below);
    ASSERT_EQ(showing.size(), static_cast<std::size_t>(kW) * kH);
    EXPECT_EQ(showing[0].r, 0xFF);
    EXPECT_EQ(showing[0].g, 0x00);
}

// Two machines on one screen: each picture is one screen wide, both layers span the whole viewport, and
// the right-hand one is placed by scrolling its content a screen to the left. Outside its own dimensions
// a picture draws nothing, so the two halves meet without overlapping — which is what lets a viewport
// two screens wide show two machines rather than one stretched across it.
TEST_F(RasterRenderTest, TwoPicturesSitSideBySideWhenOneIsScrolled) {
    constexpr int kWideW = kW * 2;
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kWideW, kH}};
    r.automaticInterpolation(false);

    // Two pictures that cannot be mistaken for one another, nor for a half of the other.
    auto solid = [](std::uint8_t red, std::uint8_t blue) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4);
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            pixels[at + 0] = red;
            pixels[at + 1] = 0x00;
            pixels[at + 2] = blue;
            pixels[at + 3] = 0xFF;
        }
        return pixels;
    };
    const std::vector<std::uint8_t> onLeft  = solid(0xFF, 0x00);
    const std::vector<std::uint8_t> onRight = solid(0x00, 0xFF);

    auto layerFor = [&](const char* key, std::int32_t z, int scrollX,
                        const std::vector<std::uint8_t>& pixels) {
        DrawLayer layer{.key = ObjectKey{key}};
        layer.z       = z;
        layer.size    = PixelSize{kWideW, kH};
        layer.scroll  = LayerScroll{scrollX, 0};
        layer.content = RasterContent{.pixels = std::span<const std::uint8_t>(pixels),
                                      .width  = kW,
                                      .height = kH,
                                      .format = RasterPixelFormat::Rgba8888,
                                      .generation = 1};
        return layer;
    };

    FrameDrawState frame;
    frame.layers = {layerFor("left", 0, 0, onLeft), layerFor("right", 1, -kW, onRight)};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kWideW) * kH);

    // Both edges of each half, so a picture placed one texel off is caught rather than averaged over.
    EXPECT_EQ(px[0].r, 0xFF);                                              // left half, first column
    EXPECT_EQ(px[static_cast<std::size_t>(kW) - 1].r, 0xFF);               // left half, last column
    EXPECT_EQ(px[static_cast<std::size_t>(kW)].b, 0xFF);                   // right half, first column
    EXPECT_EQ(px[static_cast<std::size_t>(kWideW) - 1].b, 0xFF);           // right half, last column
    EXPECT_EQ(px[static_cast<std::size_t>(kW) - 1].b, 0x00);               // and they do not bleed
    EXPECT_EQ(px[static_cast<std::size_t>(kW)].r, 0x00);
}

// ── The fit ─────────────────────────────────────────────────────────────────────────────────────

// A picture whose every color byte is a distinct pattern, so a texel landing one place off is caught —
// and opaque, so every pixel reaches the capture as the color it holds rather than blended over what
// is under it.
std::vector<std::uint8_t> distinctPicture(int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (i % 4 == 3) ? 0xFF : static_cast<std::uint8_t>(i * 7 + 3);
    }
    return pixels;
}

Rgba8 texelOf(const std::vector<std::uint8_t>& pixels, int width, int x, int y) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
    return Rgba8{pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

// The default: every viewport pixel is the raster's own texel — the whole raster, not a sample of it.
// Nothing a consumer submits today names a fit, so this is the output every consumer keeps.
TEST_F(RasterRenderTest, AnUnsetFitShowsTheRasterAtItsOwnSizeTexelForTexel) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = distinctPicture(kW, kH);

    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 want = texelOf(picture, kW, x, y);
            const Rgba8 got  = px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
            EXPECT_EQ(got.r, want.r) << "at " << x << "," << y;
            EXPECT_EQ(got.g, want.g) << "at " << x << "," << y;
            EXPECT_EQ(got.b, want.b) << "at " << x << "," << y;
        }
    }
}

// A raster smaller than its fit repeats texels: each covers fit/raster pixels on each axis.
TEST_F(RasterRenderTest, AFitStretchesTheRasterToFillIt) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    constexpr int kSmallW = kW / 4, kSmallH = kH / 4;   // 4×2, shown 16×8: each texel a 4×4 block
    const std::vector<std::uint8_t> picture = distinctPicture(kSmallW, kSmallH);

    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kSmallW,
                                   .height = kSmallH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .fit    = PixelSize{kW, kH},
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba8 want = texelOf(picture, kSmallW, x / 4, y / 4);
            const Rgba8 got  = px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
            EXPECT_EQ(got.r, want.r) << "at " << x << "," << y;
            EXPECT_EQ(got.b, want.b) << "at " << x << "," << y;
        }
    }
}

// A raster larger than its fit drops texels: fitted to half its width, every shown column is an even
// source column, and the half of the slot the fit does not reach shows nothing.
TEST_F(RasterRenderTest, AFitShrinksTheRasterByDroppingTexels) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);

    // Even columns red, odd columns blue — a sample of the wrong column changes color.
    std::vector<std::uint8_t> picture(static_cast<std::size_t>(kW) * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4;
            picture[at + 0] = (x % 2 == 0) ? 0xFF : 0x00;
            picture[at + 1] = 0x00;
            picture[at + 2] = (x % 2 == 0) ? 0x00 : 0xFF;
            picture[at + 3] = 0xFF;
        }
    }

    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .fit    = PixelSize{kW / 2, kH},
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int x = 0; x < kW / 2; ++x) {
        EXPECT_EQ(px[static_cast<std::size_t>(x)].r, 0xFF) << "column " << x;   // column 2x, red
        EXPECT_EQ(px[static_cast<std::size_t>(x)].b, 0x00) << "column " << x;
    }
    for (int x = kW / 2; x < kW; ++x) {
        EXPECT_EQ(px[static_cast<std::size_t>(x)].r, 0x00) << "column " << x;   // outside the fit
        EXPECT_EQ(px[static_cast<std::size_t>(x)].b, 0x00) << "column " << x;
    }
}

// The fit is placed by scroll as any raster is: the same half-width picture, scrolled a half viewport
// to the left, shows in the right half and leaves the left showing nothing.
TEST_F(RasterRenderTest, AFitIsPlacedByScrollLikeAnyRaster) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();   // left half red, right half blue

    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.scroll  = LayerScroll{-kW / 2, 0};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .fit    = PixelSize{kW / 2, kH},
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    EXPECT_EQ(px[0].r, 0x00);                                          // left half: nothing
    EXPECT_EQ(px[0].b, 0x00);
    EXPECT_EQ(px[static_cast<std::size_t>(kW) / 2].r, 0xFF);           // the fitted picture's first column
    EXPECT_EQ(px[static_cast<std::size_t>(kW) / 2 + kW / 4 - 1].r, 0xFF);   // its red half's last column
    EXPECT_EQ(px[static_cast<std::size_t>(kW) / 2 + kW / 4].b, 0xFF);       // its blue half's first
    EXPECT_EQ(px[static_cast<std::size_t>(kW) - 1].b, 0xFF);           // its last column
}

// An axis left at 0 keeps the raster's own size on that axis: a fit naming only a width halves the width
// and leaves every row where it was.
TEST_F(RasterRenderTest, AnAxisLeftAtZeroKeepsTheRastersOwnSizeOnThatAxis) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = distinctPicture(kW, kH);

    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                   .width  = kW,
                                   .height = kH,
                                   .format = RasterPixelFormat::Rgba8888,
                                   .fit    = PixelSize{kW / 2, 0},
                                   .generation = 1};
    FrameDrawState frame;
    frame.layers = {screen};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW / 2; ++x) {
            const Rgba8 want = texelOf(picture, kW, x * 2, y);   // every other column, the same row
            const Rgba8 got  = px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
            EXPECT_EQ(got.r, want.r) << "at " << x << "," << y;
            EXPECT_EQ(got.g, want.g) << "at " << x << "," << y;
        }
    }
}

// A changed fit is a different picture on screen, so a frame identical in every other way recomposes
// rather than reusing the retained output. renderFrame is the path that skips (captureViewport always
// composes), and with no window it composes offscreen and skips the blit — the compose-skip tests' harness.
TEST_F(RasterRenderTest, AChangedFitRecomposes) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();

    DrawLayer screen{.key = "screen"};
    screen.size = PixelSize{kW, kH};
    auto submit = [&](PixelSize fit) {
        screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                       .width  = kW,
                                       .height = kH,
                                       .format = RasterPixelFormat::Rgba8888,
                                       .fit    = fit,
                                       .generation = 1};
        FrameDrawState frame;
        frame.layers = {screen};
        r.renderFrame(frame);
    };

    submit(PixelSize{});             // composes (composePasses 1)
    submit(PixelSize{});             // identical: skips (composeSkips 1)
    ASSERT_EQ(r.renderStats().composePasses, 1u);
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    submit(PixelSize{kW / 2, kH});   // the fit moved: composed again
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    submit(PixelSize{kW / 2, kH});   // identical again: skips again
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 2u);
}

// The generation is the whole upload decision: a submission carrying one the resident texture does not
// hold sends the raster, and one carrying the generation already up there sends nothing. The pixels are
// never hashed to decide — a full-color raster differs every time the machine draws.
TEST_F(RasterRenderTest, TheRendererUploadsWhenTheGenerationMovesAndNotOtherwise) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();

    DrawLayer screen{.key = "screen"};
    screen.z    = 0;
    screen.size = PixelSize{kW, kH};
    auto submit = [&](std::uint64_t generation) {
        screen.content = RasterContent{.pixels = std::span<const std::uint8_t>(picture),
                                       .width  = kW,
                                       .height = kH,
                                       .format = RasterPixelFormat::Rgba8888,
                                       .generation = generation};
        FrameDrawState frame;
        frame.layers = {screen};
        (void)r.captureViewport(frame);
    };

    submit(1);
    const Renderer::RenderStats first = r.renderStats();
    EXPECT_EQ(first.rasterUploads, 1u);
    EXPECT_EQ(first.rasterSkips, 0u);

    submit(1);
    const Renderer::RenderStats held = r.renderStats();
    EXPECT_EQ(held.rasterUploads, 1u);  // the same frame — nothing sent
    EXPECT_EQ(held.rasterSkips, 1u);

    submit(2);
    const Renderer::RenderStats again = r.renderStats();
    EXPECT_EQ(again.rasterUploads, 2u);  // the machine drew — it is sent
    EXPECT_EQ(again.rasterSkips, 1u);

    // A generation that jumps — several frames finished between two submissions — is one upload, not
    // one per frame that went by.
    submit(9);
    const Renderer::RenderStats jumped = r.renderStats();
    EXPECT_EQ(jumped.rasterUploads, 3u);
    EXPECT_EQ(jumped.rasterSkips, 1u);
}

}  // namespace
