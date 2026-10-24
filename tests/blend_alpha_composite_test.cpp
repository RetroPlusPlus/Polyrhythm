// Partial-alpha container composite (device-backed). A non-Normal container (a Sprite or a DrawLayer)
// contributes at STRAIGHT-alpha weight over the scene it sits on:
//
//     out.rgb = (1 - a)·dst + a·B(dst, color)
//
// where `color` is the container's straight source color, `a` its source alpha at the pixel, `dst` the
// accumulator beneath, and B the container's separable BlendMode operator. This is exactly
// retropp::applyBlendMode(dst, {color, a}, mode) — the CPU authority the composite must reproduce on the
// GPU. The container's isolated render arrives at the composite PREMULTIPLIED (color·a in rgb, a in
// alpha), so the composite un-premultiplies before evaluating the operator; a is applied once, not twice.
//
// Two composite paths reach the same blend shader and are both covered here:
//   • sprite-run  — a non-Normal Sprite (its own blend), composited by compositeSpriteRuns.
//   • layer       — a partial-alpha Sprite inside a non-Normal DrawLayer, composited by
//                   renderLayerIsolated over the accumulator.
//
// The container-blend MATH itself lives device-free in blend_mode_test.cpp (applyBlendMode); this file
// verifies it reaches the GPU composite at PARTIAL alpha — the regime where premultiplied and straight
// source colors diverge (at a = 1 they coincide, which is what the opaque-only cells of
// sprite_blend_test.cpp already anchor). The sprite `color` and the scene `dst` are read back from Normal
// captures so palette→float→8-bit rounding cancels; the composite is compared within 2 8-bit steps.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/draw_state.h"
#include "retropp/postprocess.h"  // applyBlendMode, Vec4
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

constexpr int kW = 64;
constexpr int kH = 64;

// Windows on ARM is a courtesy runner with no production-representative GPU backend in CI; its production
// path (D3D12) is covered by the Windows x64 job, so a missing device there is an out-of-scope skip.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

class BlendAlpha : public ::testing::Test {
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
                   << ". This partial-alpha composite harness requires a GPU device on every "
                      "production-representative platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); a "
                      "software rasterizer (lavapipe / WARP) suffices; on a headless runner set "
                      "SDL_VIDEODRIVER=offscreen.";
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

// A solid opaque 8×8 sheet (one index) + a palette carrying `color` — a GameBoy8x8 tile/sprite renders a
// flat, known color.
struct Art { AtlasId atlas{}; PaletteId palette{}; };
Art uploadSolid(Renderer& r, Rgba8 colour) {
    std::array<std::uint8_t, 8 * 8> idx{};  // all index 0
    const AtlasId atlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;  // opaque (default None)
    const std::array<Rgba8, 4> pal{{colour, colour, colour, colour}};
    return {atlas, r.uploadPalette(std::span<const Rgba8>(pal))};
}
DrawLayer solidBgLayer(std::int32_t z, const Art& art, std::vector<TileCell>& keep) {
    keep.assign(8 * 8, TileCell{.atlas = art.atlas, .tile = 0, .palette = art.palette});
    DrawLayer l{.key = "bg"};
    l.z = z; l.size = PixelSize{kW, kH};
    l.content = TileContent{.widthInTiles = 8, .heightInTiles = 8, .cells = std::span<const TileCell>(keep)};
    return l;
}

enum class Path { SpriteRun, Layer };

// Render `spriteColour` over `bgColour` at source alpha `a` under `mode`, on the chosen composite path, and
// check every covered pixel against the straight-alpha oracle applyBlendMode(dst, {color, a}, mode). The
// sprite color is read from a Normal, fully-opaque capture (mirror-from-capture); the scene from a
// no-sprite baseline. Also reports the measured value at the sprite center so a failure shows the composited
// color directly.
void runPartialAlphaCell(SDL_GPUDevice* dev, const char* name, Path path, BlendMode mode, float a,
                         Rgba8 bgColour, Rgba8 spriteColour) {
    Renderer r{dev, nullptr, ViewportResolution{kW, kH}};
    const Art bg    = uploadSolid(r, bgColour);
    const Art solid = uploadSolid(r, spriteColour);
    std::vector<TileCell> cB, cN, cG;
    std::vector<Sprite>   sN, sG;

    // Baseline: the background alone.
    FrameDrawState base;
    base.layers.push_back(solidBgLayer(0, bg, cB));
    const std::vector<Rgba8> B = r.captureViewport(base);

    // A scene: bg + one sprite. `graded` picks the partial-alpha non-Normal configuration on the chosen
    // path; otherwise a Normal, fully-opaque sprite (the color/coverage reference). Layer path carries the
    // BlendMode on the layer with a Normal sprite; sprite-run path carries it on the sprite.
    auto scene = [&](bool graded, std::vector<TileCell>& cells, std::vector<Sprite>& keepS) {
        FrameDrawState f;
        f.layers.push_back(solidBgLayer(0, bg, cells));
        Sprite s{.key = "fx", .x = 24, .y = 20, .atlas = solid.atlas, .tile = 0, .palette = solid.palette};
        DrawLayer sp{.key = "sprites"};
        sp.z = 10; sp.size = PixelSize{kW, kH};
        if (graded) {
            s.alpha = a;
            if (path == Path::SpriteRun) s.blend = mode;
            else                         sp.blend = mode;
        }
        keepS = {s};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keepS)};
        f.layers.push_back(sp);
        return f;
    };

    FrameDrawState fN = scene(false, cN, sN);  // Normal, opaque ⇒ the sprite's flat color on coverage
    FrameDrawState fG = scene(true,  cG, sG);  // the partial-alpha graded sprite
    const std::vector<Rgba8> N = r.captureViewport(fN);
    const std::vector<Rgba8> G = r.captureViewport(fG);
    ASSERT_EQ(N.size(), B.size());
    ASSERT_EQ(G.size(), B.size());

    // The sprite sits at (24,20) size 8×8 — sample its center for the diagnostic.
    const std::size_t centre = static_cast<std::size_t>(24) * kW + 28;
    const Vec4  wantC = applyBlendMode(norm(B[centre]), Vec4{norm(N[centre]).x, norm(N[centre]).y,
                                                             norm(N[centre]).z, a}, mode);
    const Rgba8 wantCentre{quant(wantC.x), quant(wantC.y), quant(wantC.z), quant(wantC.w)};

    int covered = 0, mismatched = 0;
    for (std::size_t i = 0; i < B.size(); ++i) {
        if (exactEq(N[i], B[i])) continue;  // sprite does not cover here
        ++covered;
        const Vec4  want4 = applyBlendMode(norm(B[i]), Vec4{norm(N[i]).x, norm(N[i]).y, norm(N[i]).z, a}, mode);
        const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
        if (chDelta(G[i], want) > 2) ++mismatched;
    }
    EXPECT_GT(covered, 0) << name << ": the sprite covered no pixel";
    EXPECT_EQ(mismatched, 0)
        << name << " (a=" << a << "): " << mismatched << " covered pixels off the straight-alpha oracle. "
        << "centre got " << int(G[centre].r) << "," << int(G[centre].g) << "," << int(G[centre].b)
        << " want " << int(wantCentre.r) << "," << int(wantCentre.g) << "," << int(wantCentre.b);
}

// ── Sprite-run path (compositeSpriteRuns) — partial alpha × non-Normal ──────────────────────────────

// Named oracle (audit decision 4): Add, a = 0.5, red {255,0,0} over black → (128,0,0). A composite that
// treats the premultiplied source as straight color weights alpha twice and yields (64,0,0).
TEST_F(BlendAlpha, SpriteRunAddHalfRedOverBlack) {
    runPartialAlphaCell(device_, "run_add_red_black", Path::SpriteRun, BlendMode::Add, 0.5f,
                        Rgba8{0, 0, 0, 255}, Rgba8{255, 0, 0, 255});
}
// Named oracle (audit decision 4): Add, a = 0.5, {240,40,40} over {10,10,40} → (130,30,60); the
// double-count yields (70,20,50).
TEST_F(BlendAlpha, SpriteRunAddHalfOverDarkBlue) {
    runPartialAlphaCell(device_, "run_add_probe", Path::SpriteRun, BlendMode::Add, 0.5f,
                        Rgba8{10, 10, 40, 255}, Rgba8{240, 40, 40, 255});
}
TEST_F(BlendAlpha, SpriteRunMultiplyHalf) {
    runPartialAlphaCell(device_, "run_multiply", Path::SpriteRun, BlendMode::Multiply, 0.5f,
                        Rgba8{200, 200, 200, 255}, Rgba8{120, 60, 60, 255});
}
TEST_F(BlendAlpha, SpriteRunAddQuarter) {
    runPartialAlphaCell(device_, "run_add_quarter", Path::SpriteRun, BlendMode::Add, 0.25f,
                        Rgba8{20, 20, 30, 255}, Rgba8{200, 180, 60, 255});
}

// ── Layer path (renderLayerIsolated) — partial-alpha sprite inside a non-Normal layer ────────────────

TEST_F(BlendAlpha, LayerAddHalfRedOverBlack) {
    runPartialAlphaCell(device_, "layer_add_red_black", Path::Layer, BlendMode::Add, 0.5f,
                        Rgba8{0, 0, 0, 255}, Rgba8{255, 0, 0, 255});
}
TEST_F(BlendAlpha, LayerMultiplyHalf) {
    runPartialAlphaCell(device_, "layer_multiply", Path::Layer, BlendMode::Multiply, 0.5f,
                        Rgba8{200, 200, 200, 255}, Rgba8{120, 60, 60, 255});
}

// ── Invariants that must hold across the fix ─────────────────────────────────────────────────────────

// At full source alpha (a = 1) premultiplied ≡ straight — an opaque non-Normal sprite is unchanged. This is
// the byte-identity anchor: the fix must not move any full-alpha composite (every committed golden).
TEST_F(BlendAlpha, OpaqueAddUnchangedByUnpremultiply) {
    runPartialAlphaCell(device_, "opaque_add", Path::SpriteRun, BlendMode::Add, 1.0f,
                        Rgba8{30, 40, 60, 255}, Rgba8{180, 120, 90, 255});
}

// Normal at partial alpha is plain alpha-over on both paths — the composite reduces to (1-a)·dst + color
// (color already premultiplied), independent of the un-premultiply. A guard that Normal partial alpha is
// correct too, not only the operator modes.
TEST_F(BlendAlpha, NormalHalfIsAlphaOver) {
    runPartialAlphaCell(device_, "run_normal_half", Path::SpriteRun, BlendMode::Normal, 0.5f,
                        Rgba8{40, 40, 40, 255}, Rgba8{200, 100, 160, 255});
}

}  // namespace
