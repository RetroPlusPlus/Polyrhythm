// An image the renderer holds, shown by a raster layer that names it.
//
// A truecolor image is uploaded once and a raster layer shows it by naming its ImageId: at the texels
// the image carries, at the depth its pixels need, compositing by its own alpha, filling the size its
// `fit` names, and uploading nothing on any frame. The pixels form of a raster is untouched beside it,
// and an image named in its place outranks the pixels the layer also carries.
//
// The first case runs with no device: it pins the handle and the field. The rest are device-backed (a
// GPU device, no display — the harness the raster, golden-readback and compose-skip tests use, so they
// run on a software rasterizer in CI). The PNG fixtures and their exact planes are authored by
// tests/fixtures/gen_fixtures.py; RETROPP_FIXTURES_DIR points at that directory at build time.

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/asset_registry.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/palette.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {

using namespace retropp;

// ── The handle and the field ────────────────────────────────────────────────────────────────────

// ImageId is a 32-bit handle whose default names no image, and the field it fills keeps RasterContent
// a plain standard-layout aggregate — so a layer that names an image is written with that one field and
// every other one defaulted, and a RasterContent that leaves it unset is the raster its pixels carry.
TEST(ImageLayerType, ImageIdIsAHandleAndTheFieldDefaultsToNone) {
    static_assert(std::is_same_v<std::underlying_type_t<ImageId>, std::uint32_t>);
    static_assert(std::is_same_v<decltype(RasterContent::image), ImageId>);
    static_assert(std::is_standard_layout_v<RasterContent>);
    EXPECT_EQ(RasterContent{}.image, ImageId{});
    EXPECT_EQ(contentKind(LayerContent{RasterContent{.image = ImageId{1}}}), LayerContentKind::Raster);
}

// ── Device-backed ───────────────────────────────────────────────────────────────────────────────

constexpr int kW = 16;
constexpr int kH = 8;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << "could not open fixture: " << path;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string fixture(const char* name) { return std::string{RETROPP_FIXTURES_DIR} + "/" + name; }

// rgb8.png's exact 2×2 plane (gen_fixtures.py RGB8_2x2), row-major; an RGB source is opaque.
constexpr std::array<Rgba8, 4> kRgb8{{{0, 0, 0}, {255, 128, 0}, {12, 34, 56}, {78, 90, 255}}};

// rgba8.png's exact 2×2 plane (gen_fixtures.py RGBA8_2x2), row-major: black and white opaque, one
// pixel at alpha 40, one fully transparent.
constexpr std::array<Rgba8, 4> kRgba8{{{0, 0, 0, 255}, {255, 255, 255, 255}, {10, 20, 30, 40}, {200, 100, 50, 0}}};

// A picture whose every color byte is a distinct pattern, so a texel landing one place off is caught —
// and opaque, so every pixel reaches the capture as the color it holds.
std::vector<Rgba8> distinctPicture(int width, int height) {
    std::vector<Rgba8> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = Rgba8{.r = static_cast<std::uint8_t>(i * 7 + 3),
                          .g = static_cast<std::uint8_t>(i * 13 + 10),
                          .b = static_cast<std::uint8_t>(i * 29 + 17),
                          .a = 255};
    }
    return pixels;
}

Rgba8 texelOf(const std::vector<Rgba8>& pixels, int width, int x, int y) {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
}

std::vector<Rgba8> solidPicture(int width, int height, Rgba8 color) {
    return std::vector<Rgba8>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), color);
}

// A layer the size of the viewport naming `image`, placed at the origin.
DrawLayer imageLayer(const char* key, std::int32_t z, int width, int height, ImageId image,
                     PixelSize fit = {}) {
    DrawLayer layer{.key = ObjectKey{key}};
    layer.z       = z;
    layer.size    = PixelSize{width, height};
    layer.content = RasterContent{.fit = fit, .image = image};
    return layer;
}

class ImageLayerTest : public ::testing::Test {
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

    // A solid one-color tile layer covering a `width` × `height` viewport: an 8×8 sheet whose every index
    // is 1, under a palette whose entry 1 is `color`.
    struct Backdrop {
        std::vector<TileCell> cells;
        DrawLayer             layer;
    };
    static Backdrop backdropLayer(Renderer& r, const char* key, std::int32_t z, int width, int height,
                                  Rgba8 color) {
        std::array<std::uint8_t, 8 * 8> idx{};
        idx.fill(1);
        const AtlasId              atlas   = r.uploadAtlas(idx.data(), 8, 8).atlasId;
        const std::array<Rgba8, 2> pal{{{0, 0, 0}, color}};
        const PaletteId            palette = r.uploadPalette(std::span<const Rgba8>(pal));
        const int                  tilesW  = (width + 7) / 8;
        const int                  tilesH  = (height + 7) / 8;

        Backdrop b{.cells = std::vector<TileCell>(
                       static_cast<std::size_t>(tilesW) * static_cast<std::size_t>(tilesH),
                       TileCell{.atlas = atlas, .tile = 0, .palette = palette}),
                   .layer = DrawLayer{.key = ObjectKey{key}}};
        b.layer.z     = z;
        b.layer.size  = PixelSize{width, height};
        b.layer.content = TileContent{.widthInTiles  = tilesW,
                                      .heightInTiles = tilesH,
                                      .cells         = std::span<const TileCell>(b.cells)};
        return b;
    }
};

void expectRgb(const Rgba8& got, const Rgba8& want, int x, int y) {
    EXPECT_EQ(got.r, want.r) << "at " << x << "," << y;
    EXPECT_EQ(got.g, want.g) << "at " << x << "," << y;
    EXPECT_EQ(got.b, want.b) << "at " << x << "," << y;
}

// The layer shows the image at the texels it was uploaded with: every viewport pixel is the image's own
// texel, at its own place, with nothing between them.
TEST_F(ImageLayerTest, AnImageLayerRendersTheTexelsItWasUploaded) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<Rgba8> picture = distinctPicture(kW, kH);
    const ImageId            image   = r.uploadImage(std::span<const Rgba8>(picture), kW, kH);

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, kW, kH, image)};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            expectRgb(px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)],
                      texelOf(picture, kW, x, y), x, y);
        }
    }
}

// A PNG's bytes load as an image and draw the colors the PNG holds.
TEST_F(ImageLayerTest, AnImageAssetLoadsFromPngBytesAndDrawsItsPixels) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{2, 2}};
    r.automaticInterpolation(false);
    const ImageId image = r.loadImageFromMemory(readFile(fixture("rgb8.png")));

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, 2, 2, image)};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        expectRgb(px[i], kRgb8[i], static_cast<int>(i % 2), static_cast<int>(i / 2));
    }
}

// Alpha travels per pixel: an opaque texel covers what is beneath, a transparent one shows it, and a
// partly transparent one blends over it by its own alpha.
TEST_F(ImageLayerTest, AnImagesAlphaBlendsOverTheLayerBeneath) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{2, 2}};
    r.automaticInterpolation(false);
    const Rgba8    green{.r = 0, .g = 255, .b = 0};
    const Backdrop below = backdropLayer(r, "below", 0, 2, 2, green);
    const ImageId  image = r.loadImageFromMemory(readFile(fixture("rgba8.png")));

    FrameDrawState frame;
    frame.layers = {below.layer, imageLayer("image", 10, 2, 2, image)};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), 4u);

    expectRgb(px[0], Rgba8{0, 0, 0}, 0, 0);         // opaque black
    expectRgb(px[1], Rgba8{255, 255, 255}, 1, 0);   // opaque white
    expectRgb(px[3], green, 1, 1);                  // alpha 0: the backdrop

    // Alpha 40 over the backdrop: each channel is c · 40/255 + backdrop · 215/255.
    const Rgba8 src = kRgba8[2];
    auto        over = [](int c, int dst) {
        return static_cast<int>(std::lround(c * 40.0 / 255.0 + dst * 215.0 / 255.0));
    };
    EXPECT_NEAR(px[2].r, over(src.r, green.r), 1);
    EXPECT_NEAR(px[2].g, over(src.g, green.g), 1);
    EXPECT_NEAR(px[2].b, over(src.b, green.b), 1);
}

// A 16-bit image keeps its 16 bits: 0xFF00 is not an 8-bit value widened, so it reads back as 254 — a
// crush to its high byte would read 255.
TEST_F(ImageLayerTest, A16BitImageKeepsItsDepth) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<Rgba16> picture(static_cast<std::size_t>(kW) * kH,
                                      Rgba16{.r = 0xFF00, .g = 0x8080, .b = 0xFFFF, .a = 0xFFFF});
    const ImageId image = r.uploadImage(std::span<const Rgba16>(picture), kW, kH);

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, kW, kH, image)};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (std::size_t i = 0; i < px.size(); ++i) {
        EXPECT_EQ(px[i].r, 254) << "pixel " << i;
        EXPECT_EQ(px[i].g, 128) << "pixel " << i;
        EXPECT_EQ(px[i].b, 255) << "pixel " << i;
    }
}

// An 8-bit picture handed over widened to 16 bits draws exactly what the 8-bit picture draws.
TEST_F(ImageLayerTest, AnEightBitImageUploadedAs16BitDrawsTheSame) {
    const std::vector<Rgba8> picture = distinctPicture(kW, kH);
    std::vector<Rgba16>      widened;
    widened.reserve(picture.size());
    for (const Rgba8 c : picture) widened.push_back(widen(c));

    Renderer eight{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    eight.automaticInterpolation(false);
    const ImageId a = eight.uploadImage(std::span<const Rgba8>(picture), kW, kH);
    FrameDrawState frameA;
    frameA.layers = {imageLayer("image", 0, kW, kH, a)};
    const std::vector<Rgba8> fromEight = eight.captureViewport(frameA);

    Renderer sixteen{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    sixteen.automaticInterpolation(false);
    const ImageId b = sixteen.uploadImage(std::span<const Rgba16>(widened), kW, kH);
    FrameDrawState frameB;
    frameB.layers = {imageLayer("image", 0, kW, kH, b)};
    const std::vector<Rgba8> fromSixteen = sixteen.captureViewport(frameB);

    ASSERT_EQ(fromEight.size(), static_cast<std::size_t>(kW) * kH);
    EXPECT_EQ(fromEight, fromSixteen);
}

// A layer that names an image shows the image, whatever pixels it also carries, and sends none of them.
TEST_F(ImageLayerTest, AnImageOutranksThePixelsBesideIt) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<Rgba8> blue  = solidPicture(kW, kH, Rgba8{.r = 0, .g = 0, .b = 255});
    const ImageId            image = r.uploadImage(std::span<const Rgba8>(blue), kW, kH);

    std::vector<std::uint8_t> red(static_cast<std::size_t>(kW) * kH * 4);
    for (std::size_t at = 0; at < red.size(); at += 4) {
        red[at + 0] = 0xFF;
        red[at + 1] = 0x00;
        red[at + 2] = 0x00;
        red[at + 3] = 0xFF;
    }
    DrawLayer layer{.key = "image"};
    layer.size    = PixelSize{kW, kH};
    layer.content = RasterContent{.pixels     = std::span<const std::uint8_t>(red),
                                  .width      = kW,
                                  .height     = kH,
                                  .format     = RasterPixelFormat::Rgba8888,
                                  .generation = 1,
                                  .image      = image};
    FrameDrawState frame;
    frame.layers = {layer};

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (std::size_t i = 0; i < px.size(); ++i) {
        EXPECT_EQ(px[i].r, 0) << "pixel " << i;
        EXPECT_EQ(px[i].b, 255) << "pixel " << i;
    }
    EXPECT_EQ(r.renderStats().rasterUploads, 0u);
}

// An image layer sends nothing on any frame: the image's texture is bound as it is, so neither raster
// counter moves.
TEST_F(ImageLayerTest, AnImageLayerUploadsNothingOnAnyFrame) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<Rgba8> picture = distinctPicture(kW, kH);
    const ImageId            image   = r.uploadImage(std::span<const Rgba8>(picture), kW, kH);

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, kW, kH, image)};
    for (int i = 0; i < 5; ++i) (void)r.captureViewport(frame);

    EXPECT_EQ(r.renderStats().rasterUploads, 0u);
    EXPECT_EQ(r.renderStats().rasterSkips, 0u);
}

// Naming a different image is a different picture on screen, so a frame identical in every other way
// recomposes; naming the same image again skips. renderFrame is the path that skips (captureViewport
// always composes), and with no window it composes offscreen and skips the blit.
TEST_F(ImageLayerTest, AChangedImageRecomposesAndTheSameImageSkips) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<Rgba8> pictureA = distinctPicture(kW, kH);
    const std::vector<Rgba8> pictureB = solidPicture(kW, kH, Rgba8{.r = 40, .g = 80, .b = 120});
    const ImageId            a = r.uploadImage(std::span<const Rgba8>(pictureA), kW, kH);
    const ImageId            b = r.uploadImage(std::span<const Rgba8>(pictureB), kW, kH);

    auto submit = [&](ImageId image) {
        FrameDrawState frame;
        frame.layers = {imageLayer("image", 0, kW, kH, image)};
        r.renderFrame(frame);
    };

    submit(a);   // composes (composePasses 1)
    submit(a);   // identical: skips (composeSkips 1)
    ASSERT_EQ(r.renderStats().composePasses, 1u);
    ASSERT_EQ(r.renderStats().composeSkips, 1u);

    submit(b);   // a different image: composed again
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 1u);

    submit(b);   // identical again: skips again
    EXPECT_EQ(r.renderStats().composePasses, 2u);
    EXPECT_EQ(r.renderStats().composeSkips, 2u);
}

// An id the renderer never minted draws nothing: what is beneath shows, whether the renderer holds no
// image at all or holds one that is not the one named.
TEST_F(ImageLayerTest, AnImageNobodyMintedDrawsNothing) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const Rgba8    green{.r = 0, .g = 255, .b = 0};
    const Backdrop below = backdropLayer(r, "below", 0, kW, kH, green);

    FrameDrawState frame;
    frame.layers = {below.layer, imageLayer("image", 10, kW, kH, ImageId{7})};

    const std::vector<Rgba8> none = r.captureViewport(frame);
    ASSERT_EQ(none.size(), static_cast<std::size_t>(kW) * kH);
    for (std::size_t i = 0; i < none.size(); ++i) {
        expectRgb(none[i], green, static_cast<int>(i % kW), static_cast<int>(i / kW));
    }

    const std::vector<Rgba8> red = solidPicture(kW, kH, Rgba8{.r = 255, .g = 0, .b = 0});
    ASSERT_EQ(r.uploadImage(std::span<const Rgba8>(red), kW, kH), ImageId{1});
    const std::vector<Rgba8> oneHeld = r.captureViewport(frame);
    ASSERT_EQ(oneHeld.size(), static_cast<std::size_t>(kW) * kH);
    for (std::size_t i = 0; i < oneHeld.size(); ++i) {
        expectRgb(oneHeld[i], green, static_cast<int>(i % kW), static_cast<int>(i / kW));
    }
}

// A fit applies to an image as to any raster: a 4×2 image fitted to the 16×8 viewport repeats each texel
// over a 4×4 block.
TEST_F(ImageLayerTest, AFitAppliesToAnImageAsToAnyRaster) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    constexpr int            kSmallW = kW / 4, kSmallH = kH / 4;
    const std::vector<Rgba8> picture = distinctPicture(kSmallW, kSmallH);
    const ImageId            image   = r.uploadImage(std::span<const Rgba8>(picture), kSmallW, kSmallH);

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, kW, kH, image, PixelSize{kW, kH})};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            expectRgb(px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)],
                      texelOf(picture, kSmallW, x / 4, y / 4), x, y);
        }
    }
}

// An indexed or grayscale PNG is atlas art, refused as an image, and the refusal mints nothing.
TEST_F(ImageLayerTest, AnIndexedPngIsRefusedAsAnImageAsset) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    EXPECT_THROW((void)r.loadImageFromMemory(readFile(fixture("indexed4.png"))), std::runtime_error);
    EXPECT_THROW((void)r.loadImageFromMemory(readFile(fixture("gray2.png"))), std::runtime_error);

    const std::vector<Rgba8> picture = distinctPicture(2, 2);
    EXPECT_EQ(r.uploadImage(std::span<const Rgba8>(picture), 2, 2), ImageId{1});
}

// A dimension that is not positive, or fewer pixels than the dimensions name, is refused at the call,
// and the refusal mints nothing.
TEST_F(ImageLayerTest, AnEmptyOrShortImageIsRefusedAtTheCall) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    const std::vector<Rgba8> three = distinctPicture(3, 1);
    EXPECT_THROW((void)r.uploadImage(std::span<const Rgba8>(three), 0, 0), std::invalid_argument);
    EXPECT_THROW((void)r.uploadImage(std::span<const Rgba8>(three), -1, 2), std::invalid_argument);
    EXPECT_THROW((void)r.uploadImage(std::span<const Rgba8>(three), 2, 2), std::invalid_argument);

    const std::vector<Rgba8> four = distinctPicture(2, 2);
    EXPECT_EQ(r.uploadImage(std::span<const Rgba8>(four), 2, 2), ImageId{1});
}

// The path form reads the PNG from the asset root: a literal logical path resolves against assetRoot().
// "rgb8.png" names no file at the project root, so the build scan leaves it alone and the read at run
// time resolves it against the fixtures directory set as the root here.
TEST_F(ImageLayerTest, TheLiteralPathFormResolvesAgainstTheAssetRoot) {
    const std::filesystem::path previousRoot = assetRoot();
    setAssetRoot(RETROPP_FIXTURES_DIR);

    Renderer r{device_, /*window=*/nullptr, ViewportResolution{2, 2}};
    r.automaticInterpolation(false);
    const ImageId image = r.loadImage("rgb8.png");
    setAssetRoot(previousRoot);

    FrameDrawState frame;
    frame.layers = {imageLayer("image", 0, 2, 2, image)};
    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        expectRgb(px[i], kRgb8[i], static_cast<int>(i % 2), static_cast<int>(i / 2));
    }
}

}  // namespace
