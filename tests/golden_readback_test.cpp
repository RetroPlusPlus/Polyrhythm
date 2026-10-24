// Golden-readback harness: compose a fixed battery of representative frames OFFSCREEN on a real GPU
// device, download the composed viewport, and diff it pixel-exactly against a committed per-backend
// golden. It is the engine's first device-backed test — every other test exercises the format-
// independent constexpr CPU mirrors and creates no device. It is compose-only and windowless (it
// builds a Renderer with no window, so it needs a GPU device but no display), which lets it run on a
// software rasterizer in CI — lavapipe (Vulkan) on Linux, WARP (D3D12) on Windows, Metal on the Mac.
//
// Each backend commits its own golden under tests/fixtures/golden/<backend>/. Set the environment
// variable RETROPP_CAPTURE_GOLDEN to (re)capture the goldens for the live backend — the first run on
// each platform writes them; every later run compares. A scene's tolerance tag is exact (arithmetic-
// free relocations / selections / constant writes) or within one 8-bit step per channel (the blend and
// feather composites, where per-pass rounding can shift a value by at most one).
//
// A device is REQUIRED on every production-representative platform (macOS/Metal, Windows-x64/D3D12,
// Linux/Vulkan): if none is reachable the test FAILS with the SDL reason rather than skipping. The one
// exception is Windows on ARM — a courtesy coverage runner with no production-representative GPU backend
// in CI (its real path, D3D12 + DXIL, is pixel-covered by the Windows x64 job), where a missing device
// is a documented out-of-scope skip. The only other skip is the transient capture window — a backend
// whose golden is not committed yet — which the capture run then closes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/curve.h"
#include "retropp/draw_state.h"
#include "retropp/geometry.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/viewport.h"

namespace {
using namespace retropp;

// A small, pitch-friendly viewport: 64 px wide → 256-byte rows, aligned for backend texture transfer.
constexpr int kW = 64;
constexpr int kH = 64;

// Per-scene comparison tolerance. Exact = byte-identical; OneStep = each channel within one 8-bit step
// (the blend / feather composites round per pass). In this 8-bit pipeline both pass exactly; the tag is
// the documented bar each scene is held to.
enum class Tol { Exact, OneStep };

// The CPU arch this binary was built for. A software rasterizer (lavapipe / WARP) is LLVM-codegen'd
// per arch, so its byte output can differ between x64 and ARM64; the golden key includes the arch so
// each runner owns its own exact golden rather than comparing across architectures.
const char* archTag() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x64";
#else
    return "unknown-arch";
#endif
}

// The live backend's fixture key: "<backend>-<arch>" (e.g. metal-arm64, vulkan-x64, d3d12-x64). SDL
// reports the GPU driver name; one key per (backend, arch) = one committed golden per runner.
std::string backendKey(const char* driver) {
    std::string b = "unknown";
    if (driver) {
        const std::string d = driver;
        if (d == "metal") b = "metal";
        else if (d == "vulkan") b = "vulkan";
        else if (d == "direct3d12") b = "d3d12";
        else b = d;  // an unmapped backend keeps its own key rather than colliding with a mapped one
    }
    return b + "-" + archTag();
}

// Windows on ARM is a courtesy coverage runner: in a VM it has no production-representative GPU backend
// (a Windows-ARM guest gets no D3D12 adapter, and SDL_GPU has no software fallback), and a real
// Windows-ARM device's production path is D3D12 with arch-independent DXIL — the same backend + the same
// shader bytecode the Windows x64 job already pixel-validates. So when no device is reachable HERE, the
// golden test is out of scope by design (a documented, visible skip). On every production-representative
// platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan) a missing device is a hard failure. A real
// device on this platform (a future hardware runner) still runs the test normally.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

std::filesystem::path goldenPath(const std::string& backend, const std::string& scene) {
    return std::filesystem::path(RETROPP_FIXTURES_DIR) / "golden" / backend / (scene + ".rgba");
}

// .rgba blob: 'RGBA' magic + width + height (uint32 LE) + width·height packed Rgba8 texels.
void writeBlob(const std::filesystem::path& p, int w, int h, const std::vector<Rgba8>& px) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    const std::uint32_t W = static_cast<std::uint32_t>(w);
    const std::uint32_t H = static_cast<std::uint32_t>(h);
    f.write("RGBA", 4);
    f.write(reinterpret_cast<const char*>(&W), sizeof(W));
    f.write(reinterpret_cast<const char*>(&H), sizeof(H));
    f.write(reinterpret_cast<const char*>(px.data()),
            static_cast<std::streamsize>(px.size() * sizeof(Rgba8)));
}

bool readBlob(const std::filesystem::path& p, int& w, int& h, std::vector<Rgba8>& px) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "RGBA", 4) != 0) return false;
    std::uint32_t W = 0, H = 0;
    f.read(reinterpret_cast<char*>(&W), sizeof(W));
    f.read(reinterpret_cast<char*>(&H), sizeof(H));
    w = static_cast<int>(W);
    h = static_cast<int>(H);
    px.resize(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    f.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size() * sizeof(Rgba8)));
    return static_cast<bool>(f);
}

::testing::AssertionResult compareGolden(const std::vector<Rgba8>& got,
                                         const std::vector<Rgba8>& want, int w, Tol tol) {
    if (got.size() != want.size()) {
        return ::testing::AssertionFailure()
               << "pixel count mismatch — got " << got.size() << ", want " << want.size();
    }
    const int maxDelta = (tol == Tol::Exact) ? 0 : 1;
    for (std::size_t i = 0; i < got.size(); ++i) {
        const Rgba8& a = got[i];
        const Rgba8& b = want[i];
        const int dr = std::abs(static_cast<int>(a.r) - static_cast<int>(b.r));
        const int dg = std::abs(static_cast<int>(a.g) - static_cast<int>(b.g));
        const int db = std::abs(static_cast<int>(a.b) - static_cast<int>(b.b));
        const int da = std::abs(static_cast<int>(a.a) - static_cast<int>(b.a));
        if (dr > maxDelta || dg > maxDelta || db > maxDelta || da > maxDelta) {
            const int x = static_cast<int>(i) % w;
            const int y = static_cast<int>(i) / w;
            return ::testing::AssertionFailure()
                   << "first mismatch at (" << x << ", " << y << "): got "
                   << static_cast<int>(a.r) << "," << static_cast<int>(a.g) << ","
                   << static_cast<int>(a.b) << "," << static_cast<int>(a.a) << "  want "
                   << static_cast<int>(b.r) << "," << static_cast<int>(b.g) << ","
                   << static_cast<int>(b.b) << "," << static_cast<int>(b.a)
                   << "  (allowed per-channel delta " << maxDelta << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

// A small deterministic indexed atlas (16×16 px = a 2×2 tile grid, indices 0..3 in a blocky pattern)
// and a 4-color palette. Shared by every scene so the composite is identical art across the battery.
struct BaseArt {
    AtlasId   atlas{};
    PaletteId palette{};
};

BaseArt uploadBaseArt(Renderer& r) {
    std::array<std::uint8_t, 16 * 16> idx{};
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            idx[static_cast<std::size_t>(y) * 16 + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(((x / 4) + (y / 4)) % 4);
        }
    }
    const AtlasId atlas = r.uploadAtlas(idx.data(), 16, 16).atlasId;  // no structural hole (default None)
    const std::array<Rgba8, 4> pal{{{20, 20, 30}, {200, 60, 60}, {60, 200, 90}, {230, 230, 240}}};
    const PaletteId palette = r.uploadPalette(std::span<const Rgba8>(pal));
    return {atlas, palette};
}

// Backing storage a scene's layer spans point into — must outlive the captureViewport call, so the
// caller owns these and passes them by reference.
struct SceneBacking {
    std::vector<PaletteId> palSet;
    std::vector<TileCell>  cells;
    std::vector<Sprite>    sprites;
};

// A tile layer (z 0, 8×8 tiles filling the viewport) + a sprite layer (z 10, two opaque sprites over
// index-0 holes). The base composite every scene starts from.
void addBaseScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas,
                         .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
        }
    }
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(b.cells)};
    frame.layers.push_back(bg);

    b.sprites = {Sprite{.key = "sp0", .x = 12, .y = 20, .atlas = art.atlas, .tile = 1, .palette = art.palette},
                 Sprite{.key = "sp1", .x = 40, .y = 36, .atlas = art.atlas, .tile = 3, .palette = art.palette}};
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    frame.layers.push_back(sp);
}

// A tile-only background (z 0, 8×8 tiles filling the viewport) — the base the crisp-parity scenes layer a
// single destination-driven path on top of. No sprites: transformed-sprite crispness is a separate scope, so the
// parity of the destination-driven paths is measured against a background that already upscales cleanly.
void addTileBackground(FrameDrawState& frame, const BaseArt& art, SceneBacking& b) {
    b.cells.resize(8 * 8);
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                TileCell{.atlas = art.atlas,
                         .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette};
        }
    }
    DrawLayer bg{.key = "bg"};
    bg.z       = 0;
    bg.size    = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(b.cells)};
    frame.layers.push_back(bg);
}

// Nearest-upscale a captured image by an integer factor: each source pixel becomes a scale×scale block.
// The reference the crisp-parity scenes compare a scaled compose against.
std::vector<Rgba8> nearestUpscale(const std::vector<Rgba8>& src, int w, int h, int scale) {
    const int ow = w * scale;
    std::vector<Rgba8> out(static_cast<std::size_t>(ow) * static_cast<std::size_t>(h * scale));
    for (int y = 0; y < h * scale; ++y) {
        for (int x = 0; x < ow; ++x) {
            out[static_cast<std::size_t>(y) * ow + x] =
                src[static_cast<std::size_t>(y / scale) * w + (x / scale)];
        }
    }
    return out;
}

// The crisp invariant, machine-checked: a Viewport-grid capture at compose `scale` must exactly equal the
// scale-1 capture nearest-upscaled. Every destination-driven analytic path snaps to the viewport
// grid, so scaling the compose only relocates whole viewport pixels onto the finer output grid — no
// per-output-pixel softening. Exact equality — no tolerance; a non-exact path is a defect, not a threshold.
void runCrispParity(const std::string& name, const FrameDrawState& frame, Renderer& r, int scale = 3) {
    r.evaluationGrid(EvaluationGrid::Viewport);
    const std::vector<Rgba8> one    = r.captureViewport(frame, 1);
    const std::vector<Rgba8> scaled = r.captureViewport(frame, scale);
    const std::vector<Rgba8> want   = nearestUpscale(one, kW, kH, scale);
    ASSERT_EQ(scaled.size(), want.size()) << name;
    EXPECT_TRUE(compareGolden(scaled, want, kW * scale, Tol::Exact)) << name;
}

class GoldenReadback : public ::testing::Test {
protected:
    static inline SDL_GPUDevice* device_ = nullptr;
    static inline std::string    backend_;
    static inline std::string    initError_;

    static void SetUpTestSuite() {
        // The GPU subsystem needs the video subsystem initialized (mirrors SdlPlatform). A headless
        // machine needs an offscreen video driver (SDL_VIDEODRIVER=offscreen) + an installed GPU device
        // (a software rasterizer is fine). A failure here is recorded and surfaced as a hard test
        // failure in SetUp with the SDL reason — the harness REQUIRES a device, it does not skip past one.
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            initError_ = std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
            return;
        }
        device_ = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
            /*debug_mode=*/false, /*name=*/nullptr);
        if (!device_) {
            initError_ = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
            return;
        }
        backend_ = backendKey(SDL_GetGPUDeviceDriver(device_));
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
                                "backend in CI; its production path (D3D12 + DXIL) is pixel-covered by the "
                                "Windows x64 job. ("
                             << initError_ << ")";
            }
            FAIL() << "no GPU device reachable — " << initError_
                   << ". The golden harness requires a GPU device on every production-representative "
                      "platform (macOS/Metal, Windows-x64/D3D12, Linux/Vulkan); install a GPU driver (a "
                      "software rasterizer such as lavapipe suffices) and, on a headless runner, set "
                      "SDL_VIDEODRIVER=offscreen so SDL video init succeeds.";
        }
    }

    // Capture `frame` and either write the live backend's golden (capture mode) or diff against the
    // committed one. A missing golden skips with a parity-gap message rather than passing.
    void runScene(const std::string& name, Tol tol, const FrameDrawState& frame, Renderer& r) {
        const std::vector<Rgba8> px = r.captureViewport(frame);
        ASSERT_EQ(px.size(), static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH));

        const std::filesystem::path path = goldenPath(backend_, name);
        if (std::getenv("RETROPP_CAPTURE_GOLDEN") != nullptr) {
            writeBlob(path, kW, kH, px);
            SUCCEED() << "captured golden " << path.string();
            return;
        }
        int gw = 0, gh = 0;
        std::vector<Rgba8> want;
        if (!readBlob(path, gw, gh, want)) {
            GTEST_SKIP() << "no committed golden for backend '" << backend_ << "' scene '" << name
                         << "' (" << path.string()
                         << ") — capture (RETROPP_CAPTURE_GOLDEN=1) + commit it to close the parity gap";
        }
        ASSERT_EQ(gw, kW);
        ASSERT_EQ(gh, kH);
        EXPECT_TRUE(compareGolden(px, want, kW, tol));
    }
};

// ── The battery ───────────────────────────────────────────────────────────────────────────────────

TEST_F(GoldenReadback, Default) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    runScene("default", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, Displace) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 4.0f,
                                                  .frequency = 2.0f,
                                                  .phase     = 0.25f,
                                                  .axis      = Axis::Horizontal,
                                                  .edge      = DisplacementEdge::Blank});
    runScene("displace", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, Ripple) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                                  .amplitude = 3.0f,
                                                  .frequency = 4.0f,
                                                  .phase     = 0.2f,
                                                  .center    = Point{32.0f, 32.0f},
                                                  .decay     = 1.5f});
    runScene("ripple", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, ColorfillSolid) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::rectangle(Point{16, 16}, 32, 32);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 140, 0, 255}}};
    frame.regions.push_back(reg);
    runScene("colorfill_solid", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, RegionSelect) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::triangle(Point{8, 8}, Point{56, 16}, Point{24, 52});
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{40, 120, 220, 255}}};
    frame.regions.push_back(reg);
    runScene("region_select", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, StencilHard) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 0.0f);
    runScene("stencil_hard", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, StencilFeather) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 6.0f);
    runScene("stencil_feather", Tol::OneStep, frame, r);
}

// One scene per BlendMode: a whole-frame ColorFill combined over the base composite with that mode.

TEST_F(GoldenReadback, BlendMultiply) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Multiply;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{128, 128, 255, 255}});
    runScene("blend_multiply", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendScreen) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Screen;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{80, 80, 120, 255}});
    runScene("blend_screen", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendAdd) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Add;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{60, 40, 20, 255}});
    runScene("blend_add", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendSubtract) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Subtract;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{60, 40, 20, 255}});
    runScene("blend_subtract", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, BlendHalf) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Half;
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 100, 50, 255}});
    runScene("blend_half", Tol::OneStep, frame, r);
}

TEST_F(GoldenReadback, CurveRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Curve c = Curve::quadratic(Vec2{12, 12}, Vec2{52, 8}, Vec2{52, 52});
    c.lineTo(Vec2{12, 52});  // fromCurve closes the loop back to the start
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::fromCurve(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 160, 255}}};
    frame.regions.push_back(reg);
    runScene("curve_region", Tol::Exact, frame, r);
}

TEST_F(GoldenReadback, CurveStencil) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addBaseScene(frame, art, b);
    Curve c = Curve::quadratic(Vec2{16, 16}, Vec2{48, 12}, Vec2{48, 48});
    c.quadraticTo(Vec2{16, 52}, Vec2{16, 16});
    frame.regions = stencil(ShapePoints::fromCurve(c), StencilMode::TransparentInside, 5.0f);
    runScene("curve_stencil", Tol::OneStep, frame, r);
}

// A Multiply ColorFill at fillIntensity > 1 BRIGHTENS the scene — a multiplicative exposure impossible at
// 8-bit (the fill would clamp to 1 before the blend, so Multiply could only darken). The float16
// intermediates carry the > 1 fill through to the blend. This proves the capability against a no-effect
// baseline of the same scene (always runs, no golden needed) AND pins the exact result with a committed
// golden.
TEST_F(GoldenReadback, MultiplyBrighten) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // Baseline: the base composite with no whole-frame color.
    FrameDrawState base;
    SceneBacking  bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // The brighten scene: a white whole-frame ColorFill at fillIntensity 1.5, Multiply — scene · 1.5.
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.blend = BlendMode::Multiply;
    frame.postEffects.push_back(ScreenSpaceEffect{
        .kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{255, 255, 255, 255}, .fillIntensity = 1.5f});
    const std::vector<Rgba8> brightened = r.captureViewport(frame);

    // Capability: no channel is darker than the baseline, and at least one is strictly brighter.
    ASSERT_EQ(baseline.size(), brightened.size());
    bool        anyBrighter = false;
    bool        anyDarker   = false;
    std::size_t darkerAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (brightened[i].r < baseline[i].r || brightened[i].g < baseline[i].g ||
            brightened[i].b < baseline[i].b) {
            anyDarker = true;
            darkerAt  = i;
            break;
        }
        if (brightened[i].r > baseline[i].r || brightened[i].g > baseline[i].g ||
            brightened[i].b > baseline[i].b) {
            anyBrighter = true;
        }
    }
    EXPECT_FALSE(anyDarker) << "Multiply at fillIntensity 1.5 darkened pixel " << darkerAt
                            << " — a multiplicative exposure must not dim the scene";
    EXPECT_TRUE(anyBrighter) << "Multiply ColorFill at fillIntensity 1.5 brightened no pixel — the float16 "
                                "headroom is not reaching the blend";

    // Regression: the exact composited result is pinned by a committed golden (arithmetic composite → OneStep).
    runScene("multiply_brighten", Tol::OneStep, frame, r);
}

// The built-in Gleam effect on-device: a luminance-keyed diagonal sheen band. Two behavioral properties,
// checked per-backend against a no-effect baseline of the same scene (no committed golden image — the exact
// per-pixel math is the device-free applyGleam mirror; this pins the GPU path's behavior):
//   - gain 0 is an EXACT identity — the scene is byte-identical to no effect at all (the default no-op).
//   - gain > 0 only ever BRIGHTENS (multiply by >= 1 plus a luminance-keyed lift): no pixel darkens, and at
//     least one in-band pixel is brighter.
TEST_F(GoldenReadback, Gleam) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // Baseline: the base composite, no effect.
    FrameDrawState base;
    SceneBacking   bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // Identity: a whole-frame Gleam at gain 0 leaves the scene byte-identical.
    FrameDrawState id;
    SceneBacking   ib;
    addBaseScene(id, art, ib);
    id.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Gleam, .gain = 0.0f});
    const std::vector<Rgba8> identity = r.captureViewport(id);
    ASSERT_EQ(baseline.size(), identity.size());
    bool        identical = true;
    std::size_t diffAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (!(baseline[i].r == identity[i].r && baseline[i].g == identity[i].g &&
              baseline[i].b == identity[i].b && baseline[i].a == identity[i].a)) {
            identical = false;
            diffAt    = i;
            break;
        }
    }
    EXPECT_TRUE(identical) << "Gleam at gain 0 differs from no effect at pixel " << diffAt
                           << " — the default (gain 0) must be an exact identity";

    // Sheen: a wide diagonal band at gain 1 across the scene — brightens, never darkens.
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{
        .kind = ScreenSpaceEffectKind::Gleam, .sweep = 0.6f, .width = 0.6f, .gain = 1.0f, .slant = 0.35f});
    const std::vector<Rgba8> gleamed = r.captureViewport(frame);

    ASSERT_EQ(baseline.size(), gleamed.size());
    bool        anyBrighter = false;
    bool        anyDarker   = false;
    std::size_t darkerAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (gleamed[i].r < baseline[i].r || gleamed[i].g < baseline[i].g || gleamed[i].b < baseline[i].b) {
            anyDarker = true;
            darkerAt  = i;
            break;
        }
        if (gleamed[i].r > baseline[i].r || gleamed[i].g > baseline[i].g || gleamed[i].b > baseline[i].b) {
            anyBrighter = true;
        }
    }
    EXPECT_FALSE(anyDarker) << "Gleam darkened pixel " << darkerAt << " — the sheen only ever brightens";
    EXPECT_TRUE(anyBrighter) << "Gleam at gain 1 brightened no pixel — the band is not reaching the scene";
}

// The built-in ColorSaturation effect on-device: a cross-channel pull toward each pixel's luminance. Two
// behavioral properties, checked per-backend against a no-effect baseline of the same scene (no committed
// golden image — the exact per-pixel math is the device-free applySaturation mirror; this pins the GPU path's
// behavior):
//   - saturation 255 is an EXACT identity — the scene is byte-identical to no effect at all (the default no-op).
//   - saturation 0 is grayscale — every pixel's channel spread (max(rgb) - min(rgb)) collapses to zero, no
//     pixel gains spread, and at least one colored pixel actually drained.
TEST_F(GoldenReadback, ColorSaturation) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // Baseline: the base composite, no effect.
    FrameDrawState base;
    SceneBacking   bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // Identity: a whole-frame ColorSaturation at saturation 255 leaves the scene byte-identical.
    FrameDrawState id;
    SceneBacking   ib;
    addBaseScene(id, art, ib);
    id.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 255});
    const std::vector<Rgba8> identity = r.captureViewport(id);
    ASSERT_EQ(baseline.size(), identity.size());
    bool        identical = true;
    std::size_t diffAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (!(baseline[i].r == identity[i].r && baseline[i].g == identity[i].g &&
              baseline[i].b == identity[i].b && baseline[i].a == identity[i].a)) {
            identical = false;
            diffAt    = i;
            break;
        }
    }
    EXPECT_TRUE(identical) << "ColorSaturation at 255 differs from no effect at pixel " << diffAt
                           << " — the default (saturation 255) must be an exact identity";

    // Grayscale: a whole-frame ColorSaturation at saturation 0 drains every pixel to its luminance.
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorSaturation, .saturation = 0});
    const std::vector<Rgba8> grey = r.captureViewport(frame);

    ASSERT_EQ(baseline.size(), grey.size());
    auto spread = [](const Rgba8& p) {
        const int hi = std::max({p.r, p.g, p.b});
        const int lo = std::min({p.r, p.g, p.b});
        return hi - lo;
    };
    bool        anyDrained = false;   // a colored pixel that lost its spread
    bool        anyGained  = false;   // a pixel more saturated than the baseline (must not happen)
    std::size_t gainedAt   = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        const int before = spread(baseline[i]);
        const int after  = spread(grey[i]);
        if (after > before + 2) {  // +2 tolerance for 8-bit output quantization
            anyGained = true;
            gainedAt  = i;
            break;
        }
        if (before >= 16 && after <= 2) anyDrained = true;
    }
    EXPECT_FALSE(anyGained) << "ColorSaturation at 0 raised a pixel's colour spread at " << gainedAt
                            << " — saturation 0 only ever drains toward grey";
    EXPECT_TRUE(anyDrained) << "ColorSaturation at 0 drained no coloured pixel to grey — the scene never "
                               "desaturated";
}

// The built-in Bloom effect on-device: a threshold-blur-add glow (one 2-D gather pass).
// Three behavioral properties, checked per-backend against baselines of the same scenes (no committed
// golden image — the exact math is the device-free applyBrightpass / gaussianKernelWeight / applyBloomAdd
// mirror; this pins the GPU sub-chain's behavior):
//   - intensity 0 is an EXACT identity — the scene is byte-identical to no effect at all (the default no-op).
//   - a whole-frame Bloom only ever ADDS light: no pixel darkens, and pixels near bright content gain.
//   - a Bloom SPRITE radiates past its art: a pixel outside the sprite's quad within the halo reach gains
//     glow over the background, and a pixel beyond the reach stays byte-identical.
TEST_F(GoldenReadback, Bloom) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // Baseline: the base composite, no effect.
    FrameDrawState base;
    SceneBacking   bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    // Identity: a whole-frame Bloom at intensity 0 leaves the scene byte-identical.
    FrameDrawState id;
    SceneBacking   ib;
    addBaseScene(id, art, ib);
    id.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 6.0f});
    const std::vector<Rgba8> identity = r.captureViewport(id);
    ASSERT_EQ(baseline.size(), identity.size());
    bool        identical = true;
    std::size_t diffAt    = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        if (!(baseline[i].r == identity[i].r && baseline[i].g == identity[i].g &&
              baseline[i].b == identity[i].b && baseline[i].a == identity[i].a)) {
            identical = false;
            diffAt    = i;
            break;
        }
    }
    EXPECT_TRUE(identical) << "Bloom at intensity 0 differs from no effect at pixel " << diffAt
                           << " — the default (intensity 0) must be an exact identity";

    // Additive: a whole-frame Bloom never darkens, and some pixel gains glow.
    FrameDrawState glowFrame;
    SceneBacking   gb;
    addBaseScene(glowFrame, art, gb);
    glowFrame.postEffects.push_back(
        ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 4.0f, .intensity = 255});
    const std::vector<Rgba8> glowed = r.captureViewport(glowFrame);
    ASSERT_EQ(baseline.size(), glowed.size());
    bool        anyDarker = false, anyBrighter = false;
    std::size_t darkerAt = 0;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        const int before = baseline[i].r + baseline[i].g + baseline[i].b;
        const int after  = glowed[i].r + glowed[i].g + glowed[i].b;
        if (after < before - 2) { anyDarker = true; darkerAt = i; break; }
        if (after > before + 2) anyBrighter = true;
    }
    EXPECT_FALSE(anyDarker) << "Bloom darkened pixel " << darkerAt << " — the glow only ever adds light";
    EXPECT_TRUE(anyBrighter) << "Bloom at intensity 255 brightened no pixel — the glow is not reaching the scene";

    // The sprite halo: a solid bright 8×8 sprite over a dark scene, chain Bloom radius 6 — the glow lands
    // OUTSIDE the sprite's quad. Probe one pixel 3 px past the right edge (inside the halo reach) and one
    // 20 px away (beyond it).
    std::array<std::uint8_t, 8 * 8> solidIdx{};
    const AtlasId solidAtlas = r.uploadAtlas(solidIdx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> solidPal{{{255, 240, 200, 255}, {255, 240, 200, 255},
                                         {255, 240, 200, 255}, {255, 240, 200, 255}}};
    const PaletteId solidPalId = r.uploadPalette(std::span<const Rgba8>(solidPal));

    auto spriteScene = [&](bool bloom, SceneBacking& backing, std::vector<Sprite>& keep) {
        FrameDrawState f;
        addBaseScene(f, art, backing);
        Sprite s{.key = "halo", .x = 24, .y = 24, .atlas = solidAtlas, .tile = 0, .palette = solidPalId};
        s.size = AssetDimensions{8, 8};
        if (bloom)
            s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Bloom, .radius = 6.0f,
                                           .intensity = 255}};
        keep = {s};
        DrawLayer sp{.key = "halo_layer"};
        sp.z = 50; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        f.layers.push_back(sp);
        return f;
    };
    SceneBacking nb, hb;
    std::vector<Sprite> ns, hs;
    FrameDrawState noBloom  = spriteScene(false, nb, ns);
    FrameDrawState haloed   = spriteScene(true, hb, hs);
    const std::vector<Rgba8> plain = r.captureViewport(noBloom);
    const std::vector<Rgba8> halo  = r.captureViewport(haloed);
    ASSERT_EQ(plain.size(), halo.size());
    auto sum = [&](const std::vector<Rgba8>& img, int x, int y) {
        const Rgba8 p = img[static_cast<std::size_t>(y) * kW + x];
        return p.r + p.g + p.b;
    };
    // (35, 28): 3 px past the sprite's right edge (x 24..31), within the 6 px reach → gains glow.
    EXPECT_GT(sum(halo, 35, 28), sum(plain, 35, 28) + 2)
        << "no glow 3 px past the sprite edge — the halo is not radiating beyond the art";
    // (52, 28): 20 px past the edge, beyond the reach → byte-identical to the plain sprite scene.
    EXPECT_LE(std::abs(sum(halo, 52, 28) - sum(plain, 52, 28)), 2)
        << "the halo reached past its radius — the footprint inflation is leaking";
}

// The built-in Glow effect on-device: an authored-color aura (one 2-D gather pass over a scalar emission
// mask, times the tint). Behavioral properties per-backend against live baselines (no committed golden
// image — the exact math is the device-free glowMask / gaussianKernelWeight / applyGlowAdd mirror):
//   - intensity 0 AND radius 0 are each an EXACT identity (the default no-op; radius 0 at full intensity too).
//   - THE CHROMA SIGNATURE: a dark-BLUE sprite with threshold 0 and an ember tint radiates a halo whose hue
//     is the tint — the gained light is red-dominant, the source's blue absent from the added term.
//   - the aura radiates past the sprite's quad within the radius, and a pixel beyond the reach stays
//     byte-identical.
//   - threshold 255 keys out everything (nothing survives) — byte-identical.
TEST_F(GoldenReadback, Glow) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    constexpr Rgba8 kEmber{.r = 255, .g = 66, .b = 26, .a = 255};

    // Baseline: the base composite, no effect.
    FrameDrawState base;
    SceneBacking   bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    auto expectIdentical = [&](const std::vector<Rgba8>& img, const char* what) {
        ASSERT_EQ(baseline.size(), img.size());
        bool        identical = true;
        std::size_t diffAt    = 0;
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            if (!(baseline[i].r == img[i].r && baseline[i].g == img[i].g &&
                  baseline[i].b == img[i].b && baseline[i].a == img[i].a)) {
                identical = false;
                diffAt    = i;
                break;
            }
        }
        EXPECT_TRUE(identical) << what << " differs from no effect at pixel " << diffAt
                               << " — it must be an exact identity";
    };

    // Identity: intensity 0 (the default) with a reach set.
    FrameDrawState id;
    SceneBacking   ib;
    addBaseScene(id, art, ib);
    id.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow,
                                               .fill = kEmber, .radius = 6.0f});
    expectIdentical(r.captureViewport(id), "Glow at intensity 0");

    // Identity: radius 0 at full intensity — no reach gates the aura off entirely.
    FrameDrawState idR;
    SceneBacking   irb;
    addBaseScene(idR, art, irb);
    idR.postEffects.push_back(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow,
                                                .fill = kEmber, .intensity = 255});
    expectIdentical(r.captureViewport(idR), "Glow at radius 0");

    // The chroma signature + the reach: a dark-BLUE solid 8×8 sprite over a NEAR-BLACK backdrop (so the
    // aura's added light reads directly, not against a bright scene the alpha-over would trade away), chain
    // Glow radius 6, threshold 0 (whole-silhouette emission — dark art radiates), ember tint.
    std::array<std::uint8_t, 8 * 8> solidIdx{};
    const AtlasId solidAtlas = r.uploadAtlas(solidIdx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> bluePal{{{.r = 8, .g = 12, .b = 96, .a = 255},
                                        {.r = 8, .g = 12, .b = 96, .a = 255},
                                        {.r = 8, .g = 12, .b = 96, .a = 255},
                                        {.r = 8, .g = 12, .b = 96, .a = 255}}};
    const PaletteId bluePalId = r.uploadPalette(std::span<const Rgba8>(bluePal));
    const std::array<Rgba8, 1> darkPal{{{.r = 8, .g = 10, .b = 14, .a = 255}}};
    const PaletteId darkPalId = r.uploadPalette(std::span<const Rgba8>(darkPal));

    std::vector<TileCell> darkCells(8 * 8, TileCell{.atlas = solidAtlas, .tile = 0, .palette = darkPalId});
    auto spriteScene = [&](bool glow, std::vector<Sprite>& keep) {
        FrameDrawState f;
        DrawLayer bg{.key = "dark_bg"};
        bg.z = 0; bg.size = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                 .cells = std::span<const TileCell>(darkCells)};
        f.layers.push_back(bg);
        Sprite s{.key = "aura", .x = 24, .y = 24, .atlas = solidAtlas, .tile = 0, .palette = bluePalId};
        s.size = AssetDimensions{8, 8};
        if (glow)
            s.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .fill = kEmber,
                                           .radius = 6.0f, .intensity = 255}};
        keep = {s};
        DrawLayer sp{.key = "aura_layer"};
        sp.z = 50; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> ns, gs;
    FrameDrawState plainFrame = spriteScene(false, ns);
    FrameDrawState glowFrame  = spriteScene(true, gs);
    const std::vector<Rgba8> plain = r.captureViewport(plainFrame);
    const std::vector<Rgba8> glowed = r.captureViewport(glowFrame);
    ASSERT_EQ(plain.size(), glowed.size());
    auto at = [&](const std::vector<Rgba8>& img, int x, int y) {
        return img[static_cast<std::size_t>(y) * kW + x];
    };
    // (35, 28): 3 px past the sprite's right edge (x 24..31), within the 6 px reach. The gained light's hue
    // is the ember tint: red-dominant, the source's blue nowhere in it.
    const int rGain = at(glowed, 35, 28).r - at(plain, 35, 28).r;
    const int gGain = at(glowed, 35, 28).g - at(plain, 35, 28).g;
    const int bGain = at(glowed, 35, 28).b - at(plain, 35, 28).b;
    EXPECT_GT(rGain, 10) << "no aura 3 px past the sprite edge — the halo is not radiating beyond the art";
    EXPECT_GT(rGain, bGain * 3)
        << "the halo is not tint-hued (r " << rGain << " vs b " << bGain
        << ") — source hue is leaking into the aura, or the tint is not applied";
    EXPECT_GT(rGain, gGain) << "the halo's channel order does not match the ember tint";
    // (52, 28): 20 px past the edge, beyond the reach → byte-identical to the plain sprite scene.
    const Rgba8 farG = at(glowed, 52, 28), farP = at(plain, 52, 28);
    EXPECT_LE(std::abs(int(farG.r) - int(farP.r)) + std::abs(int(farG.g) - int(farP.g)) +
                  std::abs(int(farG.b) - int(farP.b)),
              2)
        << "the aura reached past its radius — the footprint inflation is leaking";

    // threshold 255: nothing survives the key — byte-identical to the plain sprite scene.
    std::vector<Sprite> ts;
    FrameDrawState keyed = spriteScene(false, ts);
    Sprite hot{.key = "aura", .x = 24, .y = 24, .atlas = solidAtlas, .tile = 0, .palette = bluePalId};
    hot.size = AssetDimensions{8, 8};
    hot.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::Glow, .fill = kEmber,
                                     .radius = 6.0f, .threshold = 255, .intensity = 255}};
    ts = {hot};
    keyed.layers.back().content = SpriteContent{.sprites = std::span<const Sprite>(ts)};
    const std::vector<Rgba8> keyedImg = r.captureViewport(keyed);
    ASSERT_EQ(plain.size(), keyedImg.size());
    bool keyedIdentical = true;
    std::size_t keyedDiff = 0;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        if (!(plain[i].r == keyedImg[i].r && plain[i].g == keyedImg[i].g &&
              plain[i].b == keyedImg[i].b && plain[i].a == keyedImg[i].a)) {
            keyedIdentical = false;
            keyedDiff = i;
            break;
        }
    }
    EXPECT_TRUE(keyedIdentical) << "Glow at threshold 255 emitted at pixel " << keyedDiff
                                << " — a maxed threshold must key out everything";
}

// A Below-scope Bloom / Glow is a lens onto the SCENE's own light: the halo is made from the accumulator
// beneath the lens and shows through the lens's silhouette. Property-based like the other emission clauses —
// what is pinned is that a halo lands inside the coverage, that the scene outside it is untouched, and that a
// lens carrying TWO distinct emission steps gets both in ONE draw (the case the field array exists for).
TEST_F(GoldenReadback, SpriteBelowEmission) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    constexpr Rgba8 kEmber{.r = 255, .g = 66, .b = 26, .a = 255};

    // A near-black field with one bright blue patch — the scene's light, which is what a below lens radiates.
    std::array<std::uint8_t, 8 * 8> solidIdx{};
    const AtlasId solidAtlas = r.uploadAtlas(solidIdx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> darkPal{{{.r = 6, .g = 8, .b = 12, .a = 255}}};
    const PaletteId darkPalId = r.uploadPalette(std::span<const Rgba8>(darkPal));
    const std::array<Rgba8, 1> brightPal{{{.r = 100, .g = 200, .b = 255, .a = 255}}};
    const PaletteId brightPalId = r.uploadPalette(std::span<const Rgba8>(brightPal));
    const std::array<Rgba8, 1> maskPal{{{.r = 255, .g = 255, .b = 255, .a = 255}}};
    const PaletteId maskPalId = r.uploadPalette(std::span<const Rgba8>(maskPal));

    std::vector<TileCell> darkCells(8 * 8, TileCell{.atlas = solidAtlas, .tile = 0, .palette = darkPalId});

    // Which emission steps the lens carries. The lens sits at (20,20)–(35,35) — its silhouette — with the
    // bright patch at (28,28)–(35,35) inside it, so the halo has something to radiate from and the reach
    // stays clear of the viewport edge.
    enum class Lens { None, BloomOnly, GlowOnly, Both };
    auto sceneWith = [&](Lens lens, std::vector<Sprite>& emitterKeep, std::vector<Sprite>& lensKeep) {
        FrameDrawState f;
        DrawLayer bg{.key = "dark_bg"};
        bg.z = 0; bg.size = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                 .cells = std::span<const TileCell>(darkCells)};
        f.layers.push_back(bg);

        Sprite emitter{.key = "emitter", .x = 28, .y = 28,
                       .atlas = solidAtlas, .tile = 0, .palette = brightPalId};
        emitter.size = AssetDimensions{8, 8};
        emitterKeep  = {emitter};
        DrawLayer el{.key = "emitter_layer"};
        el.z = 10; el.size = PixelSize{kW, kH};
        el.content = SpriteContent{.sprites = std::span<const Sprite>(emitterKeep)};
        f.layers.push_back(el);

        if (lens != Lens::None) {
            // An opaque 16×16 mask: its art is the coverage, never drawn.
            Sprite l{.key = "lens", .x = 20, .y = 20,
                     .atlas = solidAtlas, .tile = 0, .palette = maskPalId};
            l.size = AssetDimensions{16, 16};
            // Two genuinely distinct parameter sets, so the lens needs two fields. The threshold keys out the
            // near-black surround (luminance ≈ 0.03) and passes the bright patch (≈ 0.51); both reaches
            // comfortably cover the 3 px from the patch to the probe point.
            const ScreenSpaceEffect bloom{.kind      = ScreenSpaceEffectKind::Bloom,
                                          .scope     = ScreenSpaceEffectScope::Below,
                                          .radius    = 5.0f, .threshold = 26, .intensity = 255};
            const ScreenSpaceEffect glow{.kind      = ScreenSpaceEffectKind::Glow,
                                         .scope     = ScreenSpaceEffectScope::Below,
                                         .fill      = kEmber,
                                         .radius    = 12.0f, .threshold = 26, .intensity = 255};
            if (lens == Lens::BloomOnly)      l.effects = {bloom};
            else if (lens == Lens::GlowOnly)  l.effects = {glow};
            else                              l.effects = {bloom, glow};
            lensKeep = {l};
            DrawLayer ll{.key = "lens_layer"};
            ll.z = 50; ll.size = PixelSize{kW, kH};
            ll.content = SpriteContent{.sprites = std::span<const Sprite>(lensKeep)};
            f.layers.push_back(ll);
        }
        return f;
    };

    std::vector<Sprite> e0, l0, e1, l1, e2, l2, e3, l3;
    FrameDrawState fNone  = sceneWith(Lens::None, e0, l0);
    FrameDrawState fBloom = sceneWith(Lens::BloomOnly, e1, l1);
    FrameDrawState fGlow  = sceneWith(Lens::GlowOnly, e2, l2);
    FrameDrawState fBoth  = sceneWith(Lens::Both, e3, l3);
    const std::vector<Rgba8> none  = r.captureViewport(fNone);
    const std::vector<Rgba8> bloom = r.captureViewport(fBloom);
    const std::vector<Rgba8> glow  = r.captureViewport(fGlow);
    const std::vector<Rgba8> both  = r.captureViewport(fBoth);
    ASSERT_EQ(none.size(), both.size());

    auto at = [&](const std::vector<Rgba8>& img, int x, int y) {
        return img[static_cast<std::size_t>(y) * kW + x];
    };

    // (25, 31): inside the silhouette, 5 px from the bright patch — the halo lands here.
    EXPECT_GT(int(at(both, 25, 31).r) + int(at(both, 25, 31).g) + int(at(both, 25, 31).b),
              int(at(none, 25, 31).r) + int(at(none, 25, 31).g) + int(at(none, 25, 31).b))
        << "no halo inside the lens silhouette — the below emission field is not reaching the lens";

    // The two steps are independent additions onto the same running color, so carrying both must gain
    // exactly what carrying each alone gains.
    for (int ch = 0; ch < 3; ++ch) {
        auto chan = [&](const Rgba8& p) { return ch == 0 ? int(p.r) : ch == 1 ? int(p.g) : int(p.b); };
        const int gainBloom = chan(at(bloom, 25, 31)) - chan(at(none, 25, 31));
        const int gainGlow  = chan(at(glow, 25, 31))  - chan(at(none, 25, 31));
        const int gainBoth  = chan(at(both, 25, 31))  - chan(at(none, 25, 31));
        EXPECT_NEAR(gainBoth, gainBloom + gainGlow, 2)
            << "channel " << ch << ": a two-field lens gained " << gainBoth << " where its steps gain "
            << gainBloom << " + " << gainGlow << " — the two steps are not composing independently";
    }

    // (21, 31): still inside the silhouette but 7 px from the patch — past the Bloom's 5 px reach and well
    // inside the Glow's 12. The two fields therefore hold VERY different light here, which is what makes this
    // point decide the question the array exists to answer: each step must read ITS OWN layer. A lens that
    // read one field for both steps would show the Bloom's nothing where the Glow's aura belongs.
    const int farBloomGain = int(at(bloom, 21, 31).r) - int(at(none, 21, 31).r);
    const int farGlowGain  = int(at(glow, 21, 31).r)  - int(at(none, 21, 31).r);
    const int farBothGain  = int(at(both, 21, 31).r)  - int(at(none, 21, 31).r);
    EXPECT_LE(farBloomGain, 1) << "the Bloom reached 7 px on a 5 px radius — its field is not reach-bounded";
    EXPECT_GT(farGlowGain, 5) << "the Glow did not reach 7 px on a 12 px radius — its field is not resolving";
    EXPECT_NEAR(farBothGain, farGlowGain, 2)
        << "a two-field lens gained " << farBothGain << " where its Glow alone gains " << farGlowGain
        << " — the steps are not indexing their own array layers";

    // The tint signature separates the two: the Glow's ember is red-dominant while the Bloom carries the
    // scene's blue, so each step must move the channel its own authority says it should.
    EXPECT_GT(int(at(glow, 25, 31).r) - int(at(none, 25, 31).r),
              int(at(glow, 25, 31).b) - int(at(none, 25, 31).b))
        << "the below Glow's halo is not tint-hued — scene hue is leaking into an authored-colour aura";
    EXPECT_GT(int(at(bloom, 25, 31).b) - int(at(none, 25, 31).b),
              int(at(bloom, 25, 31).r) - int(at(none, 25, 31).r))
        << "the below Bloom's halo is not carrying the scene's own chroma";

    // Outside the silhouette the scene is untouched — a lens is confined to its coverage, and the halo it
    // makes must not spill into the accumulator around it.
    bool   surroundIdentical = true;
    int    firstX = -1, firstY = -1;
    for (int y = 0; y < kH && surroundIdentical; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (x >= 20 && x < 36 && y >= 20 && y < 36) continue;  // the silhouette itself
            const Rgba8 a = at(none, x, y), b = at(both, x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
                surroundIdentical = false;
                firstX = x; firstY = y;
                break;
            }
        }
    }
    EXPECT_TRUE(surroundIdentical)
        << "the scene outside the lens changed at (" << firstX << ", " << firstY
        << ") — a below halo is escaping the silhouette";
}

TEST_F(GoldenReadback, Swirl) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // A disc centered in the 64×64 viewport, comfortably clear of the borders.
    constexpr Point kCenter{32.0f, 32.0f};
    constexpr float kRadius = 20.0f;

    FrameDrawState base;
    SceneBacking   bb;
    addBaseScene(base, art, bb);
    const std::vector<Rgba8> baseline = r.captureViewport(base);

    auto sceneWith = [&](float amplitudeDeg, float radius, SceneBacking& b) {
        FrameDrawState f;
        addBaseScene(f, art, b);
        ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Swirl};
        e.amplitude = amplitudeDeg;
        e.center    = kCenter;
        e.radius    = radius;
        f.postEffects.push_back(e);
        return f;
    };
    auto expectIdentical = [&](const std::vector<Rgba8>& img, const char* what) {
        ASSERT_EQ(baseline.size(), img.size());
        bool        identical = true;
        std::size_t diffAt    = 0;
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            if (!(baseline[i].r == img[i].r && baseline[i].g == img[i].g &&
                  baseline[i].b == img[i].b && baseline[i].a == img[i].a)) {
                identical = false;
                diffAt    = i;
                break;
            }
        }
        EXPECT_TRUE(identical) << what << " differs from no effect at pixel " << diffAt
                               << " — it must be an exact identity";
    };

    // Identity: a zero turn (the default) with a disc set, and a full turn with no disc.
    SceneBacking zb, rb;
    expectIdentical(r.captureViewport(sceneWith(0.0f, kRadius, zb)), "Swirl at 0 degrees");
    expectIdentical(r.captureViewport(sceneWith(180.0f, 0.0f, rb)), "Swirl at radius 0");

    // A real twist: content inside the disc moves, content outside is untouched byte for byte.
    SceneBacking cwB, ccwB;
    const std::vector<Rgba8> cw  = r.captureViewport(sceneWith(140.0f, kRadius, cwB));
    const std::vector<Rgba8> ccw = r.captureViewport(sceneWith(-140.0f, kRadius, ccwB));
    ASSERT_EQ(baseline.size(), cw.size());
    ASSERT_EQ(baseline.size(), ccw.size());
    auto at = [&](const std::vector<Rgba8>& img, int x, int y) {
        return img[static_cast<std::size_t>(y) * kW + x];
    };
    auto same = [](Rgba8 a, Rgba8 b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    // Detectability — the twist changes the image inside the disc.
    int changedInside = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - kCenter.x;
            const float dy = static_cast<float>(y) + 0.5f - kCenter.y;
            if (std::sqrt(dx * dx + dy * dy) < kRadius && !same(at(baseline, x, y), at(cw, x, y)))
                ++changedInside;
        }
    EXPECT_GT(changedInside, 40)
        << "the swirl changed almost nothing inside its disc — the twist is not reaching the shader";

    // Direction — opposite signs are different images (a sign that collapsed would make them equal).
    int differBySign = 0;
    for (std::size_t i = 0; i < cw.size(); ++i)
        if (!same(cw[i], ccw[i])) ++differBySign;
    EXPECT_GT(differBySign, 40)
        << "clockwise and counter-clockwise twists produced the same image — the sign is being dropped";

    // Outside the disc: every pixel byte-identical to no effect, on BOTH directions. The read never leaves
    // the disc, so an untouched pixel is exactly untouched — not merely close.
    std::size_t outsideDiffs = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - kCenter.x;
            const float dy = static_cast<float>(y) + 0.5f - kCenter.y;
            if (std::sqrt(dx * dx + dy * dy) <= kRadius + 2.0f) continue;  // skip the rim's own cell
            if (!same(at(baseline, x, y), at(cw, x, y)) || !same(at(baseline, x, y), at(ccw, x, y)))
                ++outsideDiffs;
        }
    EXPECT_EQ(outsideDiffs, 0u)
        << "the swirl altered " << outsideDiffs
        << " pixels beyond its disc — the radius gate is leaking";

    // A sprite-chain swirl re-reads the sprite's own art and stays within its inflated footprint.
    std::array<std::uint8_t, 8 * 8> solidIdx{};
    for (std::size_t i = 0; i < solidIdx.size(); ++i) solidIdx[i] = (i % 8 < 4) ? 0 : 1;  // an asymmetric half
    const AtlasId solidAtlas = r.uploadAtlas(solidIdx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> pal{{{.r = 220, .g = 40, .b = 40, .a = 255},
                                    {.r = 40, .g = 220, .b = 60, .a = 255}}};
    const PaletteId palId = r.uploadPalette(std::span<const Rgba8>(pal));

    auto spriteScene = [&](bool swirl, std::vector<Sprite>& keep) {
        FrameDrawState f;
        Sprite s{.key = "vortex", .x = 24, .y = 24, .atlas = solidAtlas, .tile = 0, .palette = palId};
        s.size = AssetDimensions{8, 8};
        if (swirl) {
            ScreenSpaceEffect e{.kind = ScreenSpaceEffectKind::Swirl};
            e.amplitude = 160.0f;
            e.center    = Point{4.0f, 4.0f};  // the sprite's OWN art px
            e.radius    = 4.0f;
            s.effects   = {e};
        }
        keep = {s};
        DrawLayer sp{.key = "vortex_layer"};
        sp.z = 50; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        f.layers.push_back(sp);
        return f;
    };
    std::vector<Sprite> ps, ws;
    FrameDrawState plainFrame   = spriteScene(false, ps);
    FrameDrawState swirledFrame = spriteScene(true, ws);
    const std::vector<Rgba8> plainSprite   = r.captureViewport(plainFrame);
    const std::vector<Rgba8> swirledSprite = r.captureViewport(swirledFrame);
    ASSERT_EQ(plainSprite.size(), swirledSprite.size());

    int spriteChanged = 0, outsideFootprint = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            if (same(at(plainSprite, x, y), at(swirledSprite, x, y))) continue;
            ++spriteChanged;
            // The chord bound is min(twist, 2)·radius ≈ 11 px around the 8×8 art at (24,24).
            if (x < 24 - 12 || x > 31 + 12 || y < 24 - 12 || y > 31 + 12) ++outsideFootprint;
        }
    EXPECT_GT(spriteChanged, 4) << "a sprite-chain swirl changed nothing — the pre-pass is not running";
    EXPECT_EQ(outsideFootprint, 0)
        << "a sprite swirl wrote " << outsideFootprint
        << " pixels beyond its inflated footprint — the displacement bound is wrong";
}

// 90° rotation on the tile + sprite paths. The base tiles are asymmetric under a quarter turn, so a
// Rot90 layer must change the composed pixels (the always-runs capability check, no golden needed). The
// scene also drives a Rot90 sprite and a non-square (8×16) sprite at Rot270 — whose read transposes — so
// both GPU rotation paths are exercised, then pins the exact composite with a committed per-backend golden.
TEST_F(GoldenReadback, TileSpriteRotation) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    const auto buildTiles = [&](Rotation rot, SceneBacking& b, FrameDrawState& frame) {
        b.cells.resize(8 * 8);
        for (int ty = 0; ty < 8; ++ty) {
            for (int tx = 0; tx < 8; ++tx) {
                b.cells[static_cast<std::size_t>(ty) * 8 + static_cast<std::size_t>(tx)] =
                    TileCell{.atlas = art.atlas,
                             .tile = static_cast<std::uint16_t>((tx + ty) % 4), .palette = art.palette, .rotation = rot};
            }
        }
        DrawLayer bg{.key = "bg"};
        bg.z       = 0;
        bg.size    = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                 .cells = std::span<const TileCell>(b.cells)};
        frame.layers.push_back(bg);
    };

    // Capability: an unrotated tile layer vs the same layer at Rot90 — the asymmetric tiles must differ.
    FrameDrawState none;
    SceneBacking   bn;
    buildTiles(Rotation::None, bn, none);
    const std::vector<Rgba8> unrotated = r.captureViewport(none);

    FrameDrawState rotated;
    SceneBacking   br;
    buildTiles(Rotation::Rot90, br, rotated);
    br.sprites = {
        Sprite{.key = "sp0", .x = 16, .y = 16, .atlas = art.atlas, .tile = 1, .palette = art.palette,
               .rotation = Rotation::Rot90},
        Sprite{.key = "sp1", .x = 40, .y = 24, .size = AssetDimensions::GameBoy8x16, .atlas = art.atlas,
               .tile = 0, .palette = art.palette, .rotation = Rotation::Rot270},
    };
    DrawLayer sp{.key = "sprites"};
    sp.z       = 10;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(br.sprites)};
    rotated.layers.push_back(sp);
    const std::vector<Rgba8> withRot = r.captureViewport(rotated);

    ASSERT_EQ(unrotated.size(), withRot.size());
    EXPECT_NE(0, std::memcmp(unrotated.data(), withRot.data(), unrotated.size() * sizeof(Rgba8)))
        << "Rot90 on asymmetric tiles left the composite unchanged — the shader rotation path is dead";

    // Regression: pin the exact rotated composite per backend (a quarter-turn relocation is arithmetic-free).
    runScene("tile_sprite_rotation", Tol::Exact, rotated, r);
}

// Within-layer z-order, proved on the device by equivalence (no golden needed): two overlapping
// opaque sprites submitted BACKWARD (the visually-front one first) with z keys forcing the correct
// stacking must compose byte-identically to the same sprites submitted in visual order with default
// z. Sensitivity is proved in the same test: the backward submission WITHOUT z keys differs.
TEST_F(GoldenReadback, SpriteZOrderEquivalence) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    auto capture = [&](std::vector<Sprite> sprites, SceneBacking& b, FrameDrawState& frame) {
        addBaseScene(frame, art, b);
        b.sprites = std::move(sprites);  // replace the base scene's sprite layer content
        std::get<SpriteContent>(frame.layers[1].content).sprites =
            std::span<const Sprite>(b.sprites);
        return r.captureViewport(frame);
    };

    // Overlapping opaque 8×8 sprites at (20,20) and (24,24). The 4×4 lap is the back sprite's
    // bottom-right quadrant against the front's top-left: tile 0's BR is palette index 2, tile 3's
    // TL is index 0 — different colors, so the stacking order is visible in the lap.
    const Sprite front{.key = "front", .x = 24, .y = 24, .z = 1,
                       .atlas = art.atlas, .tile = 3, .palette = art.palette};
    const Sprite back{.key = "back", .x = 20, .y = 20, .z = 0,
                      .atlas = art.atlas, .tile = 0, .palette = art.palette};

    FrameDrawState fSorted, fKeyed, fBackwards;
    SceneBacking bSorted, bKeyed, bBackwards;
    // Visual order, default z (submission order alone stacks it).
    Sprite backDefault = back, frontDefault = front;
    backDefault.z = frontDefault.z = 0;
    const std::vector<Rgba8> want = capture({backDefault, frontDefault}, bSorted, fSorted);
    // Backward submission, z keys restore the stacking.
    const std::vector<Rgba8> got = capture({front, back}, bKeyed, fKeyed);
    ASSERT_EQ(got.size(), want.size());
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::Exact)) << "z keys must restore the stacking";
    // Teeth: backward submission with default z stacks the other way — must differ.
    const std::vector<Rgba8> wrong = capture({frontDefault, backDefault}, bBackwards, fBackwards);
    EXPECT_NE(0, std::memcmp(wrong.data(), want.data(), want.size() * sizeof(Rgba8)))
        << "overlap scene shows no stacking difference — the equivalence above proved nothing";
}

// The pivot is the spin center, not the placement handle: under an identity transform it drops out of
// the placement entirely. Proved on the device by equivalence — a sprite with pivot (4, 6) at (x, y)
// composes byte-identically to the same sprite with the default pivot at (x, y).
TEST_F(GoldenReadback, SpritePivotIsANoOpUnderIdentity) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    FrameDrawState fDefault, fPivot;
    SceneBacking bDefault, bPivot;
    addBaseScene(fDefault, art, bDefault);
    addBaseScene(fPivot, art, bPivot);
    bPivot.sprites[0].pivot = Point{4.0f, 6.0f};   // spin center only — no position change

    const std::vector<Rgba8> want = r.captureViewport(fDefault);
    const std::vector<Rgba8> got  = r.captureViewport(fPivot);
    ASSERT_EQ(got.size(), want.size());
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::Exact))
        << "a pivot with no transform must not move the sprite";
}

// The origin is the placement handle x/y place: under an identity transform an origin (4, 6) at
// (x+4, y+6) composes byte-identically to the default origin at (x, y) — shifting both cancels.
TEST_F(GoldenReadback, SpriteOriginIsAPureShiftUnderIdentity) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    FrameDrawState fDefault, fOrigin;
    SceneBacking bDefault, bOrigin;
    addBaseScene(fDefault, art, bDefault);
    addBaseScene(fOrigin, art, bOrigin);
    bOrigin.sprites[0].origin = Point{4.0f, 6.0f};
    bOrigin.sprites[0].x += 4;
    bOrigin.sprites[0].y += 6;

    const std::vector<Rgba8> want = r.captureViewport(fDefault);
    const std::vector<Rgba8> got  = r.captureViewport(fOrigin);
    ASSERT_EQ(got.size(), want.size());
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::Exact))
        << "an origin under the identity transform must be a pure placement shift";
}

// The harness has teeth: a 1-pixel scroll of the background changes the captured pixels. Proves the
// readback reflects the composed frame rather than returning a constant — it always runs (no golden
// needed), so the compare path's sensitivity is verified on every platform that has a device.
TEST_F(GoldenReadback, HarnessHasTeeth) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    FrameDrawState f1;
    SceneBacking b1;
    addBaseScene(f1, art, b1);
    const std::vector<Rgba8> a = r.captureViewport(f1);

    FrameDrawState f2;
    SceneBacking b2;
    addBaseScene(f2, art, b2);
    f2.layers[0].scroll = LayerScroll{1, 0};
    const std::vector<Rgba8> shifted = r.captureViewport(f2);

    ASSERT_EQ(a.size(), shifted.size());
    EXPECT_NE(0, std::memcmp(a.data(), shifted.data(), a.size() * sizeof(Rgba8)))
        << "a 1px scroll left the capture unchanged — the readback has no teeth";
}

// ── Crisp parity: every destination-driven path is byte-identical to the viewport-res upscale ────────
//
// These need no committed golden — they compare a scaled compose against the scale-1 compose nearest-
// upscaled, so they run on every platform with a device. One scene per destination-driven path.

// A Mode-7-style homography on the tile layer: the transform evaluates at the snapped (integer) content
// pixel, so the scale-3 compose is the scale-1 image blown up 3×.
TEST_F(GoldenReadback, CrispParityTransformTile) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.layers[0].transform =
        Transform::rotation(15.0f, 32.0f, 32.0f).then(Transform::perspective(0.0f, 0.005f));
    runCrispParity("transform_tile", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionSelect) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::triangle(Point{8, 8}, Point{56, 16}, Point{24, 52});
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{40, 120, 220, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("region_select", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionStroke) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Region reg{.key = "reg"};
    reg.shape             = ShapePoints::circle(Point{32, 32}, 18);
    reg.shape.strokeWidth = 5.0f;  // gate confined to the boundary band (the outline)
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{240, 200, 60, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("region_stroke", frame, r);
}

TEST_F(GoldenReadback, CrispParityRegionStencil) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.regions = stencil(ShapePoints::circle(Point{32, 32}, 14), StencilMode::TransparentInside, 0.0f);
    runCrispParity("region_stencil", frame, r);
}

TEST_F(GoldenReadback, CrispParityCurveRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    Curve c = Curve::quadratic(Vec2{12, 12}, Vec2{52, 8}, Vec2{52, 52});
    c.lineTo(Vec2{12, 52});
    Region reg{.key = "reg"};
    reg.shape   = ShapePoints::fromCurve(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{200, 80, 160, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("curve_region", frame, r);
}

TEST_F(GoldenReadback, CrispParityCurveMaskRegion) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    const std::array<Vec2, 4> pts{{{16, 14}, {50, 20}, {44, 50}, {18, 46}}};
    const Curve c = Curve::throughPoints(std::span<const Vec2>(pts), /*closed=*/true);  // cubic → mask path
    Region reg{.key = "reg"};
    reg.shape   = r.bakeCurveRegion(c);
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = Rgba8{90, 210, 180, 255}}};
    frame.regions.push_back(reg);
    runCrispParity("curve_mask_region", frame, r);
}

TEST_F(GoldenReadback, CrispParityDisplace) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::RowDisplacement,
                                                  .amplitude = 4.0f,
                                                  .frequency = 2.0f,
                                                  .phase     = 0.25f,
                                                  .axis      = Axis::Horizontal,
                                                  .edge      = DisplacementEdge::Blank});
    runCrispParity("displace", frame, r);
}

TEST_F(GoldenReadback, CrispParityRipple) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                                  .amplitude = 3.0f,
                                                  .frequency = 4.0f,
                                                  .phase     = 0.2f,
                                                  .center    = Point{32.0f, 32.0f},
                                                  .decay     = 1.5f});
    runCrispParity("ripple", frame, r);
}

// A custom shader is crisp with ZERO shader change: the generated entry point hands its main() the
// viewport-cell center and sampleSource() quantizes the requested displacement — this scene drives both
// (a spatially-varying sample displacement + procedural tint math) through an unmodified shader file.
TEST_F(GoldenReadback, CrispParityCustomWave) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/wave_probe.frag.hlsl");
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                                  .customShader = probe,
                                                  .wobble       = 0.05f,
                                                  .bands        = 5.0f});
    runCrispParity("custom_wave", frame, r);
}

// The same shader with the displacement off: sampleSource(uv) is the identity request (the fragment
// samples its own true uv), isolating the procedural-math half of the contract.
TEST_F(GoldenReadback, CrispParityCustomProcedural) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId probe = r.registerPostProcessStage("tests/shaders/wave_probe.frag.hlsl");
    FrameDrawState frame;
    SceneBacking b;
    addTileBackground(frame, art, b);
    frame.postEffects.push_back(ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                                  .customShader = probe,
                                                  .wobble       = 0.0f,
                                                  .bands        = 9.0f});
    runCrispParity("custom_procedural", frame, r);
}

// A region Bloom's halo is stored per VIEWPORT pixel and read from the compose grid, so the read is a
// destination-driven path like the rest of this family: on the Viewport grid it moves to the center of the
// cell it landed in, and the filtered tap there returns the stored texel unchanged. That is what makes the
// scaled compose the scale-1 image blown up 3× rather than a softened version of it — the halo has to
// quantize with everything around it or it is the one soft thing in a crisp frame.
namespace {

// A region-Bloom sprite over a tile background: art bright on the left, dark but present on the right, with
// the region covering the left 6 columns. The halo spreads from the bright half into the dark one, so the
// scene carries a real gradient for the read to either quantize or smooth.
struct BloomFieldScene {
    SceneBacking        backing;
    std::vector<Sprite> sprites;
    FrameDrawState      frame;
};

void buildBloomFieldScene(BloomFieldScene& out, Renderer& r, const BaseArt& art) {
    std::array<std::uint8_t, 8 * 8> idx{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            idx[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] = x < 4 ? 1 : 2;
    const AtlasId              sheet = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{{0, 0, 0, 255}, {255, 245, 210, 255}, {24, 20, 16, 255}, {0, 0, 0, 255}}};
    const PaletteId            palId = r.uploadPalette(std::span<const Rgba8>(pal));

    addTileBackground(out.frame, art, out.backing);
    Sprite s{.key = "glow", .x = 20, .y = 20, .atlas = sheet, .tile = 0, .palette = palId};
    s.size    = AssetDimensions{8, 8};
    s.regions = {Region{.key     = "lit",
                        .shape   = ShapePoints::rectangle({0.0f, 0.0f}, 6.0f, 8.0f),
                        .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                                      .radius    = 5.0f,
                                                      .intensity = 255}}}};
    out.sprites.push_back(std::move(s));
    DrawLayer sp{.key = "glow_layer"};
    sp.z       = 50;
    sp.size    = PixelSize{kW, kH};
    sp.content = SpriteContent{.sprites = std::span<const Sprite>(out.sprites)};
    out.frame.layers.push_back(sp);
}

}  // namespace

TEST_F(GoldenReadback, CrispParityRegionBloomField) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt   art = uploadBaseArt(r);
    BloomFieldScene scene;
    buildBloomFieldScene(scene, r, art);
    runCrispParity("region_bloom_field", scene.frame, r);
}

// The other grid, on the same scene: EvaluationGrid::Output must NOT quantize the halo. The scaled compose
// has to differ from the upscale, and it has to differ INSIDE a viewport cell — three neighboring output
// pixels of one cell taking three values is the reconstruction the filtered read exists for, and it is the
// property that separates a filtered read of viewport-resolution data from a point-sampled one.
TEST_F(GoldenReadback, OutputGridRegionBloomFieldReconstructsBetweenTexels) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt   art = uploadBaseArt(r);
    BloomFieldScene scene;
    buildBloomFieldScene(scene, r, art);

    constexpr int scale = 3;
    r.evaluationGrid(EvaluationGrid::Viewport);
    const std::vector<Rgba8> one    = r.captureViewport(scene.frame, 1);
    r.evaluationGrid(EvaluationGrid::Output);
    const std::vector<Rgba8> smooth = r.captureViewport(scene.frame, scale);
    const std::vector<Rgba8> blocky = nearestUpscale(one, kW, kH, scale);
    ASSERT_EQ(smooth.size(), blocky.size());

    EXPECT_FALSE(compareGolden(smooth, blocky, kW * scale, Tol::Exact))
        << "the Output grid reproduced the nearest upscale — the read is still quantizing the halo";

    // Somewhere in the halo, one cell's own output pixels must disagree. Scan the region's dark half, where
    // the spread halo puts its falloff.
    const int  ow      = kW * scale;
    bool       varies  = false;
    for (int vy = 20; vy < 28 && !varies; ++vy) {
        for (int vx = 20; vx < 26 && !varies; ++vx) {
            const std::size_t row = static_cast<std::size_t>(vy * scale + 1) * ow;
            const Rgba8       a   = smooth[row + static_cast<std::size_t>(vx * scale)];
            const Rgba8       c   = smooth[row + static_cast<std::size_t>(vx * scale + scale - 1)];
            const int         sa  = static_cast<int>(a.r) + a.g + a.b;
            const int         sc  = static_cast<int>(c.r) + c.g + c.b;
            if (sa != sc) varies = true;
        }
    }
    EXPECT_TRUE(varies) << "no viewport cell of the halo varied across its own output pixels — the smooth "
                           "read is not reconstructing between stored texels";
}

// ── Below-scope fields on the shared store ───────────────────────────────────────────────────
//
// A below lens's halo is an extract of the SCENE into a rect sized to the lens's own footprint, packed into
// the atlas beside every other field the layer authors. Two properties follow that a single-layer scene
// cannot show, so they are pinned here.

namespace {

// A dark scene with one bright patch, and below-scope Bloom lenses over it. Each lens goes in its OWN layer,
// because the store is rewritten per layer and a stale sheet is only visible across two of them.
struct BelowFieldScene {
    std::vector<TileCell>            cells;
    std::vector<std::vector<Sprite>> sprites;   // one entry per sprite layer, kept alive for its span
    FrameDrawState                   frame;
};

// `lensX` places the lenses; an empty list means no lens at all (the scene alone). Each lens carries two
// Bloom steps at `reachA` and `reachB` — two steps rather than one because a lone field is placed at the
// atlas origin, where an addressing mistake and correct addressing produce the same image, and a clause that
// cannot tell those apart asserts nothing about the rect it reads. Reaches below
// kEmissionDownsampleRadius keep both fields on the full-resolution blur.
void buildBelowFieldScene(BelowFieldScene& out, Renderer& r, std::span<const int> lensX, int lensY,
                          float reachA = 4.0f, float reachB = 7.0f) {
    std::array<std::uint8_t, 8 * 8> solid{};
    const AtlasId              sheet = r.uploadAtlas(solid.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> dark{{{.r = 6, .g = 8, .b = 12, .a = 255}}};
    const std::array<Rgba8, 1> bright{{{.r = 100, .g = 200, .b = 255, .a = 255}}};
    const std::array<Rgba8, 1> mask{{{.r = 255, .g = 255, .b = 255, .a = 255}}};
    const PaletteId darkPal   = r.uploadPalette(std::span<const Rgba8>(dark));
    const PaletteId brightPal = r.uploadPalette(std::span<const Rgba8>(bright));
    const PaletteId maskPal   = r.uploadPalette(std::span<const Rgba8>(mask));

    out.cells.assign(8 * 8, TileCell{.atlas = sheet, .tile = 0, .palette = darkPal});
    DrawLayer bg{.key = "dark_bg"};
    bg.z = 0; bg.size = PixelSize{kW, kH};
    bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                             .cells = std::span<const TileCell>(out.cells)};
    out.frame.layers.push_back(bg);

    // TWO emitters, always both present so every scene here has identical scene content: one under the
    // lens placements near the origin, one under the far placement. The far one is what lets a clause about
    // a far lens assert BRIGHTNESS — over dark scene almost any breakage still reads dark, so a window there
    // cannot tell a correct halo from an absent one.
    out.sprites.reserve(lensX.size() + 1);
    Sprite nearEmitter{.key = "emitter", .x = 28, .y = 28, .atlas = sheet, .tile = 0, .palette = brightPal};
    nearEmitter.size = AssetDimensions{8, 8};
    Sprite farEmitter{.key = "emitter_far", .x = 50, .y = 50, .atlas = sheet, .tile = 0, .palette = brightPal};
    farEmitter.size = AssetDimensions{8, 8};
    out.sprites.push_back({nearEmitter, farEmitter});
    DrawLayer el{.key = "emitter_layer"};
    el.z = 10; el.size = PixelSize{kW, kH};
    el.content = SpriteContent{.sprites = std::span<const Sprite>(out.sprites.back())};
    out.frame.layers.push_back(el);

    for (std::size_t i = 0; i < lensX.size(); ++i) {
        Sprite l{.key   = "lens" + std::to_string(i), .x = lensX[i], .y = lensY,
                 .atlas = sheet, .tile = 0, .palette = maskPal};
        l.size    = AssetDimensions{16, 16};
        l.effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                       .scope     = ScreenSpaceEffectScope::Below,
                                       .radius    = reachA, .threshold = 26, .intensity = 255},
                     ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                       .scope     = ScreenSpaceEffectScope::Below,
                                       .radius    = reachB, .threshold = 26, .intensity = 255}};
        out.sprites.push_back({l});
        DrawLayer ll{.key = "lens_layer" + std::to_string(i)};
        ll.z = 50 + static_cast<int>(i); ll.size = PixelSize{kW, kH};
        ll.content = SpriteContent{.sprites = std::span<const Sprite>(out.sprites.back())};
        out.frame.layers.push_back(ll);
    }
}

}  // namespace

// A layer's fields are its own. The store is rewritten per layer and the rect table spans the whole frame,
// so a second layer's rows sit after the first's — and a lens that read the wrong rows would read another
// layer's rects. A single-layer scene cannot catch that, hence two.
//
// Note what does NOT protect this: the per-sheet clear. A below field's extract fills its whole rect, margin
// ring included, so nothing stale can survive inside one. (The clear is load-bearing for the REGION path,
// whose raster covers only the quad and leaves the ring to it.) What this clause pins is the row addressing.
TEST_F(GoldenReadback, SpriteBelowFieldsAreTheirOwnLayers) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    // The far lens alone, and the same far lens after a near one has already used the store.
    constexpr std::array<int, 1> aloneX{{44}};
    constexpr std::array<int, 2> pairX{{18, 44}};
    BelowFieldScene alone;
    buildBelowFieldScene(alone, r, std::span<const int>(aloneX), 44);
    BelowFieldScene pair;
    buildBelowFieldScene(pair, r, std::span<const int>(pairX), 44);

    const std::vector<Rgba8> one = r.captureViewport(alone.frame, 1);
    const std::vector<Rgba8> two = r.captureViewport(pair.frame, 1);
    ASSERT_EQ(one.size(), two.size());

    // Inside the far lens and outside its emitter's own art (which starts at 50), so the window carries the
    // far halo and nothing else: its pixels must not depend on what the layer before it put in the store,
    // nor on where that layer's rows sat in the shared table.
    int worst = 0, lit = 0;
    for (int y = 45; y < 50; ++y)
        for (int x = 45; x < 50; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x);
            worst = std::max({worst, std::abs(static_cast<int>(one[i].r) - two[i].r),
                              std::abs(static_cast<int>(one[i].g) - two[i].g),
                              std::abs(static_cast<int>(one[i].b) - two[i].b)});
            if (static_cast<int>(one[i].r) + one[i].g + one[i].b > 40) ++lit;
        }
    ASSERT_GT(lit, 8) << "too little halo under the far lens — the clause is asserting nothing";
    EXPECT_EQ(worst, 0) << "the far lens changed by " << worst
                        << " once another layer had used the store — its fields are not its own";
}

// A below halo is SCENE-anchored: a field texel holds the scene at a position, so where the rect's own
// boundary fell cannot matter. Move the lens a whole pixel and the halo over a point both placements cover
// stays put — if the extract wrote its content relative to the rect rather than to the scene, it would drag
// along with the lens.
TEST_F(GoldenReadback, SpriteBelowHaloIsSceneAnchoredNotLensAnchored) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    constexpr std::array<int, 1> atA{{20}};
    constexpr std::array<int, 1> atB{{21}};
    BelowFieldScene a;
    buildBelowFieldScene(a, r, std::span<const int>(atA), 20);
    BelowFieldScene b;
    buildBelowFieldScene(b, r, std::span<const int>(atB), 20);

    const std::vector<Rgba8> ia = r.captureViewport(a.frame, 1);
    const std::vector<Rgba8> ib = r.captureViewport(b.frame, 1);
    ASSERT_EQ(ia.size(), ib.size());

    // (22,22)–(27,27) is inside both silhouettes — [20,36) and [21,37) — and OUTSIDE the emitter itself,
    // which starts at 28. That matters: over the emitter the window would be dominated by the emitter's own
    // bright pixels, and a halo that shifted underneath them would still read as unchanged. Here the only
    // light is the halo's falloff, so a shift shows.
    int worst = 0, lit = 0;
    for (int y = 22; y < 28; ++y)
        for (int x = 22; x < 28; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x);
            worst = std::max({worst, std::abs(static_cast<int>(ia[i].r) - ib[i].r),
                              std::abs(static_cast<int>(ia[i].g) - ib[i].g),
                              std::abs(static_cast<int>(ia[i].b) - ib[i].b)});
            // The dark background sums to 26; anything brighter here is halo, since no art covers it.
            if (static_cast<int>(ia[i].r) + ia[i].g + ia[i].b > 40) ++lit;
        }
    ASSERT_GT(lit, 8) << "too little halo in the sampled window — the clause is asserting nothing";
    EXPECT_EQ(worst, 0) << "the halo moved by " << worst
                        << " when the lens moved one pixel — it is anchored to the rect, not the scene";
}

// Both paths in ONE layer: the region demands and the below demands are planned together into one index
// space, region rows first and below rows after. If the
// below half seated from the wrong base — or the two halves were allowed to overlap — each path would read
// the other's rect. Neither a region-only nor a below-only scene can see that, so it is pinned here by
// rendering the pair together and requiring each to match itself rendered alone.
TEST_F(GoldenReadback, RegionAndBelowFieldsShareOneIndexSpace) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    std::array<std::uint8_t, 8 * 8> solid{};
    const AtlasId              sheet = r.uploadAtlas(solid.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> dark{{{.r = 6, .g = 8, .b = 12, .a = 255}}};
    const std::array<Rgba8, 1> bright{{{.r = 100, .g = 200, .b = 255, .a = 255}}};
    const std::array<Rgba8, 1> mask{{{.r = 255, .g = 255, .b = 255, .a = 255}}};
    const PaletteId darkPal   = r.uploadPalette(std::span<const Rgba8>(dark));
    const PaletteId brightPal = r.uploadPalette(std::span<const Rgba8>(bright));
    const PaletteId maskPal   = r.uploadPalette(std::span<const Rgba8>(mask));
    const std::vector<TileCell> cells(8 * 8, TileCell{.atlas = sheet, .tile = 0, .palette = darkPal});

    // `withRegion` / `withLens` select which of the two the mixed layer carries.
    struct Scene { std::vector<Sprite> emitters, mixed; FrameDrawState frame; };
    auto build = [&](Scene& sc, bool withRegion, bool withLens) {
        DrawLayer bg{.key = "bg"};
        bg.z = 0; bg.size = PixelSize{kW, kH};
        bg.content = TileContent{.widthInTiles = 8, .heightInTiles = 8,
                                 .cells = std::span<const TileCell>(cells)};
        sc.frame.layers.push_back(bg);

        Sprite em{.key = "em", .x = 46, .y = 46, .atlas = sheet, .tile = 0, .palette = brightPal};
        em.size = AssetDimensions{8, 8};
        sc.emitters = {em};
        DrawLayer el{.key = "emitters"};
        el.z = 10; el.size = PixelSize{kW, kH};
        el.content = SpriteContent{.sprites = std::span<const Sprite>(sc.emitters)};
        sc.frame.layers.push_back(el);

        if (withRegion) {   // its own light, graded as a source under the region's blend
            Sprite g{.key = "grader", .x = 8, .y = 8, .atlas = sheet, .tile = 0, .palette = brightPal};
            g.size    = AssetDimensions{8, 8};
            g.regions = {Region{.key     = "lit",
                                .shape   = ShapePoints::rectangle({0.0f, 0.0f}, 6.0f, 8.0f),
                                .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                                              .radius    = 5.0f, .intensity = 255}}}};
            sc.mixed.push_back(g);
        }
        if (withLens) {     // the scene's light, read through a silhouette
            Sprite l{.key = "lens", .x = 40, .y = 40, .atlas = sheet, .tile = 0, .palette = maskPal};
            l.size    = AssetDimensions{16, 16};
            l.effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                           .scope     = ScreenSpaceEffectScope::Below,
                                           .radius    = 4.0f, .threshold = 26, .intensity = 255},
                         ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                           .scope     = ScreenSpaceEffectScope::Below,
                                           .radius    = 7.0f, .threshold = 26, .intensity = 255}};
            sc.mixed.push_back(l);
        }
        DrawLayer ml{.key = "mixed"};
        ml.z = 50; ml.size = PixelSize{kW, kH};
        ml.content = SpriteContent{.sprites = std::span<const Sprite>(sc.mixed)};
        sc.frame.layers.push_back(ml);
    };

    Scene both, regionOnly, lensOnly;
    build(both, true, true);
    build(regionOnly, true, false);
    build(lensOnly, false, true);

    const std::vector<Rgba8> ib = r.captureViewport(both.frame, 1);
    const std::vector<Rgba8> ir = r.captureViewport(regionOnly.frame, 1);
    const std::vector<Rgba8> il = r.captureViewport(lensOnly.frame, 1);

    auto worstOver = [](const std::vector<Rgba8>& a, const std::vector<Rgba8>& b, int x0, int x1, int y0,
                        int y1, int& lit) {
        int worst = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x);
                worst = std::max({worst, std::abs(static_cast<int>(a[i].r) - b[i].r),
                                  std::abs(static_cast<int>(a[i].g) - b[i].g),
                                  std::abs(static_cast<int>(a[i].b) - b[i].b)});
                if (static_cast<int>(a[i].r) + a[i].g + a[i].b > 40) ++lit;
            }
        return worst;
    };

    int litR = 0, litL = 0;
    const int wRegion = worstOver(ib, ir, 8, 16, 8, 16, litR);      // the graded sprite's own 8x8
    const int wLens   = worstOver(ib, il, 41, 46, 41, 46, litL);    // inside the lens, outside its emitter

    ASSERT_GT(litR, 8) << "the region sprite is not lit — the clause is asserting nothing";
    ASSERT_GT(litL, 8) << "no halo under the lens — the clause is asserting nothing";
    EXPECT_EQ(wRegion, 0) << "the region Bloom changed by " << wRegion
                          << " when a below lens shared its layer — the two are reading one another's rects";
    EXPECT_EQ(wLens, 0) << "the below lens changed by " << wLens
                        << " when a region Bloom shared its layer — the two are reading one another's rects";
}

// A reach at or above kEmissionDownsampleRadius blurs at quarter resolution, and THAT lattice is anchored to
// the rect — so a lens moving by a non-multiple of the reduction factor re-quantizes its own halo slightly.
// The content stays scene-anchored (the clause above proves that exactly, on the full-resolution path); what
// moves here is only which quarter-resolution texels the same content falls in. The shift is small and
// bounded, and this clause is what says how small.
TEST_F(GoldenReadback, SpriteBelowReducedHaloRequantizesWithItsRect) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    constexpr std::array<int, 1> atA{{20}};
    constexpr std::array<int, 1> atB{{21}};
    BelowFieldScene a;
    buildBelowFieldScene(a, r, std::span<const int>(atA), 20, 12.0f, 14.0f);
    BelowFieldScene b;
    buildBelowFieldScene(b, r, std::span<const int>(atB), 20, 12.0f, 14.0f);

    const std::vector<Rgba8> ia = r.captureViewport(a.frame, 1);
    const std::vector<Rgba8> ib = r.captureViewport(b.frame, 1);
    ASSERT_EQ(ia.size(), ib.size());

    int worst = 0;
    for (int y = 23; y < 35; ++y)
        for (int x = 23; x < 35; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x);
            worst = std::max({worst, std::abs(static_cast<int>(ia[i].r) - ib[i].r),
                              std::abs(static_cast<int>(ia[i].g) - ib[i].g),
                              std::abs(static_cast<int>(ia[i].b) - ib[i].b)});
        }
    // Small — the same family as the arc's other reduced-path differences — and bounded, so a real drag
    // (which moves the halo wholesale) still fails this.
    EXPECT_LE(worst, 6) << "the reduced halo moved by " << worst
                        << " for a one-pixel lens move — too much to be the quarter-resolution lattice";
}

// The crisp invariant on this path. Every committed image golden runs at composeScale 1, where snapped and
// continuous coincide and a filtered tap at a texel anchor is trivially the stored texel — so nothing else
// in the suite exercises the below read at a scale where the snap does anything. Here it does: the scaled
// compose must be the scale-1 image blown up exactly, halo included.
TEST_F(GoldenReadback, CrispParityBelowLensField) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    constexpr std::array<int, 1> lensX{{20}};
    BelowFieldScene scene;
    buildBelowFieldScene(scene, r, std::span<const int>(lensX), 20);
    runCrispParity("below_lens_field", scene.frame, r);
}

// The read on this path is destination-driven like the region one: EvaluationGrid::Output must reconstruct
// the halo between stored texels rather than quantizing it to the viewport grid.
TEST_F(GoldenReadback, OutputGridBelowLensReconstructsBetweenTexels) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};

    constexpr std::array<int, 1> lensX{{20}};
    BelowFieldScene scene;
    buildBelowFieldScene(scene, r, std::span<const int>(lensX), 20);

    constexpr int scale = 3;
    r.evaluationGrid(EvaluationGrid::Viewport);
    const std::vector<Rgba8> one    = r.captureViewport(scene.frame, 1);
    r.evaluationGrid(EvaluationGrid::Output);
    const std::vector<Rgba8> smooth = r.captureViewport(scene.frame, scale);
    const std::vector<Rgba8> blocky = nearestUpscale(one, kW, kH, scale);
    ASSERT_EQ(smooth.size(), blocky.size());

    EXPECT_FALSE(compareGolden(smooth, blocky, kW * scale, Tol::Exact))
        << "the Output grid reproduced the nearest upscale — the below read is still quantizing the halo";

    const int ow     = kW * scale;
    bool      varies = false;
    for (int vy = 23; vy < 33 && !varies; ++vy) {
        for (int vx = 23; vx < 33 && !varies; ++vx) {
            const std::size_t row = static_cast<std::size_t>(vy * scale + 1) * ow;
            const Rgba8       p0  = smooth[row + static_cast<std::size_t>(vx * scale)];
            const Rgba8       p2  = smooth[row + static_cast<std::size_t>(vx * scale + scale - 1)];
            if (static_cast<int>(p0.r) + p0.g + p0.b != static_cast<int>(p2.r) + p2.g + p2.b) varies = true;
        }
    }
    EXPECT_TRUE(varies) << "no viewport cell of the below halo varied across its own output pixels — the "
                           "smooth read is not reconstructing between stored texels";
}

// ── Crisp parity: transformed sprites resolve coverage on the viewport grid ──────────────────
//
// A tile background + one sprite layer of geometrically-transformed sprites, whose analytic (Viewport-
// grid) coverage must upscale to an exact nearest-multiple of the scale-1 image. The base art is fully
// opaque, so parity is a clean coverage + color test: every viewport cell inside the true quad writes
// its texel, every cell outside discards.

void addTransformedSpriteScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& b,
                               const Transform& layer, const Transform& spriteXf) {
    addTileBackground(frame, art, b);
    b.sprites = {
        Sprite{.key = "sp0", .x = 24, .y = 20, .size = AssetDimensions{8, 8}, .atlas = art.atlas,
               .tile = 1, .palette = art.palette, .transform = spriteXf},
        Sprite{.key = "sp1", .x = 40, .y = 36, .size = AssetDimensions{8, 8}, .atlas = art.atlas,
               .tile = 3, .palette = art.palette, .transform = spriteXf},
    };
    DrawLayer sp{.key = "sprites"};
    sp.z        = 10;
    sp.size     = PixelSize{kW, kH};
    sp.content  = SpriteContent{.sprites = std::span<const Sprite>(b.sprites)};
    sp.transform = layer;
    frame.layers.push_back(sp);
}

TEST_F(GoldenReadback, CrispParitySpriteRotation) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTransformedSpriteScene(frame, art, b, Transform{}, Transform::rotation(30.0f, 4.0f, 4.0f));
    runCrispParity("sprite_rotation", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteScale) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    addTransformedSpriteScene(frame, art, b, Transform{}, Transform::scale(2.5f, 1.75f, 4.0f, 4.0f));
    runCrispParity("sprite_scale", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpritePerspective) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    const Transform xf =
        Transform::rotation(18.0f, 4.0f, 4.0f).then(Transform::perspective(0.01f, 0.006f));
    addTransformedSpriteScene(frame, art, b, Transform{}, xf);
    runCrispParity("sprite_perspective", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteLayerTransform) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    // A per-layer transform composed over sprites that carry their OWN transforms — coverage of both.
    const Transform layer =
        Transform::rotation(12.0f, 32.0f, 32.0f).then(Transform::scale(1.3f, 1.1f, 32.0f, 32.0f));
    addTransformedSpriteScene(frame, art, b, layer, Transform::rotation(25.0f, 4.0f, 4.0f));
    runCrispParity("sprite_layer_transform", frame, r);
}

TEST_F(GoldenReadback, CrispParitySpriteFractional) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking b;
    // Sub-pixel placement via a fractional translation baked into the transform. Placement stays
    // sub-pixel; parity holds because both captures use the identical fractional transform.
    const Transform xf =
        Transform::rotation(15.0f, 4.0f, 4.0f).then(Transform::translation(0.5f, 0.25f));
    addTransformedSpriteScene(frame, art, b, Transform{}, xf);
    runCrispParity("sprite_fractional", frame, r);
}

// ── Region batching: batched-vs-per-region equivalence ──────────────────────────────────────────
//
// The strong proof that the instanced-additive fast path matches the per-region path. A DECLARED additive
// shader (additive_glow — compiled with its batched variant → the batched route) and its PLAIN twin
// (additive_glow_plain — identical body, no declaration → the per-region route) render the SAME multi-
// circle scene at each confined site; the readbacks must match within Tol::OneStep (additive order / float
// rounding only). No committed goldens — the two live captures ARE each other's reference. Overlaps + an
// ineligible Multiply-blend splitter exercise additive accumulation and run-boundary preservation.

enum class BatchSite { Frame, BelowLayer, IsolatedLayer };

// Build the full scene for `site` + stage `h` into caller-owned frame/backing (both must outlive the
// capture). The base composite (tile bg + two sprites) plus additive glow circles: overlapping (additive
// accumulation in the lap), and — when `splitter` — an ineligible Multiply-blend region mid-list that
// breaks the batched run into two (the hard boundary the per-region twin also honors). For the layer
// sites the glows ride a content-less fx layer at z 5 (between the bg at 0 and the sprites at 10).
void buildGlowScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& base,
                    BatchSite site, PostProcessStageId h, bool splitter) {
    addBaseScene(frame, art, base);
    const ScreenSpaceEffectScope scope =
        (site == BatchSite::BelowLayer) ? ScreenSpaceEffectScope::Below : ScreenSpaceEffectScope::Layer;
    std::vector<Region> local;
    std::vector<Region>& sink = (site == BatchSite::Frame) ? frame.regions : local;
    auto glow = [&](const char* key, float cx, float cy, float rad, float gain, BlendMode blend) {
        Region reg{.key = key};
        reg.shape   = ShapePoints::circle(Point{cx, cy}, rad);
        reg.blend   = blend;
        reg.effects = {ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                         .customShader = h,
                                         .scope        = scope,
                                         .addGain      = gain}};
        sink.push_back(std::move(reg));
    };
    glow("g0", 22, 26, 14, 0.6f, BlendMode::Normal);
    glow("g1", 34, 32, 14, 0.6f, BlendMode::Normal);   // overlaps g0 — additive accumulation in the lap
    if (splitter)
        glow("gx", 30, 40, 10, 0.5f, BlendMode::Multiply);  // ineligible → splits the run (hard boundary)
    glow("g2", 40, 44, 12, 0.6f, BlendMode::Normal);
    glow("g3", 18, 46, 11, 0.6f, BlendMode::Normal);
    if (site != BatchSite::Frame) {
        DrawLayer fx{.key = "fx"};
        fx.z       = 5;
        fx.size    = PixelSize{kW, kH};
        fx.regions = std::move(local);  // Region owns its data → the layer's copy is self-contained
        frame.layers.push_back(std::move(fx));
    }
}

// Build a MIXED-composition scene: TWO different additive effects (same stage, different addGain → distinct
// (stage, params) keys → two interleaved runs A₁ B₁ A₂ B₂) with an ineligible region sandwiched mid-stretch.
// Proves grouping keeps both compositions on the fast path AND preserves order across the boundary.
void buildMixedGlowScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& base,
                         PostProcessStageId h) {
    addBaseScene(frame, art, base);
    auto glow = [&](const char* key, float cx, float cy, float rad, float gain, BlendMode blend) {
        Region reg{.key = key};
        reg.shape   = ShapePoints::circle(Point{cx, cy}, rad);
        reg.blend   = blend;
        reg.effects = {ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                         .customShader = h,
                                         .addGain      = gain}};
        frame.regions.push_back(std::move(reg));
    };
    glow("a0", 20, 24, 13, 0.6f, BlendMode::Normal);   // group A (gain 0.6)
    glow("b0", 40, 28, 13, 0.3f, BlendMode::Normal);   // group B (gain 0.3) — interleaved with A
    glow("mx", 30, 36, 10, 0.5f, BlendMode::Multiply);  // ineligible sandwich → hard boundary
    glow("a1", 24, 46, 12, 0.6f, BlendMode::Normal);   // group A again (after the boundary)
    glow("b1", 44, 46, 12, 0.3f, BlendMode::Normal);   // group B again
}

void runBatchEquivalence(const std::string& name, BatchSite site, bool splitter, Renderer& r,
                         const BaseArt& art, PostProcessStageId declared, PostProcessStageId plain) {
    FrameDrawState fBatched, fPlain;
    SceneBacking   bBatched, bPlain;
    buildGlowScene(fBatched, art, bBatched, site, declared, splitter);
    buildGlowScene(fPlain,   art, bPlain,   site, plain,    splitter);
    const std::vector<Rgba8> got  = r.captureViewport(fBatched);
    const std::vector<Rgba8> want = r.captureViewport(fPlain);
    ASSERT_EQ(got.size(), want.size()) << name;
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::OneStep)) << name << " — batched vs per-region";
}

TEST_F(GoldenReadback, RegionBatchEquivFrame) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId declared = r.registerPostProcessStage("tests/shaders/additive_glow.frag.hlsl");
    const PostProcessStageId plain    = r.registerPostProcessStage("tests/shaders/additive_glow_plain.frag.hlsl");
    runBatchEquivalence("batch_equiv_frame", BatchSite::Frame, /*splitter=*/true, r, art, declared, plain);
}

TEST_F(GoldenReadback, RegionBatchEquivBelow) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId declared = r.registerPostProcessStage("tests/shaders/additive_glow.frag.hlsl");
    const PostProcessStageId plain    = r.registerPostProcessStage("tests/shaders/additive_glow_plain.frag.hlsl");
    runBatchEquivalence("batch_equiv_below", BatchSite::BelowLayer, /*splitter=*/true, r, art, declared, plain);
}

TEST_F(GoldenReadback, RegionBatchEquivLayer) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId declared = r.registerPostProcessStage("tests/shaders/additive_glow.frag.hlsl");
    const PostProcessStageId plain    = r.registerPostProcessStage("tests/shaders/additive_glow_plain.frag.hlsl");
    // The isolated-Layer site exercises the run-tail premultiplied-over composite (all steps are runs → the
    // last unit is a run, so the finish is the empty-region blend composite, not a per-step composite).
    runBatchEquivalence("batch_equiv_layer", BatchSite::IsolatedLayer, /*splitter=*/false, r, art, declared, plain);
}

TEST_F(GoldenReadback, RegionBatchEquivMixed) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId declared = r.registerPostProcessStage("tests/shaders/additive_glow.frag.hlsl");
    const PostProcessStageId plain    = r.registerPostProcessStage("tests/shaders/additive_glow_plain.frag.hlsl");
    FrameDrawState fBatched, fPlain;
    SceneBacking   bBatched, bPlain;
    buildMixedGlowScene(fBatched, art, bBatched, declared);
    buildMixedGlowScene(fPlain,   art, bPlain,   plain);
    const std::vector<Rgba8> got  = r.captureViewport(fBatched);
    const std::vector<Rgba8> want = r.captureViewport(fPlain);
    ASSERT_EQ(got.size(), want.size());
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::OneStep)) << "batch_equiv_mixed — batched vs per-region";
}

// The one COMMITTED golden for the batched path: a Below-site batch of overlapping additive circles. Its
// bytes are captured per backend (Metal locally; Vulkan x64/arm64 + D3D12 x64 via the capture dispatch),
// pinning the batched additive output so a future regression in the fast path is caught even if the
// equivalence twins drifted together.
TEST_F(GoldenReadback, RegionBatchBelowGolden) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId declared = r.registerPostProcessStage("tests/shaders/additive_glow.frag.hlsl");
    FrameDrawState frame;
    SceneBacking   base;
    buildGlowScene(frame, art, base, BatchSite::BelowLayer, declared, /*splitter=*/false);
    runScene("region_batch_below", Tol::OneStep, frame, r);
}

// ── Region gathering: gather-vs-per-region equivalence ───────────────────────────────────
//
// The strong proof that the union-shape gather pass matches the per-region path for SOURCE-DEPENDENT
// (replace) effects — the class additive batching cannot touch. A GATHERING shader (gather_warp — no declaration → its
// GATHER variant is compiled → the gather route) and its no-gather twin (gather_warp_nogather — identical
// displacement+tint body, `// @retropp:no-gather` → the per-region route) render the SAME multi-circle scene
// at each confined site over WELL-SEPARATED shapes (no overlap → gather's last-wins == sequential); the
// readbacks must match within Tol::OneStep. Distinct per-region tint proves params ride the records; an
// ineligible Multiply-blend splitter exercises run-boundary preservation.

enum class GatherSite { Frame, BelowLayer, IsolatedLayer };

void buildWarpScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& base,
                    GatherSite site, PostProcessStageId h, bool splitter) {
    addBaseScene(frame, art, base);
    const ScreenSpaceEffectScope scope =
        (site == GatherSite::BelowLayer) ? ScreenSpaceEffectScope::Below : ScreenSpaceEffectScope::Layer;
    std::vector<Region> local;
    std::vector<Region>& sink = (site == GatherSite::Frame) ? frame.regions : local;
    auto warp = [&](const char* key, float cx, float cy, float rad, float amp, Vec3 tint, BlendMode blend) {
        Region reg{.key = key};
        reg.shape   = ShapePoints::circle(Point{cx, cy}, rad);
        reg.blend   = blend;
        reg.effects = {ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                         .customShader = h,
                                         .scope        = scope,
                                         .gwarpAmp     = amp,
                                         .gwarpFreq    = 18.0f,
                                         .gwarpTint    = tint}};
        sink.push_back(std::move(reg));
    };
    // WELL-SEPARATED circles (no gate overlap → gather's last-wins equals the sequential per-region result),
    // distinct tint + amp per region.
    warp("w0", 16, 16, 8, 0.020f, Vec3{0.15f, 0.00f, 0.00f}, BlendMode::Normal);
    warp("w1", 48, 16, 8, 0.030f, Vec3{0.00f, 0.15f, 0.00f}, BlendMode::Normal);   // gathers with w0
    if (splitter)
        warp("wx", 32, 32, 6, 0.020f, Vec3{0.00f, 0.00f, 0.15f}, BlendMode::Multiply);  // ineligible → splits
    warp("w2", 16, 48, 8, 0.025f, Vec3{0.00f, 0.00f, 0.15f}, BlendMode::Normal);
    warp("w3", 48, 48, 8, 0.020f, Vec3{0.12f, 0.12f, 0.00f}, BlendMode::Normal);   // gathers with w2
    if (site != GatherSite::Frame) {
        DrawLayer fx{.key = "fx"};
        fx.z       = 5;
        fx.size    = PixelSize{kW, kH};
        fx.regions = std::move(local);  // Region owns its data → the layer's copy is self-contained
        frame.layers.push_back(std::move(fx));
    }
}

// A MIXED-composition scene: two DIFFERENT gather stages (h1, h2 — two registrations of the gathering shader)
// with an ineligible Multiply splitter and a partial-alpha ineligible tail. Proves run boundaries (a stage
// change AND an ineligible step both end a contiguous run) + order preservation across them.
void buildMixedWarpScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& base,
                         PostProcessStageId h1, PostProcessStageId h2) {
    addBaseScene(frame, art, base);
    auto warp = [&](const char* key, float cx, float cy, float rad, float amp, Vec3 tint, BlendMode blend,
                    float alpha, PostProcessStageId h) {
        Region reg{.key = key};
        reg.shape   = ShapePoints::circle(Point{cx, cy}, rad);
        reg.blend   = blend;
        reg.alpha   = alpha;
        reg.effects = {ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                         .customShader = h,
                                         .gwarpAmp     = amp,
                                         .gwarpFreq    = 18.0f,
                                         .gwarpTint    = tint}};
        frame.regions.push_back(std::move(reg));
    };
    warp("m0", 14, 14, 7, 0.020f, Vec3{0.15f, 0, 0},     BlendMode::Normal,   1.0f, h1);  // h1 run start
    warp("m1", 32, 14, 7, 0.030f, Vec3{0, 0.15f, 0},     BlendMode::Normal,   1.0f, h1);  // h1 → gathers w/ m0
    warp("mx", 50, 14, 6, 0.020f, Vec3{0, 0, 0.15f},     BlendMode::Multiply, 1.0f, h1);  // ineligible → boundary
    warp("m2", 16, 40, 7, 0.020f, Vec3{0.12f, 0.12f, 0}, BlendMode::Normal,   1.0f, h2);  // h2 → different stage
    warp("m3", 40, 40, 7, 0.030f, Vec3{0, 0.12f, 0.12f}, BlendMode::Normal,   1.0f, h2);  // h2 → gathers w/ m2
    warp("ma", 32, 56, 6, 0.020f, Vec3{0.1f, 0, 0.1f},   BlendMode::Normal,   0.5f, h1);  // partial alpha → solo
}

void runGatherEquivalence(const std::string& name, GatherSite site, bool splitter, Renderer& r,
                          const BaseArt& art, PostProcessStageId gathers, PostProcessStageId perRegion) {
    FrameDrawState fG, fP;
    SceneBacking   bG, bP;
    buildWarpScene(fG, art, bG, site, gathers,   splitter);
    buildWarpScene(fP, art, bP, site, perRegion, splitter);
    const std::vector<Rgba8> got  = r.captureViewport(fG);
    const std::vector<Rgba8> want = r.captureViewport(fP);
    ASSERT_EQ(got.size(), want.size()) << name;
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::OneStep)) << name << " — gather vs per-region";
}

TEST_F(GoldenReadback, RegionGatherEquivFrame) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId gathers   = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    const PostProcessStageId perRegion = r.registerPostProcessStage("tests/shaders/gather_warp_nogather.frag.hlsl");
    runGatherEquivalence("gather_equiv_frame", GatherSite::Frame, /*splitter=*/true, r, art, gathers, perRegion);
}

TEST_F(GoldenReadback, RegionGatherEquivBelow) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId gathers   = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    const PostProcessStageId perRegion = r.registerPostProcessStage("tests/shaders/gather_warp_nogather.frag.hlsl");
    runGatherEquivalence("gather_equiv_below", GatherSite::BelowLayer, /*splitter=*/true, r, art, gathers, perRegion);
}

TEST_F(GoldenReadback, RegionGatherEquivLayer) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId gathers   = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    const PostProcessStageId perRegion = r.registerPostProcessStage("tests/shaders/gather_warp_nogather.frag.hlsl");
    // The isolated-Layer site exercises the run-as-last-step premultiplied-over composite onto target_ (the
    // gather branch's blend pipeline, not the additive-path post-loop composite). No splitter → a run IS the last step.
    runGatherEquivalence("gather_equiv_layer", GatherSite::IsolatedLayer, /*splitter=*/false, r, art, gathers, perRegion);
}

TEST_F(GoldenReadback, RegionGatherEquivMixed) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    // Two registrations of each twin → two distinct stages, so the interleave crosses a stage boundary.
    const PostProcessStageId g1 = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    const PostProcessStageId g2 = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    const PostProcessStageId p1 = r.registerPostProcessStage("tests/shaders/gather_warp_nogather.frag.hlsl");
    const PostProcessStageId p2 = r.registerPostProcessStage("tests/shaders/gather_warp_nogather.frag.hlsl");
    FrameDrawState fG, fP;
    SceneBacking   bG, bP;
    buildMixedWarpScene(fG, art, bG, g1, g2);
    buildMixedWarpScene(fP, art, bP, p1, p2);
    const std::vector<Rgba8> got  = r.captureViewport(fG);
    const std::vector<Rgba8> want = r.captureViewport(fP);
    ASSERT_EQ(got.size(), want.size());
    EXPECT_TRUE(compareGolden(got, want, kW, Tol::OneStep)) << "gather_equiv_mixed — gather vs per-region";
}

// Two overlapping circles with distinct per-region tint — the LAST-region-wins semantics.
void buildOverlapScene(FrameDrawState& frame, const BaseArt& art, SceneBacking& base,
                       PostProcessStageId h, bool onlyLast) {
    addBaseScene(frame, art, base);
    auto warp = [&](const char* key, float cx, float cy, float rad, Vec3 tint) {
        Region reg{.key = key};
        reg.shape   = ShapePoints::circle(Point{cx, cy}, rad);
        reg.effects = {ScreenSpaceEffect{.kind         = ScreenSpaceEffectKind::Custom,
                                         .customShader = h,
                                         .gwarpAmp     = 0.020f,
                                         .gwarpFreq    = 18.0f,
                                         .gwarpTint    = tint}};
        frame.regions.push_back(std::move(reg));
    };
    if (!onlyLast) warp("o0", 26, 32, 14, Vec3{0.30f, 0.00f, 0.00f});  // first (red) — overlaps o1
    warp("o1", 40, 32, 14, Vec3{0.00f, 0.00f, 0.30f});                 // last (blue) — wins the lap
}

// The one COMMITTED golden for the gather path: overlapping circles with per-region tint, pinning the
// last-wins output. Captured per backend (Metal locally; Vulkan x64/arm64 + D3D12 x64 via the capture
// dispatch), so a future regression is caught even if the equivalence twins drifted together.
TEST_F(GoldenReadback, RegionGatherOverlapGolden) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId h = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    FrameDrawState frame;
    SceneBacking   base;
    buildOverlapScene(frame, art, base, h, /*onlyLast=*/false);
    runScene("region_gather_overlap", Tol::OneStep, frame, r);
}

// Last-wins, machine-checked without a CPU mirror: a lap pixel of the two overlapping circles must equal the
// SAME scene rendered with ONLY the last region (the single-region render IS the expectation). Teeth: a pixel
// inside only the FIRST circle must DIFFER (there the two scenes genuinely diverge — first-effect vs source).
TEST_F(GoldenReadback, RegionGatherLastWins) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    const PostProcessStageId h = r.registerPostProcessStage("tests/shaders/gather_warp.frag.hlsl");
    FrameDrawState fBoth, fLast;
    SceneBacking   bBoth, bLast;
    buildOverlapScene(fBoth, art, bBoth, h, /*onlyLast=*/false);  // both circles, gather → last wins in the lap
    buildOverlapScene(fLast, art, bLast, h, /*onlyLast=*/true);   // only the last (blue) circle
    const std::vector<Rgba8> both = r.captureViewport(fBoth);
    const std::vector<Rgba8> last = r.captureViewport(fLast);
    ASSERT_EQ(both.size(), last.size());
    auto near = [](const Rgba8& a, const Rgba8& b) {
        return std::abs(int(a.r) - int(b.r)) <= 1 && std::abs(int(a.g) - int(b.g)) <= 1 &&
               std::abs(int(a.b) - int(b.b)) <= 1 && std::abs(int(a.a) - int(b.a)) <= 1;
    };
    // (36,32): inside BOTH circles (d(o0)≈10, d(o1)≈4 < 14). Last-wins ⇒ the blue region's output, which is
    // exactly what the only-last render produces there.
    const std::size_t lap = static_cast<std::size_t>(32) * kW + 36;
    EXPECT_TRUE(near(both[lap], last[lap])) << "lap pixel must equal the last-region-only render (last wins)";
    // (20,32): inside only o0 (d(o0)≈6 < 14, d(o1)≈20 > 14). `both` applies o0's warp; `last` is plain source
    // there — they MUST differ, proving the scenes are genuinely distinct (the lap match is not trivial).
    const std::size_t only0 = static_cast<std::size_t>(32) * kW + 20;
    EXPECT_FALSE(near(both[only0], last[only0])) << "first-circle-only pixel must differ (test has teeth)";
}

// ── ColorFill gathering: byte-identical goldens for the built-in fast path ────────────────────────
//
// A built-in has no plain-twin shader to A/B against, so the proof inverts: each backend's committed
// golden holds the sequential per-region output for these scenes, and the gather path must reproduce
// those bytes EXACTLY — overlaps, blend modes, partial alpha, invert, stroke, and transform included
// (the gather composites every covering record in submission order with the per-step float16
// quantization replicated, so Tol::Exact is the contract, not a hope). A recapture
// (RETROPP_CAPTURE_GOLDEN) records whatever path is live — only valid from a build whose ColorFill
// regions render per-region, or the golden stops proving the equivalence.

// One ColorFill region. A shapeless `shape` (no points) is the whole-viewport grade.
Region fillRegion(const char* key, ShapePoints shape, Rgba8 colour, BlendMode blend = BlendMode::Normal,
                  float alpha = 1.0f, ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer) {
    Region reg{.key = key};
    reg.shape   = std::move(shape);
    reg.blend   = blend;
    reg.alpha   = alpha;
    reg.effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .scope = scope,
                                     .fill = colour}};
    return reg;
}

// Overlapping opaque Normal rectangles — the input_probe archetype. Order-sensitive where they lap
// (the later paint wins), so the golden pins sequential composition, not a union.
TEST_F(GoldenReadback, ColorFillGatherOverlap) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.regions.push_back(fillRegion("r0", ShapePoints::rectangle({10, 10}, 28, 20), Rgba8{255, 140, 0, 255}));
    frame.regions.push_back(fillRegion("r1", ShapePoints::rectangle({24, 16}, 28, 20), Rgba8{40, 120, 220, 255}));
    frame.regions.push_back(fillRegion("r2", ShapePoints::rectangle({18, 24}, 20, 24), Rgba8{60, 200, 90, 255}));
    frame.regions.push_back(fillRegion("r3", ShapePoints::rectangle({4, 44}, 56, 14),  Rgba8{70, 76, 100, 255}));
    runScene("colorfill_gather_overlap", Tol::Exact, frame, r);
}

// The full record surface in one contiguous run: blend modes compounding in the laps, partial alpha,
// an inverted region (fills the OUTSIDE), a stroked outline, and a transformed rectangle.
TEST_F(GoldenReadback, ColorFillGatherBlends) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    frame.regions.push_back(fillRegion("b0", ShapePoints::rectangle({8, 8}, 48, 24),
                                       Rgba8{90, 90, 180, 255}, BlendMode::Multiply));
    frame.regions.push_back(fillRegion("b1", ShapePoints::rectangle({16, 12}, 40, 28),
                                       Rgba8{40, 40, 10, 255}, BlendMode::Add));
    frame.regions.push_back(fillRegion("b2", ShapePoints::rectangle({12, 20}, 32, 20),
                                       Rgba8{220, 80, 80, 255}, BlendMode::Normal, 0.5f));
    ShapePoints inverted = ShapePoints::rectangle({20, 20}, 24, 24);
    inverted.invert      = true;
    frame.regions.push_back(fillRegion("b3", std::move(inverted),
                                       Rgba8{120, 200, 120, 255}, BlendMode::Multiply, 0.75f));
    ShapePoints outline  = ShapePoints::rectangle({10, 36}, 44, 20);
    outline.strokeWidth  = 3.0f;
    frame.regions.push_back(fillRegion("b4", std::move(outline), Rgba8{240, 240, 60, 255}));
    ShapePoints rotated  = ShapePoints::rectangle({24, 40}, 20, 12);
    rotated.transform    = Transform::rotation(25.0f, 34.0f, 46.0f);
    frame.regions.push_back(fillRegion("b5", std::move(rotated),
                                       Rgba8{60, 220, 220, 255}, BlendMode::Screen));
    runScene("colorfill_gather_blends", Tol::Exact, frame, r);
}

// Runs at all three confined sites in one scene: Layer-scope fills on an fx layer (the isolated chain +
// its last-step premultiplied-over composite), Below-scope fills (the accumulator swap), and a frame
// run whose last record is the shapeless whole-viewport grade (the synthesized rectangle gathers with
// its shaped peers).
TEST_F(GoldenReadback, ColorFillGatherSites) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking   b;
    addBaseScene(frame, art, b);
    DrawLayer fx{.key = "fx"};
    fx.z    = 5;
    fx.size = PixelSize{kW, kH};
    fx.regions.push_back(fillRegion("l0", ShapePoints::rectangle({6, 6}, 20, 12), Rgba8{200, 60, 60, 255}));
    fx.regions.push_back(fillRegion("l1", ShapePoints::rectangle({18, 10}, 20, 12), Rgba8{60, 200, 90, 255},
                                    BlendMode::Normal, 0.8f));
    fx.regions.push_back(fillRegion("w0", ShapePoints::rectangle({34, 30}, 18, 14), Rgba8{40, 40, 120, 255},
                                    BlendMode::Add, 1.0f, ScreenSpaceEffectScope::Below));
    fx.regions.push_back(fillRegion("w1", ShapePoints::rectangle({42, 36}, 18, 14), Rgba8{120, 40, 40, 255},
                                    BlendMode::Multiply, 0.6f, ScreenSpaceEffectScope::Below));
    frame.layers.push_back(std::move(fx));
    frame.regions.push_back(fillRegion("f0", ShapePoints::rectangle({6, 48}, 24, 10), Rgba8{230, 230, 240, 255}));
    frame.regions.push_back(fillRegion("f1", ShapePoints::rectangle({22, 52}, 24, 10), Rgba8{20, 20, 30, 255},
                                       BlendMode::Normal, 0.5f));
    frame.regions.push_back(fillRegion("grade", ShapePoints{}, Rgba8{200, 180, 160, 255},
                                       BlendMode::Multiply, 0.9f));
    runScene("colorfill_gather_sites", Tol::Exact, frame, r);
}

// The crisp invariant holds through the gather: a Viewport-grid compose at scale 3 of a gathered fill
// run must exactly equal the scale-1 capture nearest-upscaled (the shader's snap mirrors the gate's).
TEST_F(GoldenReadback, CrispParityColorFillGather) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);
    FrameDrawState frame;
    SceneBacking   b;
    addTileBackground(frame, art, b);
    frame.regions.push_back(fillRegion("c0", ShapePoints::rectangle({10, 10}, 28, 20), Rgba8{255, 140, 0, 255}));
    frame.regions.push_back(fillRegion("c1", ShapePoints::rectangle({24, 16}, 28, 20), Rgba8{40, 120, 220, 255},
                                       BlendMode::Multiply, 0.7f));
    runCrispParity("crisp_colorfill_gather", frame, r);
}

// A Bloom inside a Layer-scope sprite REGION, on-device: the bloomed pixel as the region's graded source,
// read from a field of that sprite's own light rather than summed per fragment. Behavioral properties
// against live baselines (no committed golden image — the field's arithmetic is the emission chain's, which
// emission_chain_test pins device-free):
//   - intensity 0 is an EXACT identity (the step grades with the pixel it started from).
//   - light SPREADS: a dim pixel inside the region gains from bright art beside it, which is the whole
//     difference between a blurred field and a per-pixel brightpass.
//   - the grade is CONFINED: a pixel of the same sprite outside the region's shape is byte-identical.
//   - EVERY step reads ITS OWN field: two sprites at different positions, each with a region Bloom, must
//     BOTH gain. One reading the other's array layer would sample that sprite's light where this one is —
//     empty — and gain nothing, which is exactly the fault a shared field or an off-by-one slot produces.
TEST_F(GoldenReadback, SpriteRegionBloom) {
    Renderer r{device_, nullptr, ViewportResolution{kW, kH}};
    const BaseArt art = uploadBaseArt(r);

    // 8×8 art: the left half bright, the right half dark but present. The bright half is the light source;
    // the dark half is where a spread halo shows up.
    std::array<std::uint8_t, 8 * 8> idx{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            idx[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)] = x < 4 ? 1 : 2;
    const AtlasId splitAtlas = r.uploadAtlas(idx.data(), 8, 8).atlasId;
    const std::array<Rgba8, 4> pal{{{0, 0, 0, 255},          // 0 unused
                                    {255, 245, 210, 255},    // 1 the bright half
                                    {24, 20, 16, 255},       // 2 the dark half
                                    {0, 0, 0, 255}}};
    const PaletteId palId = r.uploadPalette(std::span<const Rgba8>(pal));

    // The region covers the sprite's LEFT 6 columns only, so column 7 is inside the silhouette and outside
    // the shape — the confinement probe.
    auto regionBloomSprite = [&](std::string key, int x, int y, std::uint8_t intensity) {
        Sprite s{.key = std::move(key), .x = x, .y = y, .atlas = splitAtlas, .tile = 0, .palette = palId};
        s.size    = AssetDimensions{8, 8};
        s.regions = {Region{.key   = "lit",
                            .shape = ShapePoints::rectangle({0.0f, 0.0f}, 6.0f, 8.0f),
                            .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                                          .radius    = 5.0f,
                                                          .intensity = intensity}}}};
        return s;
    };
    auto scene = [&](SceneBacking& backing, std::vector<Sprite>& keep, std::vector<Sprite> sprites) {
        FrameDrawState f;
        addBaseScene(f, art, backing);
        keep = std::move(sprites);
        DrawLayer sp{.key = "lit_layer"};
        sp.z = 50; sp.size = PixelSize{kW, kH};
        sp.content = SpriteContent{.sprites = std::span<const Sprite>(keep)};
        f.layers.push_back(sp);
        return f;
    };
    auto sum = [&](const std::vector<Rgba8>& img, int x, int y) {
        const Rgba8 p = img[static_cast<std::size_t>(y) * kW + x];
        return static_cast<int>(p.r) + p.g + p.b;
    };

    // Baseline: the same sprite with an intensity-0 region Bloom — the identity.
    SceneBacking ob, lb;
    std::vector<Sprite> os, ls;
    FrameDrawState off = scene(ob, os, {regionBloomSprite("a", 20, 20, 0)});
    FrameDrawState lit = scene(lb, ls, {regionBloomSprite("a", 20, 20, 255)});
    const std::vector<Rgba8> plain  = r.captureViewport(off);
    const std::vector<Rgba8> bloomed = r.captureViewport(lit);
    ASSERT_EQ(plain.size(), bloomed.size());

    // Identity: intensity 0 against no region at all.
    SceneBacking nb;
    std::vector<Sprite> ns;
    Sprite bare{.key = "a", .x = 20, .y = 20, .atlas = splitAtlas, .tile = 0, .palette = palId};
    bare.size = AssetDimensions{8, 8};
    FrameDrawState none = scene(nb, ns, {bare});
    const std::vector<Rgba8> untouched = r.captureViewport(none);
    EXPECT_EQ(std::memcmp(untouched.data(), plain.data(), untouched.size() * sizeof(Rgba8)), 0)
        << "a region Bloom at intensity 0 is not an exact identity";

    // Spread: the dark half INSIDE the region (art column 5 → viewport x 25) gains light from the bright
    // half beside it. A per-pixel brightpass of a dark pixel would gain nothing.
    EXPECT_GT(sum(bloomed, 25, 24), sum(plain, 25, 24) + 2)
        << "the dark side of the region gained no light — the field is not spreading the bright half";

    // Confinement: art column 7 (viewport x 27) is inside the silhouette, outside the shape.
    EXPECT_EQ(sum(bloomed, 27, 24), sum(plain, 27, 24))
        << "the grade escaped the region's shape";

    // Two sprites, each with its own field, at different positions — both must gain.
    SceneBacking tb;
    std::vector<Sprite> ts;
    // The two differ in strength, so one reading the other's field is a visible error rather than a
    // coincidence that shades the same.
    FrameDrawState two = scene(tb, ts, {regionBloomSprite("a", 20, 20, 255),
                                        regionBloomSprite("b", 40, 40, 120)});
    SceneBacking tob;
    std::vector<Sprite> tos;
    FrameDrawState twoOff = scene(tob, tos, {regionBloomSprite("a", 20, 20, 0),
                                             regionBloomSprite("b", 40, 40, 0)});
    const std::vector<Rgba8> pair     = r.captureViewport(two);
    const std::vector<Rgba8> pairPlain = r.captureViewport(twoOff);
    EXPECT_GT(sum(pair, 25, 24), sum(pairPlain, 25, 24) + 2)
        << "the first sprite lost its halo when a second region Bloom joined the layer";
    EXPECT_GT(sum(pair, 45, 44), sum(pairPlain, 45, 44) + 2)
        << "the second sprite gained nothing — it is reading another step's field, not its own";

    // A field is addressed by its own RECT, and this is what proves it. The second sprite's field is packed
    // beside the first's rather than at the atlas origin, so a step that ignored the rect — reading the
    // origin, or a neighbor, or its own margin — would shade differently here than it does when its field
    // is the only one packed. Identical pixels mean it read exactly its own content either way.
    SceneBacking sb;
    std::vector<Sprite> ss;
    FrameDrawState solo = scene(sb, ss, {regionBloomSprite("b", 40, 40, 120)});
    const std::vector<Rgba8> alone = r.captureViewport(solo);
    for (int y = 38; y < 50; ++y)
        for (int x = 38; x < 50; ++x)
            ASSERT_EQ(sum(pair, x, y), sum(alone, x, y))
                << "the second sprite shaded differently once its field moved off the atlas origin, at ("
                << x << "," << y << ") — it is not addressing its own rect";

    // Nothing carries over between layers. The atlas is rewritten per layer, so a layer that did not clear
    // it would blur the previous layer's light — which lands exactly where this layer's blur reads. A halo
    // on the upper layer must therefore look the same whether or not a lower layer authored one.
    SceneBacking stacked, onlyUpper;
    std::vector<Sprite> lowerKeep, upperKeep, upperOnlyKeep;
    auto twoLayerScene = [&](SceneBacking& backing, std::vector<Sprite>& lower, std::vector<Sprite>& upper,
                             std::vector<Sprite> lowerSprites, std::vector<Sprite> upperSprites) {
        FrameDrawState f;
        addBaseScene(f, art, backing);
        lower = std::move(lowerSprites);
        upper = std::move(upperSprites);
        DrawLayer lo{.key = "lower_lit"};
        lo.z = 50; lo.size = PixelSize{kW, kH};
        lo.content = SpriteContent{.sprites = std::span<const Sprite>(lower)};
        f.layers.push_back(lo);
        DrawLayer up{.key = "upper_lit"};
        up.z = 60; up.size = PixelSize{kW, kH};
        up.content = SpriteContent{.sprites = std::span<const Sprite>(upper)};
        f.layers.push_back(up);
        return f;
    };
    // The two scenes are identical in every way but the lower halo's strength — same layers, same sprites,
    // same geometry — so any difference up top is light that carried over, not a composition difference.
    std::vector<Sprite> dimLower;
    FrameDrawState bothLayers = twoLayerScene(stacked, lowerKeep, upperKeep,
                                              {regionBloomSprite("low", 20, 20, 255)},
                                              {regionBloomSprite("up", 44, 44, 255)});
    FrameDrawState upperAlone = twoLayerScene(onlyUpper, dimLower, upperOnlyKeep,
                                              {regionBloomSprite("low", 20, 20, 0)},
                                              {regionBloomSprite("up", 44, 44, 255)});
    const std::vector<Rgba8> withLower = r.captureViewport(bothLayers);
    const std::vector<Rgba8> without   = r.captureViewport(upperAlone);
    for (int y = 42; y < 54; ++y)
        for (int x = 42; x < 54; ++x)
            ASSERT_EQ(sum(withLower, x, y), sum(without, x, y))
                << "the upper layer's halo changed when a lower layer authored one, at (" << x << "," << y
                << ") — the atlas is carrying stale light between layers";

    // A reach at or past kEmissionDownsampleRadius blurs a QUARTER-SIZE copy of the atlas and resolves it
    // back, which is a different code path from everything above — every clause so far runs at radius 5, on
    // the full-resolution one. A wide halo has to spread too, and further than a narrow one.
    SceneBacking wb, wob;
    std::vector<Sprite> ws, wos;
    auto wideSprite = [&](std::uint8_t intensity) {
        Sprite s{.key = "wide", .x = 20, .y = 20, .atlas = splitAtlas, .tile = 0, .palette = palId};
        s.size    = AssetDimensions{8, 8};
        s.regions = {Region{.key   = "lit",
                            .shape = ShapePoints::rectangle({0.0f, 0.0f}, 6.0f, 8.0f),
                            .effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Bloom,
                                                          .radius    = 12.0f,
                                                          .intensity = intensity}}}};
        return s;
    };
    FrameDrawState wideOn  = scene(wb, ws, {wideSprite(255)});
    FrameDrawState wideOff = scene(wob, wos, {wideSprite(0)});
    const std::vector<Rgba8> wide     = r.captureViewport(wideOn);
    const std::vector<Rgba8> wideBase = r.captureViewport(wideOff);
    EXPECT_GT(sum(wide, 25, 24), sum(wideBase, 25, 24) + 2)
        << "a reach on the reduced path spread no light — the quarter-resolution blur is not resolving back";
    // Reaching further shows up as a FLATTER falloff, not a brighter pixel: a normalized Gaussian spreads
    // the same light over more area, so a wide halo is dimmer near its source and carries further from it.
    // Column 24 sits beside the bright half, column 25 one further out.
    const int nearNarrow = sum(bloomed, 24, 24) - sum(plain, 24, 24);
    const int farNarrow  = sum(bloomed, 25, 24) - sum(plain, 25, 24);
    const int nearWide   = sum(wide, 24, 24) - sum(wideBase, 24, 24);
    const int farWide     = sum(wide, 25, 24) - sum(wideBase, 25, 24);
    ASSERT_GT(nearNarrow, 0);
    ASSERT_GT(nearWide, 0);
    EXPECT_GT(farWide * nearNarrow, farNarrow * nearWide)
        << "the reduced-path halo falls off as steeply as the full-resolution one — its taps are not "
           "reaching further (near/far narrow " << nearNarrow << "/" << farNarrow << ", wide " << nearWide
        << "/" << farWide << ")";
}

}  // namespace
