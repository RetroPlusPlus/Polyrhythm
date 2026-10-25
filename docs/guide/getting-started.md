# Getting started

This page takes you from a fresh clone to a window with a scrolling, full-color tile background you
can steer with the d-pad — the smallest complete *interactive* program that uses the platform for real.
It is about 60 lines, uses no asset files, and every line is explained below. The full source is
[`examples/controller_scrolling.cpp`](../../examples/controller_scrolling.cpp); it builds as the
`retropp-controller-scrolling` target, so you can run exactly what you read here.

> Want the absolute smallest thing first? [`examples/hello_world.cpp`](../../examples/hello_world.cpp)
> (the `retropp-hello-world` target) just opens a window and shows "Hello, world!" — load one image,
> draw one sprite. This page is the next step up: input and a moving world.

New to how the pieces fit together? Skim [concepts.md](concepts.md) first for the mental model — but
you can also just follow along here and pick it up as you go.

## Contents

- [1. Build the platform](#1-build-the-platform)
- [2. The whole program](#2-the-whole-program)
- [3. What each part does](#3-what-each-part-does)
- [4. Build and run it](#4-build-and-run-it)
- [5. Where to go next](#5-where-to-go-next)

## 1. Build the platform

```sh
git clone --recurse-submodules <repo-url>
cd Polyrhythm
cmake -S . -B build
cmake --build build
```

`--recurse-submodules` matters — SDL3 and the other vendored dependencies come in as submodules. You
also need CMake 3.28+, a C++20 compiler (GCC 13+, Clang 16+, or MSVC 19.38+), and a build-time shader
toolchain. The full requirements list and what each dependency is for are in
[build-and-consume.md](build-and-consume.md). When the build finishes you have the platform library,
its tests, and several runnable examples — including `retropp-hello-world` (the tiniest — just shows
"Hello, world!"), `retropp-controller-scrolling` (this page), `retropp-beach-demo` (a per-layer-effects
beach scene), and `retropp-layer-transparency-demo` (index-hole transparency).

## 2. The whole program

This is the complete thing — copy it, or just read it and run the committed
[`examples/controller_scrolling.cpp`](../../examples/controller_scrolling.cpp).

```cpp
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

// The game's input vocabulary — any enum; the platform never sees the names.
enum class Action : std::uint8_t { Up, Down, Left, Right };

int main() {
    // 1. Configure. A default EngineConfig is the faithful Game Boy Color baseline; the identity
    //    is the one REQUIRED field (setActive refuses an anonymous config — every program declares
    //    who it is). Set it active ONCE; the bare core objects below inherit it.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Controller Scrolling"},
        .window   = {.title = "Polyrhythm — controller scrolling"}};
    EngineConfig::setActive(config);

    // 2. The four core objects — bare ctors inherit the active config.
    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.window()};
    RunLoop     loop{clock};

    // 2b. Bind the actions: one preset covers arrows + WASD + d-pad. The map is a value the game
    //     owns — hand it to the platform; resubmit an edited copy to rebind at any time.
    ActionMap map = presets::directional(Action::Up, Action::Down, Action::Left, Action::Right);
    platform.actions(map);

    // 3. Upload indexed art + a palette.
    constexpr int kTile = 8, kCols = 2;
    std::array<std::uint8_t, kTile * kTile * kCols> atlas{};
    for (int y = 0; y < kTile; ++y) {
        for (int x = 0; x < kTile; ++x) {
            const bool edge = (x == 0 || y == 0 || x == kTile - 1 || y == kTile - 1);
            atlas[y * (kTile * kCols) + x]           = 1;
            atlas[y * (kTile * kCols) + (kTile + x)] = edge ? 3 : 1;
        }
    }
    const AtlasId atlasId = renderer.uploadAtlas(atlas.data(), kTile * kCols, kTile).atlasId;

    const std::array<Rgba8, 4> colors{{{20, 20, 30}, {70, 110, 180}, {0, 0, 0}, {200, 230, 255}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colors));

    // 4. A tilemap. Each cell names its own sheet + palette.
    constexpr int kMapW = 32, kMapH = 32;
    std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x) {
            TileCell& c = cells[static_cast<std::size_t>(y) * kMapW + x];
            c.atlas   = atlasId;
            c.tile    = static_cast<std::uint16_t>((x + y) % 2);
            c.palette = pal;
        }

    // 5. One layer, built once.
    FrameDrawState frame;
    frame.layers.push_back(DrawLayer{.key = "background"});
    DrawLayer& bg = frame.layers[0];
    bg.z       = 0;
    bg.size    = PixelSize{config.viewport.width, config.viewport.height};
    bg.content = TileContent{.widthInTiles  = kMapW,
                             .heightInTiles = kMapH,
                             .cells         = std::span<const TileCell>(cells)};

    // 6. Wire the loop.
    int camX = 0, camY = 0;
    loop.simTick([&](const InputState& in) {
        if (in.isHeld(Action::Right)) ++camX;
        if (in.isHeld(Action::Left))  --camX;
        if (in.isHeld(Action::Down))  ++camY;
        if (in.isHeld(Action::Up))    --camY;
    });
    loop.renderLoop([&]() {
        frame.layers[0].scroll = LayerScroll{camX, camY};
        renderer.renderFrame(frame);
    });

    // 7. Run.
    WindowedHost{loop, platform}.run();
    return 0;
}
```

## 3. What each part does

**Step 1 — configure, then set active.** [`EngineConfig`](platform-and-windowing.md) is one value
bundle for startup: window, internal viewport, timing, and the application identity.
Every field defaults to the faithful Game Boy Color baseline and you override only what you mean to —
with one exception: **`identity` is required.** `setActive` throws when either identity field is
empty, the same way no platform lets a project exist without one; the identity also names the
per-user save directory ([persistence.md](persistence.md)).
`EngineConfig::setActive(config)` then makes it the active config *once* — it stores the config and
fans its fields out into per-type defaults so the bare core objects in step 2 inherit them, instead of
you threading `config.viewport` / `config.timing` into every constructor. (You still *can* thread them
explicitly — `RunLoop{clock, config.timing}` — and that overrides the active default; `setActive` is
the recommended minimal path.)

**Step 2 — the four core objects.** This is the whole architecture in four lines (see
[concepts.md](concepts.md)):

- `SteadyClock` — the monotonic time source the loop reads. (Tests swap in a fake clock; you use the
  real one.)
- `SdlPlatform` — owns the OS window, the GPU device, and input. A bare `SdlPlatform` reads the
  active config (window); the game hands it the action bindings (`actions`).
- `Renderer` — draws. It takes the platform's live `device()` and `window()` and inherits the active
  viewport. Drawing is the renderer's job; the platform owns the window/device.
- `RunLoop` — the fixed-step scheduler. It takes the clock and inherits the active timing profile.

**Step 3 — upload art.** The platform's color model is **indexed**: an atlas holds one palette *index*
per pixel, and color comes from a palette chosen at render time — never baked into the art (full
detail in [tiles-and-color.md](tiles-and-color.md)). Here we build a tiny 16×8 atlas of two 8×8
tiles by hand: tile 0 is solid index 1, tile 1 has a bright (index 3) border. `uploadPalette` gives
those indices colors; `uploadAtlas` uploads the index plane. Both uploads are **amortized** — you do
them once, and the per-frame draw state just references the returned `AtlasId` / `PaletteId` handles.
A real game loads art from PNG instead of building it in code (see
[images-and-transparency.md](images-and-transparency.md)); hand-building keeps this example
dependency-free.

**Step 4 — a tilemap.** A 32×32 grid of `TileCell`s, each naming which atlas tile it shows. We
checkerboard tiles 0 and 1. The `cells` vector must stay alive as long as the layer references it —
it's declared in `main`, so it lives for the whole program.

**Step 5 — one layer, built once.** A [`FrameDrawState`](draw-state.md) is the whole frame you hand
the renderer: a stack of layers sorted by `z`. We make one tile layer pointing at our atlas, palette
set, and tilemap. Note we build it **once**, before the loop — the only thing that changes each frame
is its scroll, so there's no reason to rebuild it. (You *can* rebuild the frame every frame instead;
both styles are fine — see [the retained-vs-rebuilt recipe](how-to.md#retained-vs-rebuilt-frame).)

**Step 6 — wire the loop.** You give the loop two callbacks:

- **Tick** is one logical step of your game. It receives an [`InputState`](input.md) (held actions +
  press/release edges) and updates game state — here, a camera moved by the held d-pad. Ticks run at
  the fixed timing-profile rate, so your logic is deterministic and frame-rate-independent.
- **Render** draws the current state. It runs once per displayed frame and calls
  `renderer.renderFrame(frame)`. We set the layer's scroll from the camera and submit. Motion between
  ticks is smoothed automatically — the renderer interpolates each keyed object toward its latest
  tick state (see [run-loop-and-timing.md](run-loop-and-timing.md)); the callback just submits.

**Step 7 — run.** [`WindowedHost`](platform-and-windowing.md) is the driver: each iteration it pumps
OS events, pushes the current held buttons into the loop, and advances the simulation (which calls
your render). It returns when the window's close button is pressed.

## 4. Build and run it

The example builds with the platform (top-level builds turn examples on by default):

```sh
cmake --build build --target retropp-controller-scrolling
./build/retropp-controller-scrolling        # path varies by generator/platform
```

A window opens showing a blue-and-white checkerboard. Hold the arrow keys (the default keyboard
mapping) or a gamepad d-pad and the field scrolls. Close the window to quit.

## 5. Where to go next

- **Understand the architecture** you just used — [concepts.md](concepts.md).
- **Do specific things** — scroll, animate a sprite, make a character walk behind scenery, add a HUD,
  fade the screen, load a PNG: [how-to.md](how-to.md).
- **The frame you submit, in depth** — layers, sprites, z-ordering, modifiers: [draw-state.md](draw-state.md).
- **Color** — indexed atlases, palettes, recoloring without new art: [tiles-and-color.md](tiles-and-color.md).
- **Richer running examples** — the `retropp-beach-demo` target
  ([`examples/beach_demo.cpp`](../../examples/beach_demo.cpp)) composites a beach scene with a wavy
  ocean (per-layer screen-space effects) beating over a rock, and the `retropp-layer-transparency-demo`
  target ([`examples/layer_transparency_demo.cpp`](../../examples/layer_transparency_demo.cpp)) loads a
  real PNG and shows per-source index-hole transparency. Both rebuild their frame each tick (the
  immediate-mode style).
