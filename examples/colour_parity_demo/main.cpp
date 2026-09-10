// Colour-parity demo — a runnable host that VISUALLY proves what a container blend grade combines OVER,
// which is the effect's scope. One holed lattice layer sits over a colourful scene; the SAME ColorFill
// grade (one blend mode, one fill) is confined to the left half at Layer scope and to the right half at
// Below scope, so the only thing that differs across the mid-screen seam is what the grade reaches:
//
//   • LEFT half — Layer scope: the grade colours the lattice's OWN art in place. The lattice's holes stay
//     transparent, so the scene shows THROUGH them in its true colours — the tint never touches what is
//     behind the layer.
//   • RIGHT half — Below scope: the grade colours the accumulated image beneath the layer — the lattice AND
//     the scene showing through its holes. The tint reaches through the holes; the windows are tinted too.
//
// Press A to cycle the blend mode (Multiply → Add → Screen → Subtract → Half → Normal) and watch the seam
// hold: at every mode, Layer keeps the holes clear and Below grades through them. This is the layer-path
// precedent for a per-sprite effect — a sprite is holed layer content, and its effect grades its own art
// (Layer) or the scene beneath it (Below) the same way.
//
// The pixel-exact blend math is the ctest suite's job (applyBlendMode / colorFillParams vs the shaders, and
// the per-layer scope matrix in colour_parity_layer_test.cpp); this is the live GPU sanity check.
//
// The scene is STATIC — a key press swaps a static image, nothing strobes, the window never auto-launches
// (a dev drives it). A = cycle blend mode; Backspace = fullscreen; close to quit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"  // TransparentIndices
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;  // 20×18 tiles cover the 160×144 viewport

// The blend modes the demo cycles through, with a label for the console line.
struct ModeEntry {
    BlendMode   mode;
    const char* name;
};
constexpr std::array<ModeEntry, 6> kModes{{{BlendMode::Multiply, "Multiply"},
                                           {BlendMode::Add, "Add"},
                                           {BlendMode::Screen, "Screen"},
                                           {BlendMode::Subtract, "Subtract"},
                                           {BlendMode::Half, "Half"},
                                           {BlendMode::Normal, "Normal"}}};

enum class Action : std::uint8_t { CycleMode, Fullscreen };

// A ColorFill of `colour`, at the given scope, confined by its owning Region and graded by that region's
// blend. Layer scope grades the region's own layer content; Below scope grades the accumulated image.
[[nodiscard]] Region gradeRect(ShapePoints shape, Rgba8 colour, BlendMode mode,
                               ScreenSpaceEffectScope scope) {
    return Region{.key     = "grade",
                  .shape   = std::move(shape),
                  .effects = {ScreenSpaceEffect{
                      .kind = ScreenSpaceEffectKind::ColorFill, .scope = scope, .fill = colour}},
                  .blend   = mode};
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Colour Parity Demo"},
        .window   = {.title = "Polyrhythm — colour-parity demo (Layer vs Below scope over holes)"}};
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

    // The opaque scene the grade combines over: a four-colour diagonal pattern, so a tint reads clearly and
    // "the true colours through the holes" is obvious.
    std::array<std::uint8_t, 64> sceneArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sceneArt[static_cast<std::size_t>(y) * 8 + x] = static_cast<std::uint8_t>(((x + y) / 2) % 4);
    const AtlasId sceneAtlas = renderer.uploadAtlas(sceneArt.data(), 8, 8).atlasId;  // opaque (default None)
    const std::array<Rgba8, 4> scenePal{{{210, 205, 120}, {90, 170, 210}, {200, 120, 90}, {120, 200, 130}}};
    const PaletteId scenePalId = renderer.uploadPalette(std::span<const Rgba8>(scenePal));
    const std::vector<TileCell> sceneCells(static_cast<std::size_t>(kMapW) * kMapH,
                                           TileCell{.atlas = sceneAtlas, .tile = 0, .palette = scenePalId});

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
    const std::vector<TileCell> latticeCells(static_cast<std::size_t>(kMapW) * kMapH,
                                             TileCell{.atlas = latticeAtlas, .tile = 0, .palette = latticePalId});

    constexpr Rgba8 tint{70, 90, 200};  // one strong blue grade, same on both halves — only the scope differs

    int modeIdx = 0;
    auto announce = [&]() {
        std::printf("colour-parity demo — blend mode: %s. LEFT half = Layer scope (holes stay clear, the "
                    "scene shows through untinted); RIGHT half = Below scope (the tint reaches through the "
                    "holes). A = next mode; Backspace = fullscreen; close to quit.\n",
                    kModes[static_cast<std::size_t>(modeIdx)].name);
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
        const BlendMode mode = kModes[static_cast<std::size_t>(modeIdx)].mode;
        frame.layers.clear();

        // The opaque scene (whole screen).
        DrawLayer scene{.key = "scene"};
        scene.z       = 0;
        scene.size    = PixelSize{kViewW, kViewH};
        scene.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                    .cells = std::span<const TileCell>(sceneCells)};
        frame.layers.push_back(scene);

        // The holed lattice, over the scene. Its two grade regions differ ONLY in scope: the left half at
        // Layer scope (Multiply composes over the lattice's own content, where a hole is a destination of
        // nothing), the right half at Below scope (over the composited image beneath, reaching through the
        // holes). Same tint, same mode, one seam.
        DrawLayer lattice{.key = "lattice"};
        lattice.z       = 10;
        lattice.size    = PixelSize{kViewW, kViewH};
        lattice.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                      .cells = std::span<const TileCell>(latticeCells)};
        lattice.regions = {
            gradeRect(ShapePoints::rectangle(Point{0, 0}, kViewW / 2, kViewH), tint, mode,
                      ScreenSpaceEffectScope::Layer),
            gradeRect(ShapePoints::rectangle(Point{kViewW / 2, 0}, kViewW / 2, kViewH), tint, mode,
                      ScreenSpaceEffectScope::Below),
        };
        frame.layers.push_back(lattice);

        renderer.renderFrame(frame);
    });

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
