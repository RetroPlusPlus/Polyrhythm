// A hosted machine's picture as a layer's content — the submission surface.
//
// The first cases run with no machine and no device: what they pin is that a picture is a content
// alternative like any other, that dispatch over the content variant names it correctly, and that the
// type carries plain values and nothing a console brought with it.
//
// The rest are device-backed (a GPU device, no display — the harness the golden-readback and compose-skip
// tests use, so they run on a software rasterizer in CI): a picture reaches the screen at the pixels the
// machine drew, composites at its own z among native layers, and re-uploads only when the machine
// finished a new frame.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/guest_frame.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {

using namespace retropp;

// ── Dispatch over the content variant ───────────────────────────────────────────────────────────

// A picture reports itself as one, and specifically NOT as the alternative that sat last in the
// variant before it did. Dispatch over the content variant runs on this answer everywhere in the
// platform, so an alternative that reports the wrong kind is drawn as the wrong kind.
TEST(GuestFrameContentType, ContentKindReportsGuestFrameNotSprites) {
    const LayerContent content{GuestFrameContent{}};
    EXPECT_EQ(contentKind(content), LayerContentKind::GuestFrame);
    EXPECT_NE(contentKind(content), LayerContentKind::Sprites);
}

// The other two alternatives keep the kind they already reported.
TEST(GuestFrameContentType, ContentKindStillReportsTilesAndSprites) {
    EXPECT_EQ(contentKind(LayerContent{TileContent{}}), LayerContentKind::Tiles);
    EXPECT_EQ(contentKind(LayerContent{SpriteContent{}}), LayerContentKind::Sprites);
}

// ── What the type carries ───────────────────────────────────────────────────────────────────────

// Every member is a plain value: a byte span, dimensions, a declared layout, and the platform's answer
// to whether the machine drew something new. Nothing here is a handle to a core, a machine, or a
// console's own vocabulary — which is what keeps a second core implementing this surface rather than
// inheriting the first one's.
TEST(GuestFrameContentType, TheContentTypeNamesNoConsole) {
    static_assert(std::is_same_v<decltype(GuestFrameContent::pixels), std::span<const std::uint8_t>>);
    static_assert(std::is_same_v<decltype(GuestFrameContent::width), int>);
    static_assert(std::is_same_v<decltype(GuestFrameContent::height), int>);
    static_assert(std::is_same_v<decltype(GuestFrameContent::format), GuestPixelFormat>);
    static_assert(std::is_same_v<decltype(GuestFrameContent::generation), std::uint64_t>);
    static_assert(std::is_same_v<std::underlying_type_t<GuestPixelFormat>, std::uint8_t>);
    static_assert(std::is_standard_layout_v<GuestFrameContent>);
    SUCCEED();
}

TEST(GuestFrameContentType, AGuestFrameLayerCarriesItsDimensionsAndFormat) {
    const std::vector<std::uint8_t> pixels(4 * 2 * 4, 0x40);
    DrawLayer layer{.key = "screen"};
    layer.content = GuestFrameContent{.pixels = std::span<const std::uint8_t>(pixels),
                                      .width  = 4,
                                      .height = 2,
                                      .format = GuestPixelFormat::Rgba8888,
                                      .generation = 1};

    const GuestFrameContent& gc = std::get<GuestFrameContent>(layer.content);
    EXPECT_EQ(gc.width, 4);
    EXPECT_EQ(gc.height, 2);
    EXPECT_EQ(gc.format, GuestPixelFormat::Rgba8888);
    EXPECT_EQ(gc.pixels.size(), static_cast<std::size_t>(gc.width) *
                                    static_cast<std::size_t>(gc.height) * bytesPerPixel(gc.format));
}

// A default-constructed picture is a valid, degenerate submission — nothing drawn, the same way an
// empty sprite span is.
TEST(GuestFrameContentType, AnEmptyPictureIsAValidSubmission) {
    const GuestFrameContent empty{};
    EXPECT_EQ(empty.width, 0);
    EXPECT_EQ(empty.height, 0);
    EXPECT_TRUE(empty.pixels.empty());
    EXPECT_EQ(empty.generation, 0u);
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

class GuestFrameRenderTest : public ::testing::Test {
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

// The picture reaches the screen as the machine drew it: the same colours, at the same places, with no
// filtering between the machine's texels and the viewport's pixels.
TEST_F(GuestFrameRenderTest, AGuestFrameLayerRendersThePixelsTheMachineDrew) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();

    DrawLayer screen{.key = "screen"};
    screen.z    = 0;
    screen.size = PixelSize{kW, kH};
    screen.content = GuestFrameContent{.pixels = std::span<const std::uint8_t>(picture),
                                       .width  = kW,
                                       .height = kH,
                                       .format = GuestPixelFormat::Rgba8888,
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
TEST_F(GuestFrameRenderTest, AGuestFrameLayerComposesAtItsOwnZ) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);

    // A solid one-colour 8×8 sheet and its palette — the native layer that argues over the picture.
    std::array<std::uint8_t, 8 * 8> idx{};
    idx.fill(1);
    const AtlasId               atlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2>  pal{{{0, 0, 0}, {0, 255, 0}}};
    const PaletteId             palette = r.uploadPalette(std::span<const Rgba8>(pal));
    std::vector<TileCell>       cells(4 * 2, TileCell{.atlas = atlas, .tile = 0, .palette = palette});

    const std::vector<std::uint8_t> picture = splitPicture();
    DrawLayer screen{.key = "screen"};
    screen.size    = PixelSize{kW, kH};
    screen.content = GuestFrameContent{.pixels = std::span<const std::uint8_t>(picture),
                                       .width  = kW,
                                       .height = kH,
                                       .format = GuestPixelFormat::Rgba8888,
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
TEST_F(GuestFrameRenderTest, TwoPicturesSitSideBySideWhenOneIsScrolled) {
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
        layer.content = GuestFrameContent{.pixels = std::span<const std::uint8_t>(pixels),
                                          .width  = kW,
                                          .height = kH,
                                          .format = GuestPixelFormat::Rgba8888,
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

// The generation is the whole upload decision: a submission carrying one the resident texture does not
// hold sends the raster, and one carrying the generation already up there sends nothing. The pixels are
// never hashed to decide — a full-colour raster differs every time the machine draws.
TEST_F(GuestFrameRenderTest, TheRendererUploadsWhenTheGenerationMovesAndNotOtherwise) {
    Renderer r{device_, /*window=*/nullptr, ViewportResolution{kW, kH}};
    r.automaticInterpolation(false);
    const std::vector<std::uint8_t> picture = splitPicture();

    DrawLayer screen{.key = "screen"};
    screen.z    = 0;
    screen.size = PixelSize{kW, kH};
    auto submit = [&](std::uint64_t generation) {
        screen.content = GuestFrameContent{.pixels = std::span<const std::uint8_t>(picture),
                                           .width  = kW,
                                           .height = kH,
                                           .format = GuestPixelFormat::Rgba8888,
                                           .generation = generation};
        FrameDrawState frame;
        frame.layers = {screen};
        (void)r.captureViewport(frame);
    };

    submit(1);
    const Renderer::RenderStats first = r.renderStats();
    EXPECT_EQ(first.guestFrameUploads, 1u);
    EXPECT_EQ(first.guestFrameSkips, 0u);

    submit(1);
    const Renderer::RenderStats held = r.renderStats();
    EXPECT_EQ(held.guestFrameUploads, 1u);  // the same frame — nothing sent
    EXPECT_EQ(held.guestFrameSkips, 1u);

    submit(2);
    const Renderer::RenderStats again = r.renderStats();
    EXPECT_EQ(again.guestFrameUploads, 2u);  // the machine drew — it is sent
    EXPECT_EQ(again.guestFrameSkips, 1u);

    // A generation that jumps — several frames finished between two submissions — is one upload, not
    // one per frame that went by.
    submit(9);
    const Renderer::RenderStats jumped = r.renderStats();
    EXPECT_EQ(jumped.guestFrameUploads, 3u);
    EXPECT_EQ(jumped.guestFrameSkips, 1u);
}

}  // namespace
