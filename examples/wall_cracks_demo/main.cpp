// Cracked-wall demo — a runnable showcase of MATERIAL palette transparency: a palette entry's own
// alpha makes the holes. A brick wall is drawn over a slowly drifting background and colored through
// palette IMAGES (16-bit RGBA PNGs loaded with Renderer::loadPaletteImage), whose alpha does three
// visible things:
//
//   1. ALPHA 0 = a true hole. The wall palette's entry 0 is alpha 0; a "missing brick" tile is all
//      index 0, so the shader discards it and the background shows through. The background SCROLLS, so
//      you watch it move through the gaps — the proof the hole is real, not a painted-on color.
//   2. ALPHA 1-254 = a blend. The wall's "weathered" entry is partial-alpha, so weathered bricks let
//      the moving background faintly bleed through them.
//   3. The same on the SPRITE path. A fracture is a column of sprites colored through a crack palette
//      whose surround entry is alpha 0 (so only the crack lines composite onto the wall) and whose edge
//      entry is partial-alpha (soft crack edges that blend over the brick). A Tween eases the fracture
//      layer's alpha in and back out.
//
// Transparency here is the PALETTE'S alpha — never a color key and never a per-index hole list (the
// structural TransparentIndices set is the other mechanism, shown by palette_image_demo). The atlas is
// uploaded with TransparentIndices::None; every hole is a palette entry at alpha 0.
//
// Controls (printf-labeled, no on-screen text): A pauses/resumes the fracture fade; B pauses/resumes
// the background drift. The window does not open until you run the demo. One of the runnable example
// hosts: it keeps the live loadPaletteImage / loadAtlas / Tween path compiling on every CI platform
// even though CI never opens a window.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/tween.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;
using namespace std::chrono_literals;

// The viewport in 8px tiles (160x144).
constexpr int kCols = 20;
constexpr int kRows = 18;

// The demo's input vocabulary: pause/resume the fracture fade and the background drift.
enum class Action : std::uint8_t { ToggleFade, ToggleDrift };

// Atlas tile indices (the horizontal strip in wall_atlas.png, in load order).
constexpr std::uint16_t kBrickA    = 0;
constexpr std::uint16_t kBrickB    = 1;
constexpr std::uint16_t kHole      = 2;  // all palette index 0 -> alpha-0 hole through the wall palette
constexpr std::uint16_t kWeathered = 3;  // body is the partial-alpha "weathered" index
constexpr std::uint16_t kBg        = 4;  // a BG_A/BG_B checker the background layer tiles + scrolls
constexpr std::uint16_t kCrack1    = 5;
constexpr std::uint16_t kCrack2    = 6;

// Wall cells overridden away from plain brick: gaps reveal the background, weathered bricks blend it.
constexpr std::array<std::pair<int, int>, 7> kHoleCells{{
    {3, 4}, {4, 4}, {3, 5}, {15, 6}, {16, 6}, {6, 12}, {14, 13}}};
constexpr std::array<std::pair<int, int>, 6> kWeatheredCells{{
    {9, 3}, {10, 3}, {2, 9}, {17, 10}, {8, 14}, {11, 15}}};

bool isIn(std::span<const std::pair<int, int>> cells, int tx, int ty) {
    for (const auto& [cx, cy] : cells) {
        if (cx == tx && cy == ty) return true;
    }
    return false;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Wall Cracks Demo"},
        .window = {.title = "Polyrhythm — cracked-wall demo (palette alpha)"}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ToggleFade,  {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::ToggleDrift, {SDL_SCANCODE_Z, PadButton::FaceEast}},
    };
    platform.actions(map);

    // The tile art (indices only) + three palette IMAGES carrying the colors and their alpha. The
    // palette images are 16-bit RGBA PNGs; loadPaletteImage decodes and slices them one pixel per entry.
    AtlasId   atlas{};
    PaletteId wallPal{};
    PaletteId bgPal{};
    PaletteId crackPal{};
    try {
        atlas    = renderer.loadAtlas("examples/wall_cracks_demo/assets/wall_atlas.png",
                                      AssetDimensions::GameBoy8x8, ContentKind::Tileset).atlasId;
        wallPal  = renderer.loadPaletteImage("examples/wall_cracks_demo/assets/wall_palette.png");
        bgPal    = renderer.loadPaletteImage("examples/wall_cracks_demo/assets/bg_palette.png");
        crackPal = renderer.loadPaletteImage("examples/wall_cracks_demo/assets/crack_palette.png");
    } catch (const std::exception& e) {
        std::printf("cracked-wall demo: could not load an asset: %s\n", e.what());
        return 1;
    }
    std::printf("cracked-wall demo — loaded wall_atlas.png + three 16-bit RGBA palette images "
                "(wall / bg / crack). The holes are the palette entries' own alpha.\n");

    // The background: a checker the layer tiles and scrolls. Built once; only its scroll changes.
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kCols) * kRows);
    for (auto& c : bgCells) c = TileCell{.atlas = atlas, .tile = kBg, .palette = bgPal};

    // The wall: running-bond two-tone bricks, with a few cells holed (gap) or weathered. Built once.
    std::vector<TileCell> wallCells(static_cast<std::size_t>(kCols) * kRows);
    for (int ty = 0; ty < kRows; ++ty) {
        for (int tx = 0; tx < kCols; ++tx) {
            std::uint16_t tile = ((tx + ty) & 1) ? kBrickA : kBrickB;
            if (isIn(kHoleCells, tx, ty))           tile = kHole;
            else if (isIn(kWeatheredCells, tx, ty)) tile = kWeathered;
            wallCells[static_cast<std::size_t>(ty) * kCols + tx] =
                TileCell{.atlas = atlas, .tile = tile, .palette = wallPal};
        }
    }

    // The fracture: a column of 8x8 crack sprites down the wall's center, alternating the two crack
    // tiles so the core flows from one into the next. Built once; the layer's alpha is the tween sink.
    std::vector<Sprite> crackSprites;
    static const std::vector<std::string> crackKeys =
        [] { std::vector<std::string> v; for (int k = 0; k < 64; ++k) v.push_back("crack" + std::to_string(k)); return v; }();
    for (int y = 28; y <= 116; y += 8) {
        const std::uint16_t tile = ((y / 8) & 1) ? kCrack1 : kCrack2;
        crackSprites.push_back(Sprite{.key = crackKeys[crackSprites.size()],
                                      .x = 76, .y = y, .atlas = atlas, .tile = tile, .palette = crackPal});
    }

    // The fracture fade: alpha 0 -> 1 over 4s, then back over 4s, forever — a slow eased yoyo.
    const Tween<float> fadeTween =
        Tween<float>::of(0.0f, 1.0f, 4s, Easing::InOutSine).then(0.0f, 4s, Easing::InOutSine);
    TweenPlayer<float> fadePlayer{.tween = &fadeTween};

    int  driftTicks = 0;
    bool driftOn    = true;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::ToggleFade)) {
            fadePlayer.playing ? fadePlayer.pause() : fadePlayer.play();
            std::printf("[dev] fracture fade: %s\n", fadePlayer.playing ? "playing" : "paused");
        }
        if (in.justPressed(Action::ToggleDrift)) {
            driftOn = !driftOn;
            std::printf("[dev] background drift: %s\n", driftOn ? "on" : "off");
        }
        if (driftOn) ++driftTicks;
        fadePlayer.advance(PlaybackMode::loopIndefinitely());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        const int drift = driftTicks / 6;  // ~10 px/s gentle same-direction drift

        // z=0 — the drifting background the holes reveal.
        DrawLayer bg{.key = "background"};
        bg.z       = 0;
        bg.size    = PixelSize{160, 144};
        bg.scroll  = LayerScroll{drift, drift / 3};
        bg.content = TileContent{.widthInTiles = kCols, .heightInTiles = kRows,
                                 .cells = std::span<const TileCell>(bgCells), .wrap = TileWrap::Repeat};
        frame.layers.push_back(std::move(bg));

        // z=10 — the wall; its gap cells discard (background shows through), weathered cells blend it.
        DrawLayer wall{.key = "wall"};
        wall.z       = 10;
        wall.size    = PixelSize{160, 144};
        wall.content = TileContent{.widthInTiles = kCols, .heightInTiles = kRows,
                                   .cells = std::span<const TileCell>(wallCells), .wrap = TileWrap::Blank};
        frame.layers.push_back(std::move(wall));

        // z=20 — the fracture sprites; the layer's alpha is the tween, so the crack eases in and out.
        DrawLayer fracture{.key = "fracture"};
        fracture.z       = 20;
        fracture.size    = PixelSize{160, 144};
        fracture.alpha   = fadePlayer.value();
        fracture.content = SpriteContent{.sprites = std::span<const Sprite>(crackSprites)};
        frame.layers.push_back(std::move(fracture));

        renderer.renderFrame(frame);
    });

    std::printf("  a brick wall over a drifting background: gaps and weathered bricks reveal it through "
                "the palette's alpha, and a fracture eases in. A = pause/resume the fade, B = the drift. "
                "Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
