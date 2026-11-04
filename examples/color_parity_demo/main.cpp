// Color-parity demo — a runnable host that VISUALLY proves what a container blend grade combines OVER,
// which is the effect's scope. One holed lattice layer sits over a colorful scene; the SAME ColorFill
// grade (one blend mode, one fill) is confined to the left half at Layer scope and to the right half at
// Below scope, so the only thing that differs across the mid-screen seam is what the grade reaches:
//
//   • LEFT half — Layer scope: the grade colors the lattice's OWN art in place. A mode that reads its
//     destination has nothing to grade in the lattice's holes, so they stay see-through and the scene shows
//     THROUGH them in its true colors — the tint never touches what is behind the layer.
//   • RIGHT half — Below scope: the grade colors the accumulated image beneath the layer — the lattice AND
//     the scene showing through its holes. The tint reaches through the holes; the windows are tinted too.
//
// Press A to cycle the blend mode (Multiply → Add → Screen → Subtract → Half → HalfSubtract → Normal) and
// watch the seam hold: at every mode that reads its destination, Layer keeps the holes clear and Below grades
// through them. Normal is the alpha-over, a plain fill: it paints the holes on both halves, and the seam
// disappears. This is the layer-path precedent for a per-sprite effect — a sprite is holed layer content, and
// its effect grades its own art (Layer) or the scene beneath it (Below) the same way.
//
// The pixel-exact blend math is the ctest suite's job (applyBlendMode / colorFillParams vs the shaders, the
// per-layer scope matrix in color_parity_layer_test.cpp, and the holes under every mode in
// blend_empty_destination_test.cpp); this is the live GPU check.
//
// The scene is STATIC — a key press swaps a static image, nothing strobes, the window never auto-launches
// (a dev drives it). A = cycle blend mode; Backspace = fullscreen; close to quit.
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: draws the demo's own frame at every mode, with and
//               without its two grades, and requires — at every mode that reads its destination — every
//               lattice hole on the left half to equal the frame drawn without the grades and every lattice
//               bar there to match the CPU mirror, applyBlendMode; every pixel on the right half to match the
//               mirror, holes included; and under Normal, every pixel on both halves to be the fill. Exits
//               nonzero on any miss (CI runs this on every platform); on Windows on ARM, whose CI runner has
//               no GPU device, exits 77 — a counted skip.

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
#include "retropp/image.h"  // TransparentIndices
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

constexpr Rgba8 kTint{70, 90, 200};  // one strong blue grade, same on both halves — only the scope differs

// The blend modes the demo cycles through, with a label for the console line.
struct ModeEntry {
    BlendMode   mode;
    const char* name;
};
constexpr std::array<ModeEntry, 7> kModes{{{BlendMode::Multiply, "Multiply"},
                                           {BlendMode::Add, "Add"},
                                           {BlendMode::Screen, "Screen"},
                                           {BlendMode::Subtract, "Subtract"},
                                           {BlendMode::Half, "Half"},
                                           {BlendMode::HalfSubtract, "HalfSubtract"},
                                           {BlendMode::Normal, "Normal"}}};

enum class Action : std::uint8_t { CycleMode, Fullscreen };

// The scene's and the lattice's art, uploaded once, and the tile maps that place them.
struct Art {
    std::vector<TileCell> sceneCells;
    std::vector<TileCell> latticeCells;
};

Art uploadArt(Renderer& renderer) {
    // The opaque scene the grade combines over: a four-color diagonal pattern, so a tint reads clearly and
    // "the true colors through the holes" is obvious.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    const AtlasId sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8).atlasId;  // opaque (default None)
    const std::array<Rgba8, 4> scenePal{{{210, 205, 120}, {90, 170, 210}, {200, 120, 90}, {120, 200, 130}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));

    // The holed foreground: an 8×8 tile that is an opaque bar frame around a transparent WINDOW — index 0 is
    // the hole (TransparentIndices::GameBoy), the 2-px border is an opaque bar. Tiled across, it is a grid of
    // windows the scene shows through.
    std::array<std::uint8_t, 64> latticeArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool border = (x < 2 || y < 2);  // top/left bars → an opaque lattice with square windows
            latticeArt[static_cast<std::size_t>(y) * 8 + x] = border ? 1 : 0;  // 0 = hole
        }
    const AtlasId latticeAtlas = renderer.uploadAtlas(latticeArt.data(), 8, 8, TransparentIndices::GameBoy).atlasId;
    const std::array<Rgba8, 2> latticePal{{{0, 0, 0}, {235, 235, 240}}};  // index 0 unused (hole); 1 = bars
    const PaletteId latticePalId = renderer.uploadPalette(std::span<const Rgba8>(latticePal));

    return Art{
        .sceneCells   = std::vector<TileCell>(static_cast<std::size_t>(kMapW) * kMapH,
                                              TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId}),
        .latticeCells = std::vector<TileCell>(static_cast<std::size_t>(kMapW) * kMapH,
                                              TileCell{.atlas = latticeAtlas, .tile = 0, .palette = latticePalId}),
    };
}

// Whether a viewport pixel falls in one of the lattice's windows — from the lattice tile's own layout.
[[nodiscard]] bool latticeHole(int x, int y) { return (x % 8) >= 2 && (y % 8) >= 2; }

// A ColorFill of `color`, at the given scope, confined by its owning Region and graded by that region's
// blend. Layer scope grades the region's own layer content; Below scope grades the accumulated image.
[[nodiscard]] Region gradeRect(ShapePoints shape, Rgba8 color, BlendMode mode,
                               ScreenSpaceEffectScope scope) {
    return Region{.key     = "grade",
                  .shape   = std::move(shape),
                  .effects = {ScreenSpaceEffect{
                      .kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = color}},
                  .blend   = mode};
}

// The demo's frame: the scene, and the lattice over it carrying its two grades when `withGrades`.
void buildFrame(FrameDrawState& frame, const Art& art, BlendMode mode, bool withGrades) {
    frame.layers.clear();

    // The opaque scene (whole screen).
    DrawLayer scene{.key = "scene"};
    scene.z       = 0;
    scene.size    = PixelSize{kViewW, kViewH};
    scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                .cells = std::span<const TileCell>(art.sceneCells)};
    frame.layers.push_back(scene);

    // The holed lattice, over the scene. Its two grade regions differ ONLY in scope: the left half at Layer
    // scope (the grade composes over the lattice's own content, where a hole is a destination of nothing),
    // the right half at Below scope (over the composited image beneath, reaching through the holes). Same
    // tint, same mode, one seam.
    DrawLayer lattice{.key = "lattice"};
    lattice.z       = 10;
    lattice.size    = PixelSize{kViewW, kViewH};
    lattice.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                  .cells = std::span<const TileCell>(art.latticeCells)};
    if (withGrades) {
        lattice.regions = {
            gradeRect(ShapePoints::rectangle(Point{0, 0}, kViewW / 2, kViewH), kTint, mode,
                      ScreenSpaceEffectScope::Layer),
            gradeRect(ShapePoints::rectangle(Point{kViewW / 2, 0}, kViewW / 2, kViewH), kTint, mode,
                      ScreenSpaceEffectScope::Below),
        };
    }
    frame.layers.push_back(lattice);
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────

Vec4 norm(Rgba8 c) {
    return Vec4{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
}
std::uint8_t quant(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
bool exactEq(Rgba8 a, Rgba8 b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
int chDelta(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)), std::abs(int(a.b) - int(b.b)),
                     std::abs(int(a.a) - int(b.a))});
}

// The tint graded over an opaque pixel under `mode`, as the CPU mirror computes it.
Rgba8 mirror(Rgba8 under, BlendMode mode) {
    const ColorFillParams fp =
        colorFillParams(ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = kTint});
    const Vec4 out = applyBlendMode(norm(under), Vec4{fp.r, fp.g, fp.b, 1.0f}, mode);
    return Rgba8{quant(out.x), quant(out.y), quant(out.z), quant(out.w)};
}

int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("color-parity verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("color-parity verify: no GPU device (%s)%s\n", SDL_GetError(),
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

        for (const ModeEntry& m : kModes) {
            buildFrame(frame, art, m.mode, /*withGrades=*/false);
            const std::vector<Rgba8> A = r.captureViewport(frame);  // the scene and the lattice
            buildFrame(frame, art, m.mode, /*withGrades=*/true);
            const std::vector<Rgba8> G = r.captureViewport(frame);  // and the two grades over them

            int clearMissed = 0, gradeMissed = 0, holesGradedRight = 0, barsMoved = 0;
            for (int y = 0; y < kViewH; ++y) {
                for (int x = 0; x < kViewW; ++x) {
                    const std::size_t i     = static_cast<std::size_t>(y) * kViewW + static_cast<std::size_t>(x);
                    const bool        left  = x < kViewW / 2;
                    const bool        hole  = latticeHole(x, y);
                    const Rgba8       want  = mirror(A[i], m.mode);
                    if (left && hole && m.mode != BlendMode::Normal) {
                        if (!exactEq(G[i], A[i])) ++clearMissed;  // nothing to grade: the scene as it was
                        continue;
                    }
                    if (chDelta(G[i], want) > 2) ++gradeMissed;
                    if (!left && hole && chDelta(G[i], A[i]) > 2) ++holesGradedRight;
                    if (left && !hole && chDelta(G[i], A[i]) > 2) ++barsMoved;
                }
            }
            const std::string name = m.name;
            check(clearMissed == 0, name + ": " + std::to_string(clearMissed) +
                                        " lattice holes on the Layer half differ from the frame without the grade");
            check(gradeMissed == 0, name + ": " + std::to_string(gradeMissed) +
                                        " graded pixels differ from the mirror by more than 2 steps");
            check(holesGradedRight > 0, name + ": the Below half graded none of the scene through the holes");
            check(barsMoved > 0, name + ": the Layer half graded none of the lattice's bars");
            std::printf("  %-12s Layer holes clear: %s; every graded pixel within 2 steps of the mirror: %s\n",
                        m.name, m.mode == BlendMode::Normal ? "painted by the fill" : (clearMissed == 0 ? "yes" : "NO"),
                        gradeMissed == 0 ? "yes" : "NO");
        }
        std::printf("color-parity verify: %zu modes, both scopes; %s\n", kModes.size(),
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
        .identity = {.organization = "Retro++", .application = "Color Parity Demo"},
        .window   = {.title = "Polyrhythm — color-parity demo (Layer vs Below scope over holes)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::CycleMode, {SDL_SCANCODE_A, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    const Art art = uploadArt(renderer);

    int modeIdx = 0;
    auto announce = [&]() {
        const ModeEntry& m = kModes[static_cast<std::size_t>(modeIdx)];
        if (m.mode == BlendMode::Normal) {
            std::printf("color-parity demo — blend mode: Normal. A plain fill paints both halves, holes "
                        "included. A = next mode; Backspace = fullscreen; close to quit.\n");
            return;
        }
        std::printf("color-parity demo — blend mode: %s. LEFT half = Layer scope (holes stay clear, the "
                    "scene shows through untinted); RIGHT half = Below scope (the tint reaches through the "
                    "holes). A = next mode; Backspace = fullscreen; close to quit.\n",
                    m.name);
    };
    announce();

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::CycleMode)) {
            modeIdx = (modeIdx + 1) % static_cast<int>(kModes.size());
            announce();
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        buildFrame(frame, art, kModes[static_cast<std::size_t>(modeIdx)].mode, /*withGrades=*/true);
        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
