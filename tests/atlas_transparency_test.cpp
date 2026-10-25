// Atlas structural + material transparency — device-side behavior. Composes tiny frames offscreen on a
// real GPU device and reads one pixel back, asserting what the two shaders discard:
//
//   - a sprite from a None ({}) atlas DRAWS palette index 0 (no structural hole),
//   - a sprite from a GameBoy ({0}) atlas DISCARDS index 0 (the OBJ hole, opted in by name),
//   - a fully-transparent palette entry (alpha 0) DISCARDS regardless of the index set (material hole),
//   - the tile path reads the same per-sheet set (a {0} tile sheet holes index 0).
//
// The value type itself is covered purely in transparent_indices_test; this file proves the mask reaches
// the GPU and drives the discard. It mirrors the golden harness's windowless-device setup.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

// A windowless GPU device is REQUIRED on every production-representative platform (macOS/Metal,
// Windows-x64/D3D12, Linux/Vulkan): a missing device FAILS rather than skipping. Windows on ARM is a
// courtesy runner with no production-representative backend in CI (its D3D12 path is covered by the x64
// job), so a missing device there is a documented out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class AtlasTransparency : public ::testing::Test {
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
        if (!device_) {
            initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
        }
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
                                "backend in CI; the production path is covered by the x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This test requires a GPU device on every production-representative platform; "
                      "install a GPU driver (a software rasterizer such as lavapipe suffices) and set "
                      "SDL_VIDEODRIVER=offscreen on a headless runner.";
        }
    }

    // An 8x8 atlas cell filled with a single index — one sprite tile / one background tile of one color.
    static AtlasId solidCell(Renderer& r, std::uint8_t index, TransparentIndices transparent) {
        std::array<std::uint8_t, 8 * 8> px{};
        px.fill(index);
        return r.uploadAtlas(px.data(), 8, 8, transparent).atlasId;
    }

    // Compose: a full-viewport background tile layer (z 0) of `bgIndex` through `pal`, and a sprite layer
    // (z 10) with one 8x8 sprite at (0,0) of `spriteIndex` through `pal` from `spriteAtlas`. Returns the
    // composited pixel at (0,0) — sprite territory.
    static Rgba8 topLeftPixel(Renderer& r, PaletteId pal, AtlasId bgAtlas, std::uint8_t bgIndex,
                              AtlasId spriteAtlas) {
        std::vector<TileCell> cells(static_cast<std::size_t>(kW / 8) * (kH / 8));
        for (auto& c : cells) c = TileCell{.atlas = bgAtlas, .tile = 0, .palette = pal};
        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = kW / 8, .heightInTiles = kH / 8,
                                 .cells = std::span<const TileCell>(cells)};

        std::array<Sprite, 1> sprites{
            Sprite{.key = "sprite", .x = 0, .y = 0, .size = AssetDimensions{8, 8}, .atlas = spriteAtlas, .tile = 0, .palette = pal}};
        DrawLayer sp{.key = "sprite"};
        sp.z       = 10;
        sp.size    = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};

        FrameDrawState frame;
        frame.layers.push_back(bg);
        frame.layers.push_back(sp);
        const std::vector<Rgba8> px = r.captureViewport(frame);
        return px.at(0);
    }
};

// red at index 0, blue at index 1 (the background color) — both opaque.
constexpr std::array<Rgba8, 2> kOpaquePal{{{220, 30, 30, 255}, {30, 30, 220, 255}}};
// same, but index 0 is fully transparent (alpha 0) — the material hole.
constexpr std::array<Rgba8, 2> kAlpha0AtZero{{{220, 30, 30, 0}, {30, 30, 220, 255}}};

bool isRed(const Rgba8& c)  { return c.r > 180 && c.b < 80; }
bool isBlue(const Rgba8& c) { return c.b > 180 && c.r < 80; }

// A None ({}) sprite atlas draws index 0 — no structural hole, so palette[0] composites over the bg.
TEST_F(AtlasTransparency, NoneAtlasDrawsSpriteIndexZero) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PaletteId pal     = r.uploadPalette(std::span<const Rgba8>(kOpaquePal));
    const AtlasId   bg      = solidCell(r, /*index=*/1, TransparentIndices::None);
    const AtlasId   sprite  = solidCell(r, /*index=*/0, TransparentIndices::None);
    const Rgba8     pixel   = topLeftPixel(r, pal, bg, /*bgIndex=*/1, sprite);
    EXPECT_TRUE(isRed(pixel)) << "sprite index 0 from a None atlas should DRAW palette[0] (red); got "
                              << +pixel.r << "," << +pixel.g << "," << +pixel.b;
}

// A GameBoy ({0}) sprite atlas discards index 0 — the background shows through.
TEST_F(AtlasTransparency, GameBoyAtlasDiscardsSpriteIndexZero) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PaletteId pal    = r.uploadPalette(std::span<const Rgba8>(kOpaquePal));
    const AtlasId   bg     = solidCell(r, /*index=*/1, TransparentIndices::None);
    const AtlasId   sprite = solidCell(r, /*index=*/0, TransparentIndices::GameBoy);
    const Rgba8     pixel  = topLeftPixel(r, pal, bg, /*bgIndex=*/1, sprite);
    EXPECT_TRUE(isBlue(pixel)) << "sprite index 0 from a GameBoy ({0}) atlas should be a HOLE (blue bg); got "
                               << +pixel.r << "," << +pixel.g << "," << +pixel.b;
}

// A fully-transparent palette entry (alpha 0) is a hole regardless of the index set (material transparency).
TEST_F(AtlasTransparency, PaletteAlphaZeroDiscards) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PaletteId pal    = r.uploadPalette(std::span<const Rgba8>(kAlpha0AtZero));
    const AtlasId   bg     = solidCell(r, /*index=*/1, TransparentIndices::None);
    const AtlasId   sprite = solidCell(r, /*index=*/0, TransparentIndices::None);  // no structural hole
    const Rgba8     pixel  = topLeftPixel(r, pal, bg, /*bgIndex=*/1, sprite);
    EXPECT_TRUE(isBlue(pixel)) << "an alpha-0 palette entry should DISCARD (blue bg shows); got "
                               << +pixel.r << "," << +pixel.g << "," << +pixel.b;
}

// The tile path reads the SAME per-sheet set: a {0} tile sheet holes index 0, revealing the layer below.
TEST_F(AtlasTransparency, TilePathHonoursTheSet) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const PaletteId pal = r.uploadPalette(std::span<const Rgba8>(kOpaquePal));

    // Bottom layer: solid blue (index 1, None). Top layer: index 0 from a {0} sheet → holed → blue shows.
    const AtlasId bottom = solidCell(r, /*index=*/1, TransparentIndices::None);
    const AtlasId topGB  = solidCell(r, /*index=*/0, TransparentIndices::GameBoy);

    std::vector<TileCell> bottomCells(static_cast<std::size_t>(kW / 8) * (kH / 8));
    for (auto& c : bottomCells) c = TileCell{.atlas = bottom, .tile = 0, .palette = pal};
    std::vector<TileCell> topCells(static_cast<std::size_t>(kW / 8) * (kH / 8));
    for (auto& c : topCells) c = TileCell{.atlas = topGB, .tile = 0, .palette = pal};

    DrawLayer lower{.key = "lower"};
    lower.z       = 0;
    lower.size    = PixelSize{kW, kH};
    lower.content = TileContent{.widthInTiles = kW / 8, .heightInTiles = kH / 8,
                                .cells = std::span<const TileCell>(bottomCells)};
    DrawLayer upper{.key = "upper"};
    upper.z       = 10;
    upper.size    = PixelSize{kW, kH};
    upper.content = TileContent{.widthInTiles = kW / 8, .heightInTiles = kH / 8,
                                .cells = std::span<const TileCell>(topCells)};

    FrameDrawState frame;
    frame.layers.push_back(lower);
    frame.layers.push_back(upper);
    const std::vector<Rgba8> px = r.captureViewport(frame);
    EXPECT_TRUE(isBlue(px.at(0))) << "a {0} tile sheet should hole index 0 so the lower layer shows; got "
                                  << +px.at(0).r << "," << +px.at(0).g << "," << +px.at(0).b;
}

}  // namespace
