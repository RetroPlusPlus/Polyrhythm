// Blend-mode demo — a runnable host that VISUALLY proves the container blend modes: each compositing
// container (a DrawLayer, a Region, the FrameDrawState) carries a BlendMode beside its alpha that governs
// HOW its pixels combine with what they sit on. It opens a window over an opaque slate-grid scene and lays
// out one clearly distinct element per kind:
//
//   • LEFT HALF — a translucent HALF LAYER (DrawLayer::blend = Half): a warm color layer covering only the
//     left half (a finite tilemap with a hard right edge), averaged with the scene, (dst + src) / 2. The
//     visible boundary down the middle shows the whole layer is translucent — no per-pixel alpha.
//   • RIGHT HALF — FOUR mode circles, each a Region with the SAME gray ColorFill but a different
//     Region::blend, so the only difference you see is the blend math:
//        top-left  Add        dst + src              (brightens)
//        top-right Subtract   dst − src              (darkens hard)
//        bot-left  Multiply   dst · src              (darkens)
//        bot-right Screen     1 − (1 − dst)(1 − src) (lifts toward light)
//   • BOTTOM STRIP — a HALF-SUBTRACT pane (Region::blend = HalfSubtract) across both halves: a dark ColorFill
//     whose halved difference, (dst − src) / 2, halves and darkens the warm layer and the slate behind it —
//     the dimmed backdrop a menu sits on.
//   • WHOLE FRAME — a gentle cool SCREEN overlay (FrameDrawState::blend = Screen): a whole-frame
//     ColorFill combined over the composited image with the frame's blend mode, lifting the scene slightly.
//
// A Normal container (the default) is the plain alpha-over the compositor always ran — that is the
// untouched slate grid.
//
// Backspace = fullscreen; close to quit.
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: draws the demo's own frame with and without its
//               regions and requires every pixel inside each region to match the CPU mirror,
//               applyBlendMode, for that region's blend, and every pixel off every region to equal the frame
//               drawn without them. Exits nonzero on any miss (CI runs this on every platform); on Windows
//               on ARM, whose CI runner has no GPU device, exits 77 — a counted skip.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/postprocess.h"  // applyBlendMode, colorFillParams
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport
constexpr int kHalfW = 10;             // the Half layer's tilemap is 10 tiles wide → the left half only

// The four circles and the pane, and the whole-frame overlay's color.
constexpr Rgba8 kGray{120, 120, 120};
constexpr float kCircleR = 18.0f;
constexpr Rgba8 kPaneFill{40, 40, 40};
constexpr Point kPaneTopLeft{8, 120};
constexpr float kPaneW = 144.0f, kPaneH = 18.0f;
constexpr Rgba8 kOverlayFill{34, 44, 78};

// The demo's input vocabulary: one dev key.
enum class Action : std::uint8_t { Fullscreen };

// A solid color fill (paint the region's shape this color). The owning Region's blend mode grades how it
// combines over the scene — Add brightens, Subtract/Multiply darken, Screen lifts, HalfSubtract dims.
[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 color) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = color};
}

// A Region keyed `key` that fills `shape` with `color` and composites it over the scene with `mode`.
[[nodiscard]] Region blendedFill(const char* key, ShapePoints shape, Rgba8 color, BlendMode mode) {
    return Region{.key = key, .shape = std::move(shape), .effects = {solidFill(color)}, .blend = mode};
}

// The scene's art: the slate grid and the Half layer's warm tiles.
struct Art {
    AtlasId               gridAtlas{};
    PaletteId             gridPalId{};
    std::vector<TileCell> gridCells;
    AtlasId               warmAtlas{};
    PaletteId             warmPalId{};
    std::vector<TileCell> warmCells;
};

[[nodiscard]] Art uploadArt(Renderer& renderer) {
    Art art;

    // The opaque scene the containers blend over: a two-tone slate grid.
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    art.gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {52, 58, 82}, {72, 80, 110}}};  // opaque slate grid
    art.gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    art.gridCells.assign(static_cast<std::size_t>(kMapW) * kMapH,
                         TileCell{.atlas = art.gridAtlas, .tile = 0, .palette = art.gridPalId});

    // The Half layer's art: solid warm tiles, so the Half blend is a clean (grid + warm) / 2 average.
    std::array<std::uint8_t, 64> warmArt{};
    warmArt.fill(1);
    art.warmAtlas = renderer.uploadAtlas(warmArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 2> warmPal{{{0, 0, 0}, {235, 120, 40}}};  // warm orange
    art.warmPalId = renderer.uploadPalette(std::span<const Rgba8>(warmPal));
    art.warmCells.assign(static_cast<std::size_t>(kHalfW) * kMapH,
                         TileCell{.atlas = art.warmAtlas, .tile = 0, .palette = art.warmPalId});
    return art;
}

// The demo's frame: the scene, the Half layer and the whole-frame Screen overlay, then — when `withRegions` —
// the four circles and the pane.
void buildFrame(FrameDrawState& frame, const Art& art, bool withRegions) {
    frame.layers.clear();

    // The opaque scene (whole screen).
    DrawLayer bg{.key = "backgroundGrid"};
    bg.z       = -10;
    bg.size    = PixelSize{kViewW, kViewH};
    bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                             .cells = std::span<const TileCell>(art.gridCells)};
    frame.layers.push_back(bg);

    // The translucent HALF LAYER: a finite 10×18 warm map (wrap = Blank), so it covers ONLY the left
    // half and is transparent past its right edge. blend = Half averages it with the scene, (dst+src)/2,
    // so the whole left panel is translucent with no per-pixel alpha — the visible mid-screen boundary.
    DrawLayer half{.key = "halfLayer"};
    half.z       = 0;
    half.size    = PixelSize{kViewW, kViewH};
    half.blend   = BlendMode::Half;
    half.content = TileContent{.widthInTiles = kHalfW, .heightInTiles = kMapH,
                               .cells = std::span<const TileCell>(art.warmCells),
                               .wrap = TileWrap::Blank};
    frame.layers.push_back(half);

    frame.regions.clear();
    if (withRegions) {
        // FOUR mode circles in the right half, same gray fill so only the blend differs.
        frame.regions.push_back(blendedFill("add", ShapePoints::circle(Point{104, 44}, kCircleR), kGray,
                                            BlendMode::Add));
        frame.regions.push_back(blendedFill("subtract", ShapePoints::circle(Point{140, 44}, kCircleR), kGray,
                                            BlendMode::Subtract));
        frame.regions.push_back(blendedFill("multiply", ShapePoints::circle(Point{104, 100}, kCircleR), kGray,
                                            BlendMode::Multiply));
        frame.regions.push_back(blendedFill("screen", ShapePoints::circle(Point{140, 100}, kCircleR), kGray,
                                            BlendMode::Screen));
        // A HALF-SUBTRACT pane across the bottom, straddling the warm layer and the slate: the scene behind
        // it halved and darkened, (dst − src) / 2 — the dimmed backdrop a menu sits on.
        frame.regions.push_back(blendedFill("pane", ShapePoints::rectangle(kPaneTopLeft, kPaneW, kPaneH),
                                            kPaneFill, BlendMode::HalfSubtract));
    }

    // The frame-level SCREEN overlay: a cool whole-frame ColorFill, combined over the composited image
    // with the frame's blend mode — Screen lifts the scene gently toward light. The frame's whole-frame
    // effects apply before its regions, so each region grades the lifted scene.
    frame.postEffects.clear();
    frame.postEffects.push_back(solidFill(kOverlayFill));
    frame.blend = BlendMode::Screen;
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
[[nodiscard]] std::uint8_t quant(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
[[nodiscard]] bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
[[nodiscard]] int  chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
}

// An opaque ColorFill of `fill` combined over `dst` under `mode` — the applyBlendMode / colorFillParams
// authority.
[[nodiscard]] Vec4 fillOver(Vec4 dst, Rgba8 fill, BlendMode mode) {
    const ColorFillParams fp =
        colorFillParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = fill});
    return applyBlendMode(dst, Vec4{fp.r, fp.g, fp.b, 1.0f}, mode);
}

// One region of the frame: where its pixels lie, and the fill and blend it grades them with.
struct RegionProbe {
    const char* key;
    Rgba8       fill;
    BlendMode   mode;
    bool        circle;
    Point       center;     // a circle's center
    Point       topLeft;    // a rectangle's corner
    float       w = 0, h = 0;

    // Strictly inside: a pixel center more than 1.5 px inside a circle's edge, or 1 px inside a rectangle's.
    [[nodiscard]] bool inside(int x, int y) const {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
        if (circle) return std::hypot(px - center.x, py - center.y) <= kCircleR - 1.5f;
        return px >= topLeft.x + 1 && px < topLeft.x + w - 1 && py >= topLeft.y + 1 && py < topLeft.y + h - 1;
    }
    // Clear of the shape: past its edge by the same margin.
    [[nodiscard]] bool clear(int x, int y) const {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
        if (circle) return std::hypot(px - center.x, py - center.y) >= kCircleR + 1.5f;
        return px < topLeft.x - 1 || px >= topLeft.x + w + 1 || py < topLeft.y - 1 || py >= topLeft.y + h + 1;
    }
};

int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("blend-mode verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("blend-mode verify: no GPU device (%s)%s\n", SDL_GetError(),
                    kDeviceOptional ? " — skipped on this platform" : "");
        SDL_Quit();
        return kDeviceOptional ? 77 : 1;
    }

    int        failures = 0;
    const auto check    = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    };

    {
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);
        const Art      art = uploadArt(r);
        FrameDrawState frame;
        const auto     capture = [&](bool withRegions) {
            buildFrame(frame, art, withRegions);
            return r.captureViewport(frame);
        };
        const std::vector<Rgba8> A = capture(false);  // the scene, the Half layer and the overlay
        const std::vector<Rgba8> G = capture(true);   // and the regions over them

        const std::array<RegionProbe, 5> regions{{
            {.key = "add", .fill = kGray, .mode = BlendMode::Add, .circle = true, .center = Point{104, 44}},
            {.key = "subtract", .fill = kGray, .mode = BlendMode::Subtract, .circle = true, .center = Point{140, 44}},
            {.key = "multiply", .fill = kGray, .mode = BlendMode::Multiply, .circle = true, .center = Point{104, 100}},
            {.key = "screen", .fill = kGray, .mode = BlendMode::Screen, .circle = true, .center = Point{140, 100}},
            {.key = "pane", .fill = kPaneFill, .mode = BlendMode::HalfSubtract, .circle = false,
             .topLeft = kPaneTopLeft, .w = kPaneW, .h = kPaneH},
        }};

        int leaked = 0;
        for (const RegionProbe& rg : regions) {
            int inside = 0, mismatched = 0, moved = 0, worst = 0;
            for (int y = 0; y < kViewH; ++y)
                for (int x = 0; x < kViewW; ++x) {
                    if (!rg.inside(x, y)) continue;
                    const std::size_t i = static_cast<std::size_t>(y) * kViewW + static_cast<std::size_t>(x);
                    ++inside;
                    const Vec4  want4 = fillOver(norm(A[i]), rg.fill, rg.mode);
                    const Rgba8 want{quant(want4.x), quant(want4.y), quant(want4.z), quant(want4.w)};
                    const int   d = chDelta(G[i], want);
                    worst         = std::max(worst, d);
                    if (d > 2) ++mismatched;
                    if (chDelta(G[i], A[i]) > 2) ++moved;
                }
            check(inside > 0, std::string(rg.key) + ": no pixel lies inside the region");
            check(mismatched == 0, std::string(rg.key) + ": " + std::to_string(mismatched) + " of " +
                                       std::to_string(inside) + " pixels differ from the mirror by more than 2 "
                                       "steps (worst " + std::to_string(worst) + ")");
            check(moved > 0, std::string(rg.key) + ": the region moved no pixel off the frame drawn without it");
        }
        for (int y = 0; y < kViewH; ++y)
            for (int x = 0; x < kViewW; ++x) {
                const bool offAll = std::all_of(regions.begin(), regions.end(),
                                                [&](const RegionProbe& rg) { return rg.clear(x, y); });
                const std::size_t i = static_cast<std::size_t>(y) * kViewW + static_cast<std::size_t>(x);
                if (offAll && !exactEq(G[i], A[i])) ++leaked;
            }
        check(leaked == 0, std::to_string(leaked) + " pixels changed off every region");

        std::printf("blend-mode verify: %zu regions against the mirror; %s\n", regions.size(),
                    failures == 0 ? "PASS" : "FAIL");
    }

    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Blend Mode Demo"},
        .window = {.title = "Polyrhythm — blend-mode demo (Half layer / Add / Subtract / Multiply / Screen / "
                            "HalfSubtract pane)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    const Art art = uploadArt(renderer);

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        buildFrame(frame, art, /*withRegions=*/true);
        renderer.renderFrame(frame);
    });

    std::printf("blend-mode demo — LEFT half: a translucent HALF layer (DrawLayer::blend = Half), with a hard "
                "boundary down the middle. RIGHT half, four circles (same gray fill, different Region::blend): "
                "top-left Add, top-right Subtract, bottom-left Multiply, bottom-right Screen. BOTTOM strip: a "
                "HalfSubtract pane (Region::blend = HalfSubtract) dimming the warm layer and the slate behind "
                "it. The whole scene is gently lifted by a frame-level Screen overlay (FrameDrawState::blend). "
                "Static scene; Backspace = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
