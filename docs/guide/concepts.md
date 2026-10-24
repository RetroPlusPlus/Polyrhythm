# Core concepts

How the platform's pieces fit together, the one design idea behind all of them, and the vocabulary the
rest of the guide uses. Read this once and the per-subsystem pages will read faster. If you want code
first, do [getting-started.md](getting-started.md) and come back.

## Contents

- [The one big idea: a frame is data, computed whole](#the-one-big-idea-a-frame-is-data-computed-whole)
- [The objects and how they fit](#the-objects-and-how-they-fit)
- [Sim and render are decoupled](#sim-and-render-are-decoupled)
- [How drawing is described](#how-drawing-is-described)
- [Every surface is console-parameterized](#every-surface-is-console-parameterized)
- [Glossary](#glossary)
- [Where to go next](#where-to-go-next)

## The one big idea: a frame is data, computed whole

The platform is a **modern reimplementation** of the 8-/16-bit tile-based idiom, not a hardware
emulator. It mirrors the *data model* of that era — tiles, palettes, layers, sprites — but never the
hardware *mechanism*. There are no hardware registers, no scanline interrupts, and no mid-frame pokes
anywhere in the public API.

Concretely, every frame your game **computes a complete description of what the screen should show**
— a `FrameDrawState` — from its own logical state, and hands that whole description to the renderer.
You never tell the platform "now change this register for the next scanline." You say "here is the
entire frame," as data, and the renderer draws it. Effects that old hardware achieved with timing
tricks (a status bar that doesn't scroll with the world, a day/night tint, a screen flash, parallax)
are all expressed as ordinary data in that description: more layers, different scroll values, a
whole-frame color grade. This is what keeps the platform portable and the API small.

A direct consequence: **out of the box the platform reproduces a game's original behavior faithfully.**
Enhancements (output scaling, world zoom, audio packs, display filters) are opt-in and off by default
— never something you have to opt *out* of.

## The objects and how they fit

Five objects do everything in a typical host. A minimal `main` constructs them, wires two callbacks,
and runs:

```
            ┌─────────────────────────────────────────────────────────┐
            │                      WindowedHost                        │
            │   each iteration:  pump OS events                        │
            │                    push held buttons → RunLoop           │
            │                    RunLoop.advance()                     │
            └───────────────┬─────────────────────────┬───────────────┘
                            │                          │
                   ┌────────▼────────┐        ┌────────▼────────┐
                   │     Platform    │        │     RunLoop     │
                   │  (SdlPlatform)  │        │  fixed-step:    │
                   │                 │        │   N× tick(input)│
                   │  window         │        │   render(alpha) │◄── Clock
                   │  GPU device ────┼───┐    └────────┬────────┘    (time)
                   │  input          │   │             │
                   └─────────────────┘   │             │ your render callback calls
                                         │             ▼
                                         │    ┌─────────────────┐
                                         └───►│     Renderer    │
                                  device+window   draws a        │
                                              │   FrameDrawState │
                                              │   → the window   │
                                              └─────────────────┘
```

- **`Platform`** (production impl `SdlPlatform`) — the host-OS boundary. Owns the window, the GPU
  device, and input. Everything OS-specific lives behind this one seam, so the rest of the platform
  never touches a live device directly. [platform-and-windowing.md](platform-and-windowing.md)
- **`Renderer`** — draws. It's handed the platform's live device + window and submits frames against
  them. It owns an internal viewport (a small fixed resolution, 160×144 by default) and scales that
  onto the window. Drawing is *its* job; the platform owns the window/device. [rendering.md](rendering.md)
- **`RunLoop`** — the fixed-step scheduler. Runs game logic at a constant rate regardless of how fast
  the display refreshes, then renders. [run-loop-and-timing.md](run-loop-and-timing.md)
- **`Clock`** (production impl `SteadyClock`) — the time source the loop reads. An injectable seam so
  tests can drive the loop with fake time.
- **`WindowedHost`** — the glue driver: pump events → push input → advance, until the window closes.
  It owns nothing; it just sequences the platform and the loop.

You provide two callbacks to the `RunLoop`:

- **tick(`InputState`)** — one logical step of your game (move things, run rules). Deterministic.
- **render()** — build/submit the `FrameDrawState` for the current state. (Optionally takes the
  between-tick `alpha` — see below.)

## Sim and render are decoupled

Game logic runs on a **fixed timestep** (the timing profile — Game Boy Color by default, 59.7275 Hz).
The display might refresh faster or slower or unevenly. The loop reconciles this: each real frame it
runs however many whole *ticks* of game logic have come due, then renders **once**. Because ticks are
fixed-rate, your game logic is frame-rate-independent and reproducible — the same inputs produce the
same result on a 60 Hz laptop and a 144 Hz monitor.

The platform eases each object between its last two ticks **automatically** (matched by `key`), so
motion is smooth even though logic steps discretely — with no game-side work: submit the latest
state each frame and the platform interpolates it.
[run-loop-and-timing.md](run-loop-and-timing.md) covers it.

## How drawing is described

The thing you submit each frame is a **`FrameDrawState`**: a stack of **layers**, plus optional
whole-frame color effects. Each **layer** is tiles *or* sprites at a depth (`z`), with its own scroll
and size. The compositor draws layers back-to-front by `z`. There are **no fixed roles** — the platform
has no "background" or "sprite" or "window" layer; a layer is just content at a depth, and *you*
decide what each one means. "The player walks behind a tree" is just the tree on a higher-`z` layer.

Color is **indexed**: art (an **atlas**) stores a palette *index* per pixel, and a **palette** turns
indices into actual colors at render time. The same art renders in any color scheme by pointing it
at a different palette — that's how recoloring, day/night, and palette animation work without new
art. [draw-state.md](draw-state.md) + [tiles-and-color.md](tiles-and-color.md).

You can rebuild the whole `FrameDrawState` every frame, or keep it and mutate only what changed — both
are supported and neither is "more correct." See
[the retained-vs-rebuilt recipe](how-to.md#retained-vs-rebuilt-frame).

## Every surface is console-parameterized

The defaults are Game-Boy-flavored, but every surface generalizes across the 8-/16-bit family
and accepts arbitrary values: viewport resolutions
(`ViewportResolution::Nes`, `Snes`, … or any `{w, h}`), palette sizes (`PaletteSize::Genesis`, … or any
count), and timing (`TickPeriodNs::Hz60`, … or any period). Input has no preset axis — a game
declares its own actions and bindings (see [input.md](input.md)).
The Game Boy presets are the proven defaults, not constraints.

## Glossary

| Term | Meaning |
|---|---|
| **tick** | One fixed-rate step of game logic. Runs at the timing profile's rate, independent of display refresh. |
| **frame / render** | One drawn image. The render callback runs once per displayed frame and may run between ticks. |
| **`alpha`** | Interpolation factor ∈ [0,1): how far between the last two ticks a render falls. The platform eases by it automatically; a game reads it only to own the blend itself. |
| **viewport** | The platform's internal render resolution (160×144 by default). Drawn small, then scaled to the window. |
| **atlas** | A sheet of indexed art — one palette *index* per pixel — uploaded once and referenced by `AtlasId`. |
| **palette** | A small table mapping indices → output colors (`Rgba8`), uploaded once and referenced by `PaletteId`. |
| **named sheet / palette** | Each tile and sprite carries its own `AtlasId` + `PaletteId` directly, so one layer mixes any number of sheets and palettes — there is no per-layer set or cap. |
| **layer** | One entry in the frame's stack: tiles or sprites, at a depth `z`, with its own scroll/size/alpha. |
| **`z`** | A layer's back-to-front sort key. Depth is `z` alone — there are no role-based layers. |
| **`key`** | A layer / sprite / region's REQUIRED reconciliation identity (an `ObjectKey`, e.g. `"HUD"`) — the stable name the renderer matches an object to its previous tick by, and interpolates from. Unique per frame, not a depth key; omitting it is a compile error. |
| **`FrameDrawState`** | The whole description of one frame: the layer stack + whole-frame color and screen-space effects. |
| **indexed color** | The faithful model: pixels are palette indices; color is applied from a selected palette at render time. |
| **immediate-mode / retained** | Two equally-valid ways to produce the frame: rebuild it each frame, or keep it and mutate what changed. |

## Where to go next

- [getting-started.md](getting-started.md) — the minimal program, if you skipped it.
- [how-to.md](how-to.md) — recipes for specific tasks.
- The per-subsystem pages in the [index](README.md) — the full public surface of each object above.
