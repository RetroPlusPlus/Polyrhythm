// Sprite::blend — the container completion. A sprite carries a BlendMode beside its alpha (alpha = HOW
// MUCH it contributes, blend = HOW), so a non-Normal sprite grades against its compositing container's
// image at draw time exactly per retropp::applyBlendMode: a Multiply shadow decal darkens the scene
// beneath, an Add flare lifts it. Two halves:
//
//   1. The run partition (device-free). spriteBlendRuns() splits a sprite layer's DRAW ORDER into
//      contiguous same-blend runs — the mechanism the renderer draws each run from its own buffer with.
//      An all-Normal layer yields ONE Normal run (the byte-identical single-draw fast path); a mixed
//      layer splits at every blend change, in draw (z) order, so within-layer z survives the split.
//
//   2. The composite (device-backed, via captureViewport). A solid non-Normal sprite over an opaque
//      scene is compared, per covered pixel, against applyBlendMode(scene, {spriteColor, 1}, mode) — the
//      spriteColor read from a Normal capture of the same sprite (the mirror-from-capture method
//      color_parity_layer_test uses), so palette→float→8-bit rounding cancels. Two invariants ride
//      along: OFF the sprite the frame is byte-identical to the no-sprite scene (run-splitting never
//      leaks), and a Normal sprite is byte-identical to a sprite that sets no blend at all (the default
//      path is untouched — the byte-identity guarantee the goldens also carry). The exact-value cells use
//      OPAQUE sprites sampled on their interior: over opaque content the premultiplied isolated run image
//      and the straight applyBlendMode source coincide, so the comparison is exact to 2 8-bit steps.
//
// The container blend MATH itself (applyBlendMode / blendChannel / clamp placement) is the device-free
// authority tests/blend_mode_test.cpp; this file verifies it reaches the SPRITE surface.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/image.h"        // TransparentIndices
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// ── Part 1: the run partition (device-free) ──────────────────────────────────────────────────────

// Build a sprite with a key, z, and blend — the only fields the partition reads (z orders, blend groups).
Sprite blendSprite(const char* key, std::int32_t z, BlendMode blend) {
    return Sprite{.key = key, .z = z, .blend = blend};
}

std::vector<SpriteBlendRun> runsOf(const std::vector<Sprite>& sprites) {
    return spriteBlendRuns(sprites, spriteDrawOrder(sprites));
}

TEST(SpriteBlendRuns, DefaultBlendIsNormal) {
    EXPECT_EQ(Sprite{.key = "s"}.blend, BlendMode::Normal);
}

TEST(SpriteBlendRuns, EmptyLayerYieldsNoRuns) {
    EXPECT_TRUE(runsOf({}).empty());
}

TEST(SpriteBlendRuns, AllNormalIsOneSpanningRun) {
    // The fast-path signal: one Normal run over everything ⇒ the renderer keeps the single instanced draw.
    const std::vector<Sprite> sprites = {blendSprite("a", 0, BlendMode::Normal),
                                         blendSprite("b", 1, BlendMode::Normal),
                                         blendSprite("c", 2, BlendMode::Normal)};
    const std::vector<SpriteBlendRun> runs = runsOf(sprites);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].start, 0u);
    EXPECT_EQ(runs[0].count, 3u);
    EXPECT_EQ(runs[0].blend, BlendMode::Normal);
}

TEST(SpriteBlendRuns, SingleNonNormalIsOneRun) {
    // Even a lone non-Normal sprite leaves the fast path — one Multiply run (not a Normal run).
    const std::vector<SpriteBlendRun> runs = runsOf({blendSprite("a", 0, BlendMode::Multiply)});
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].count, 1u);
    EXPECT_EQ(runs[0].blend, BlendMode::Multiply);
}

TEST(SpriteBlendRuns, SplitsAtEveryBlendChange) {
    const std::vector<Sprite> sprites = {blendSprite("a", 0, BlendMode::Normal),
                                         blendSprite("b", 1, BlendMode::Multiply),
                                         blendSprite("c", 2, BlendMode::Normal)};
    const std::vector<SpriteBlendRun> runs = runsOf(sprites);
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].blend, BlendMode::Normal);   EXPECT_EQ(runs[0].start, 0u); EXPECT_EQ(runs[0].count, 1u);
    EXPECT_EQ(runs[1].blend, BlendMode::Multiply);  EXPECT_EQ(runs[1].start, 1u); EXPECT_EQ(runs[1].count, 1u);
    EXPECT_EQ(runs[2].blend, BlendMode::Normal);   EXPECT_EQ(runs[2].start, 2u); EXPECT_EQ(runs[2].count, 1u);
}

TEST(SpriteBlendRuns, CoalescesAdjacentSameBlend) {
    const std::vector<Sprite> sprites = {blendSprite("a", 0, BlendMode::Multiply),
                                         blendSprite("b", 1, BlendMode::Multiply),
                                         blendSprite("c", 2, BlendMode::Add)};
    const std::vector<SpriteBlendRun> runs = runsOf(sprites);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].blend, BlendMode::Multiply); EXPECT_EQ(runs[0].start, 0u); EXPECT_EQ(runs[0].count, 2u);
    EXPECT_EQ(runs[1].blend, BlendMode::Add);      EXPECT_EQ(runs[1].start, 2u); EXPECT_EQ(runs[1].count, 1u);
}

TEST(SpriteBlendRuns, PartitionFollowsDrawOrderNotSubmissionOrder) {
    // Submit out of z order; the partition rides spriteDrawOrder, so the runs come out z-sorted
    // (Normal z0 → Multiply z1 → Add z2), keeping within-layer z exact across the split.
    const std::vector<Sprite> sprites = {blendSprite("add", 2, BlendMode::Add),
                                         blendSprite("norm", 0, BlendMode::Normal),
                                         blendSprite("mul", 1, BlendMode::Multiply)};
    const std::vector<SpriteBlendRun> runs = runsOf(sprites);
    ASSERT_EQ(runs.size(), 3u);
    EXPECT_EQ(runs[0].blend, BlendMode::Normal);
    EXPECT_EQ(runs[1].blend, BlendMode::Multiply);
    EXPECT_EQ(runs[2].blend, BlendMode::Add);
    // start indices are positions in the ORDERED sequence, so they are still 0,1,2 ascending.
    EXPECT_EQ(runs[0].start, 0u);
    EXPECT_EQ(runs[1].start, 1u);
    EXPECT_EQ(runs[2].start, 2u);
}

// ── Part 2: the composite (device-backed) ─────────────────────────────────────────────────────────

constexpr int kW = 64;
constexpr int kH = 64;

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its production
// path (D3D12) is covered by the Windows x64 job, so a missing device there is an out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class SpriteBlend : public ::testing::Test {
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
                                "backend in CI; the container-blend math is the device-free applyBlendMode "
                                "mirror (blend_mode_test.cpp), and the D3D12 path is covered by Windows x64. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". This sprite-blend harness requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a software rasterizer "
                      "(lavapipe / WARP) suffices; on a headless runner set SDL_VIDEODRIVER=offscreen.";
        }
    }
};

Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}

// A 4-color opaque tile background filling the viewport (the golden harness's deterministic art) — the
// scene a sprite blends over. `keep` outlives the returned layer's span.
struct Art { AtlasId atlas{}; PaletteId palette{}; };
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
DrawLayer bgLayer(std::int32_t z, const Art& art, std::vector<TileCell>& keep) {
    keep.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
            keep[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas, .tile = static_cast<std::uint16_t>((tx + ty) % 4),
                         .palette = art.palette};
    DrawLayer l{.key = "bg"};
    l.z = z; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

// A solid opaque 8×8 sprite sheet (one index) + a palette carrying `color` at that index, so a
// GameBoy8x8 sprite (tile 0) renders a flat, known color.
Art uploadSolid(Renderer& r, Rgba8 color) {
    std::array<std::uint8_t, 8 * 8> idx{};  // all index 0
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;  // opaque
    const std::array<Rgba8, 4> pal{{color, color, color, color}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}

// The whole plain-layer matrix: a solid sprite of `blend` over the opaque background, checked per covered
// pixel against applyBlendMode(scene, {spriteColor, 1}, blend) with the sprite color read from a Normal
// capture. OFF the sprite the frame is byte-identical to the background alone (run-splitting never leaks).
void runPlainCell(SDL_GPUDevice* dev, const char* name, BlendMode mode) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art  bg    = uploadBgArt(r);
    const Art  solid = uploadSolid(r, Rgba8{110, 140, 190, 255});  // distinct from every bg color
    std::vector<TileCell> c0, c1, c2;

    // Baseline: the background alone.
    FrameDrawState base;
    base.layers.push_back(bgLayer(0, bg, c0));
    const std::vector<Rgba8> B = r.captureViewport(base);

    auto spriteScene = [&](BlendMode blend, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(0, bg, cells));
        keepS = {Sprite{.key = "fx", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0,
                        .palette = solid.palette, .blend = blend}};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };

    std::vector<Sprite> sN, sG;
    FrameDrawState fN = spriteScene(BlendMode::Normal, c1, sN);
    FrameDrawState fG = spriteScene(mode, c2, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);  // Normal ⇒ the sprite's flat color on coverage
    const std::vector<Rgba8> G = r.captureViewport(fG);  // the graded sprite
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    int covered = 0, leaked = 0, graded = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (!exactEq(N[i], B[i])) {  // the sprite covers here (Normal replaced the bg with its flat color)
            ++covered;
            const Vec4  want4 = applyBlendMode(norm(B[i]), Vec4{norm(N[i]).x, norm(N[i]).y, norm(N[i]).z, 1.0f}, mode);
            const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
            if (chDelta(G[i], want) > 2) {
                const int x = int(i) % kW, y = int(i) / kW;
                ADD_FAILURE() << name << " grade mismatch at (" << x << "," << y << "): got " << int(G[i].r)
                              << "," << int(G[i].g) << "," << int(G[i].b) << "  want " << int(want.r) << ","
                              << int(want.g) << "," << int(want.b);
            } else if (chDelta(G[i], B[i]) > 2) {
                ++graded;  // and it actually moved the pixel off the baseline
            }
        } else if (!exactEq(G[i], B[i])) {  // OFF the sprite the graded frame must equal the baseline exactly
            ++leaked;
        }
    }
    EXPECT_GT(covered, 0) << name << ": the sprite covered no pixel";
    EXPECT_EQ(leaked, 0) << name << ": " << leaked << " pixels changed OFF the sprite — run-splitting leaked";
    if (mode != BlendMode::Normal)
        EXPECT_GT(graded, 0) << name << ": the blend moved no covered pixel off the baseline";
}

TEST_F(SpriteBlend, PlainNormalReplaces)   { runPlainCell(device_, "plain_normal",   BlendMode::Normal); }
TEST_F(SpriteBlend, PlainMultiplyShadow)   { runPlainCell(device_, "plain_multiply",  BlendMode::Multiply); }
TEST_F(SpriteBlend, PlainAddFlare)         { runPlainCell(device_, "plain_add",       BlendMode::Add); }
TEST_F(SpriteBlend, PlainScreenBloom)      { runPlainCell(device_, "plain_screen",    BlendMode::Screen); }
TEST_F(SpriteBlend, PlainSubtract)         { runPlainCell(device_, "plain_subtract",  BlendMode::Subtract); }
TEST_F(SpriteBlend, PlainHalf)             { runPlainCell(device_, "plain_half",      BlendMode::Half); }

// A Normal sprite is byte-identical to a sprite that sets no blend at all — the default path (single
// buffer, single instanced draw) is untouched, the guarantee the committed goldens also carry.
TEST_F(SpriteBlend, NormalIsByteIdenticalToDefault) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadBgArt(r);
    const Art solid = uploadSolid(r, Rgba8{110, 140, 190, 255});
    std::vector<TileCell> c0, c1;

    auto scene = [&](bool setNormalExplicitly, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(bgLayer(0, bg, cells));
        Sprite s{.key = "fx", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        if (setNormalExplicitly) s.blend = BlendMode::Normal;  // the field's default is already Normal
        keepS = {s};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> sD, sN;
    FrameDrawState fD = scene(false, c0, sD);
    FrameDrawState fN = scene(true,  c1, sN);
    const std::vector<Rgba8> D = r.captureViewport(fD);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    ASSERT_EQ(D.size(), N.size());
    for (std::size_t i = 0; i < D.size(); ++i)
        ASSERT_TRUE(exactEq(D[i], N[i])) << "default vs explicit-Normal differ at pixel " << i;
}

// Isolated layer (container rule §1): a non-Normal sprite in a layer that is itself scratch-rendered
// (here forced isolated by a layer-level blend) grades against the layer's OWN scratch — within-layer
// content, not the accumulator. A lower Normal sprite A and an upper Multiply sprite B overlap; in the
// overlap B multiplies A (within the layer's scratch). The layer's own blend is Add over a BLACK
// background, which is identity for the opaque overlap, so the final overlap pixel = A·B — checked against
// applyBlendMode with A and B read from a within-layer Normal capture.
TEST_F(SpriteBlend, IsolatedLayerGradesWithinLayerContent) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const Art blackBg = uploadSolid(r, Rgba8{0, 0, 0, 255});      // a black, opaque backdrop tile
    const Art artA    = uploadSolid(r, Rgba8{210, 180, 150, 255});
    const Art artB    = uploadSolid(r, Rgba8{120, 120, 120, 255});

    // A at (16,16), B at (20,20): overlap is [20,24) × [20,24). B is drawn last (higher z).
    auto scene = [&](BlendMode bBlend, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        DrawLayer bg{.key = "bg"};
        cells.resize(8 * 8);
        for (auto& c : cells) c = TileCell{.atlas = blackBg.atlas, .tile = 0, .palette = blackBg.palette};
        bg.z = 0; bg.size = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(cells)};
        f.layers.push_back(bg);
        keepS = {Sprite{.key = "A", .x = 16, .y = 16, .atlas = artA.atlas, .tile = 0, .palette = artA.palette},
                 Sprite{.key = "B", .x = 20, .y = 20, .z = 1, .atlas = artB.atlas, .tile = 0,
                        .palette = artB.palette, .blend = bBlend}};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        sp.blend = BlendMode::Add;  // forces the layer through the isolated path; Add-over-black = identity
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<TileCell> cN, cG;
    std::vector<Sprite>   sN, sG;
    FrameDrawState fN = scene(BlendMode::Normal,   cN, sN);  // B Normal ⇒ overlap shows B's flat color
    FrameDrawState fG = scene(BlendMode::Multiply,  cG, sG);
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);

    auto at = [&](int x, int y) -> std::size_t { return static_cast<std::size_t>(y) * kW + x; };
    const Rgba8 aColor = N[at(17, 17)];  // an A-only pixel (inside A, outside B)
    const Rgba8 bColor = N[at(22, 22)];  // an overlap pixel with B Normal ⇒ B's flat color
    ASSERT_GT(aColor.r + aColor.g + aColor.b, 0) << "A rendered black — scene setup wrong";
    ASSERT_GT(bColor.r + bColor.g + bColor.b, 0) << "B rendered black — scene setup wrong";

    // Every overlap pixel, B multiplied over A within the layer's scratch.
    for (int y = 21; y < 23; ++y) {
        for (int x = 21; x < 23; ++x) {
            const Vec4  want4 = applyBlendMode(norm(aColor), Vec4{norm(bColor).x, norm(bColor).y,
                                                                   norm(bColor).z, 1.0f}, BlendMode::Multiply);
            const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
            const Rgba8 got = G[at(x, y)];
            EXPECT_LE(chDelta(got, want), 2)
                << "isolated within-layer Multiply at (" << x << "," << y << "): got " << int(got.r) << ","
                << int(got.g) << "," << int(got.b) << "  want " << int(want.r) << "," << int(want.g) << ","
                << int(want.b);
            // And it genuinely graded — the overlap is not just B (which Normal would give).
            EXPECT_GT(chDelta(got, bColor), 2) << "overlap equals B — the within-layer grade did nothing";
        }
    }
}

}  // namespace
