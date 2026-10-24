// Sprite-instancing regression test — proves N sprites render as N DISTINCT instances, not just
// instance 0. It is the guard for the D3D12 storage-buffer bug: SDL's D3D12 backend leaves a buffer's
// StructureByteStride at 0, and AMD uses that stride to index a StructuredBuffer — so a dynamically
// indexed StructuredBuffer collapses every index to element 0 on AMD (only the first sprite renders),
// while NVIDIA / WARP / Metal / Vulkan index correctly and hide the fault. The engine's fix reads the
// per-sprite record through a ByteAddressBuffer (byte offset, stride-independent), which this test
// locks in.
//
// Device-backed and reference-FREE: it composes four sprites that differ only by palette, each at its
// own position, and asserts each sprite's OWN color appears at its OWN center. On a backend exhibiting
// the bug every instance reads record 0, so only sprite 0 renders and sprites 1..3 read the background
// there — the assertion goes red. No committed golden is involved, so the test gives a clean red/green
// on ANY GPU (the per-backend GoldenReadback pins compare each backend to itself and cannot catch a
// cross-GPU divergence like this by design). Its diagnostic value is the AMD/D3D12 dispatch run.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <SDL3/SDL.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its real path
// (D3D12 + DXIL) is exercised by the Windows x64 and the AMD dispatch jobs. Mirrors the golden harness.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class SpriteInstancing : public ::testing::Test {
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
                                "backend; its D3D12 path is covered by the Windows x64 + AMD dispatch jobs. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". Install a GPU driver (a software rasterizer such as lavapipe / WARP suffices) and, "
                      "on a headless runner, set SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }
};

TEST_F(SpriteInstancing, EveryInstanceRendersItsOwnRecord) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    // One solid 8x8 tile of palette index 1 — the sprite art. Each sprite differs ONLY by palette, so the
    // draw exercises the per-instance record read (GpuSprite.atlasPalette) directly: a collapse to record 0
    // shows as sprites sharing sprite 0's color AND position.
    std::array<std::uint8_t, 8 * 8> solid{};
    solid.fill(1);
    const AtlasId atlas = r.uploadAtlas(solid.data(), 8, 8).atlasId;

    // Background fills the viewport in a color distinct from every sprite, so a non-rendered sprite's
    // cell reads the background — never a sprite color by coincidence.
    const std::array<Rgba8, 2> bgPal{{{0, 0, 0}, {25, 25, 35}}};
    const PaletteId bg = r.uploadPalette(std::span<const Rgba8>(bgPal));

    // Four distinct, opaque sprite colors (at palette index 1), one palette each.
    const std::array<Rgba8, 4> colors{{{220, 40, 40}, {40, 200, 80}, {50, 110, 230}, {240, 220, 60}}};
    std::array<PaletteId, 4> pals{};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const std::array<Rgba8, 2> pal{{{0, 0, 0}, colors[i]}};
        pals[i] = r.uploadPalette(std::span<const Rgba8>(pal));
    }

    struct Placed { int x, y; };
    const std::array<Placed, 4> at{{{8, 8}, {44, 8}, {8, 44}, {44, 44}}};

    // Background tile layer (8x8 grid of the solid tile in the bg palette).
    std::vector<TileCell> cells(8 * 8, TileCell{.atlas = atlas, .tile = 0, .palette = bg});
    DrawLayer bgLayer{.key = "bg"};
    bgLayer.z       = 0;
    bgLayer.size    = PixelSize{kW, kH};
    bgLayer.content = TileContent{.widthInTiles  = 8,
                                  .heightInTiles = 8,
                                  .cells         = std::span<const TileCell>(cells)};

    // Sprite layer — four instances, one record each.
    std::vector<Sprite> sprites;
    for (std::size_t i = 0; i < at.size(); ++i) {
        sprites.push_back(Sprite{.key     = "sp" + std::to_string(i),
                                 .x       = at[i].x,
                                 .y       = at[i].y,
                                 .size    = AssetDimensions::GameBoy8x8,
                                 .atlas   = atlas,
                                 .tile    = 0,
                                 .palette = pals[i]});
    }
    DrawLayer spLayer{.key = "sprites"};
    spLayer.z       = 10;
    spLayer.size    = PixelSize{kW, kH};
    spLayer.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};

    FrameDrawState frame;
    frame.layers.push_back(bgLayer);
    frame.layers.push_back(spLayer);

    const std::vector<Rgba8> px = r.captureViewport(frame);
    ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH));

    // Each sprite's own color must appear at its own center. If the D3D12 stride-0 collapse is present,
    // sprites 1..3 read record 0 and never render here — these expectations go red exactly on the buggy
    // backend, one line per instance so the failure names which instances vanished.
    for (std::size_t i = 0; i < at.size(); ++i) {
        const int    cx  = at[i].x + 3;
        const int    cy  = at[i].y + 3;
        const Rgba8  got = px[static_cast<std::size_t>(cy) * kW + static_cast<std::size_t>(cx)];
        const Rgba8  want = colors[i];
        EXPECT_EQ(got.r, want.r) << "sprite instance " << i << " did not render its own record at ("
                                 << cx << "," << cy << ") — every instance may be reading record 0 "
                                    "(the D3D12 StructuredBuffer stride-0 collapse)";
        EXPECT_EQ(got.g, want.g) << "sprite instance " << i << " green mismatch at (" << cx << "," << cy << ")";
        EXPECT_EQ(got.b, want.b) << "sprite instance " << i << " blue mismatch at (" << cx << "," << cy << ")";
    }
}

}  // namespace
