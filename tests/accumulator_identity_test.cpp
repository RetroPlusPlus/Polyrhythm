// The accumulator stays itself across a blended layer's composite.
//
// The renderer composites a frame into one accumulator texture, and a pass that cannot write the texture it
// reads writes a scratch texture instead, which then becomes the accumulator. The scratch textures and the
// accumulator are distinct textures the renderer owns, one each, for its whole life: a hand-off swaps which
// texture plays which role and never leaves two roles on one texture. Were two roles left on one texture, a
// later pass writing that scratch would overwrite the image being composited — in that frame and every frame
// the renderer draws after — and the renderer's teardown would release that texture twice.
//
// The hand-off these cases reach is a layer composited on its own under a non-Normal DrawLayer::blend whose
// own Layer-scope region runs first: the region writes a scratch, and the blended composite writes the
// accumulator's replacement. Each case is differential against a renderer that has drawn nothing else: what
// one renderer draws after that layer equals what a fresh renderer draws, exactly.
//
// Device-backed: a compose-only, windowless Renderer on a real GPU device (a software rasterizer in CI),
// bootstrapped per file as the other device-backed suites do.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/image.h"  // TransparentIndices
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class AccumulatorIdentity : public ::testing::Test {
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
                                "backend in CI, and the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This suite requires a GPU device on every production-representative platform "
                      "(macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a software rasterizer (lavapipe / WARP) "
                      "suffices; on a headless runner set SDL_VIDEODRIVER=offscreen.";
        }
    }
};

// ── The scene ───────────────────────────────────────────────────────────────────────────────────────

struct Art {
    AtlasId   background{};
    PaletteId backgroundPalette{};
    AtlasId   sprite{};
    PaletteId spritePalette{};
};

// The same art on every renderer, so captures from different renderers compare exactly.
Art uploadArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> bg{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            bg[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
    const std::array<Rgba8, 4> bgPal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};

    // An 8×8 sprite whose left four columns are see-through (index 0) and right four a warm color.
    std::array<std::uint8_t, 8 * 8> sp{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) sp[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] = x < 4 ? 0 : 1;
    const std::array<Rgba8, 2> spPal{{{0, 0, 0, 255}, {200, 120, 60, 255}}};

    return Art{.background        = r.uploadAtlas(bg.data(), 16, 16, TransparentIndices::None).atlasId,
               .backgroundPalette = r.uploadPalette(std::span<const Rgba8>(bgPal)),
               .sprite            = r.uploadAtlas(sp.data(), 8, 8, TransparentIndices::GameBoy).atlasId,
               .spritePalette     = r.uploadPalette(std::span<const Rgba8>(spPal))};
}

// What a frame's spans point at, kept alive for the capture.
struct Keep {
    std::vector<TileCell> cells;
    std::vector<Sprite>   sprites;
};

DrawLayer background(const Art& art, Keep& keep) {
    keep.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas   = art.background,
                         .tile    = static_cast<std::uint16_t>((tx + ty) % 4),
                         .palette = art.backgroundPalette};
    DrawLayer l{.key = "background"};
    l.z       = 0;
    l.size    = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep.cells)};
    return l;
}

// The background alone.
FrameDrawState backgroundFrame(const Art& art, Keep& keep) {
    FrameDrawState f;
    f.layers.push_back(background(art, keep));
    return f;
}

// The background, and over it a sprite layer under Add holding one sprite with see-through pixels. With
// `withRegion` the layer also carries a whole-layer ColorFill region under Subtract at fill 0 — a region
// that subtracts nothing, so the frame it draws is the frame without it.
FrameDrawState spriteFrame(const Art& art, Keep& keep, bool withRegion) {
    FrameDrawState f;
    f.layers.push_back(background(art, keep));
    keep.sprites = {Sprite{.key = "monster", .x = 24, .y = 20, .atlas = art.sprite, .tile = 0,
                           .palette = art.spritePalette}};
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.blend   = BlendMode::Add;
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep.sprites)};
    if (withRegion) {
        Region fade{.key = "fade"};
        fade.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{0, 0, 0, 255}}};
        fade.blend   = BlendMode::Subtract;
        sp.regions   = {fade};
    }
    f.layers.push_back(sp);
    return f;
}

// What a renderer that has drawn nothing else draws for one frame.
std::vector<Rgba8> freshCapture(SDL_GPUDevice* dev, bool spriteLayer) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r);
    Keep      keep;
    return r.captureViewport(spriteLayer ? spriteFrame(art, keep, /*withRegion=*/false) : backgroundFrame(art, keep));
}

bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
std::string px(Rgba8 c) {
    return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a);
}

::testing::AssertionResult sameImage(const std::vector<Rgba8>& got, const std::vector<Rgba8>& want) {
    if (got.size() != want.size()) return ::testing::AssertionFailure() << "size " << got.size() << " vs " << want.size();
    for (std::size_t i = 0; i < got.size(); ++i)
        if (!exactEq(got[i], want[i]))
            return ::testing::AssertionFailure() << "pixel (" << i % kW << "," << i / kW << ") is " << px(got[i])
                                                 << ", a fresh renderer draws " << px(want[i]);
    return ::testing::AssertionSuccess();
}

// ── Cases ───────────────────────────────────────────────────────────────────────────────────────────

// The blended layer with its region draws what the same layer draws without it, the frame after it — the
// background alone, on the same renderer — is what a fresh renderer draws, and so are the same two frames
// drawn again: a frame that hands the accumulator over leaves every later frame as it would be.
TEST_F(AccumulatorIdentity, ABlendedLayersRegionLeavesLaterFramesAsAFreshRendererDrawsThem) {
    const std::vector<Rgba8> wantSprite = freshCapture(device_, /*spriteLayer=*/true);
    const std::vector<Rgba8> wantBg     = freshCapture(device_, /*spriteLayer=*/false);

    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r);
    for (int round = 0; round < 2; ++round) {
        Keep k1, k2;
        EXPECT_TRUE(sameImage(r.captureViewport(spriteFrame(art, k1, /*withRegion=*/true)), wantSprite))
            << "round " << round << ": the frame carrying the region";
        EXPECT_TRUE(sameImage(r.captureViewport(backgroundFrame(art, k2)), wantBg))
            << "round " << round << ": the frame after it";
    }
}

// Ten frames on one renderer, alternating the blended layer with its region and the background alone: every
// one is what a fresh renderer draws for it, so nothing one frame does reaches the next.
TEST_F(AccumulatorIdentity, AlternatingFramesEachDrawAsAFreshRendererDrawsThem) {
    const std::vector<Rgba8> wantSprite = freshCapture(device_, /*spriteLayer=*/true);
    const std::vector<Rgba8> wantBg     = freshCapture(device_, /*spriteLayer=*/false);

    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art art = uploadArt(r);
    for (int i = 0; i < 10; ++i) {
        Keep       k;
        const bool sprite = (i % 2) == 0;
        const std::vector<Rgba8> got =
            r.captureViewport(sprite ? spriteFrame(art, k, /*withRegion=*/true) : backgroundFrame(art, k));
        ASSERT_TRUE(sameImage(got, sprite ? wantSprite : wantBg)) << "frame " << i;
    }
}

// A renderer that drew the blended layer with its region is torn down, and a new one on the same device draws
// the background as a fresh renderer does: the teardown released each of its textures once.
TEST_F(AccumulatorIdentity, ARendererTornDownAfterABlendedLayerLeavesTheDeviceSound) {
    const std::vector<Rgba8> wantBg = freshCapture(device_, /*spriteLayer=*/false);
    {
        Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
        const Art art = uploadArt(r);
        Keep      k;
        (void)r.captureViewport(spriteFrame(art, k, /*withRegion=*/true));
    }
    EXPECT_TRUE(sameImage(freshCapture(device_, /*spriteLayer=*/false), wantBg));
}

}  // namespace
