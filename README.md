# Polyrhythm

A platform for building, porting, and extending retro games and applications built on the
tile-based, indexed-color (CLUT) paradigm — revive and enhance existing titles without the
original source. Native, cross-platform declarative rendering, virtualization, and co-execution
for retro software (Game Boy, SNES, and beyond).

It covers the **8-bit / 16-bit, tile-based** idiom — the Game Boy / Game Boy Color / NES / SNES /
Genesis / Master System family, and original games made in that style — and supplies the generic
infrastructure such a game needs: a fixed-step run loop, a window/GPU boundary, an `SDL_GPU` render
pipeline with layered compositing, a system-agnostic VM for the narrow set of routines that must
run as original hardware code (RNG, audio driver), an audio chain, and persistent storage for saves
and player files. Each consuming game supplies its own logic, data, and assets. In code the
namespace and include paths are `retropp`: `#include "retropp/vm.h"`.

**Every surface is console-parameterized.** The viewport, palette, timing, and input surfaces
ship presets across the whole console family (`ViewportResolution::Snes`, `PaletteSize::Genesis`,
`TickPeriodNs::Hz60`, …) and accept arbitrary values; the VM selects its core per target system.
The defaults are Game-Boy-flavoured; they are defaults, not constraints.

Out of the box, with no enhancements enabled, Polyrhythm reproduces the consuming game's original
behavior faithfully. Enhancements (output scaling, world zoom, audio packs, display filters) are
opt-in and off by default.

**The platform is lean: the library is ~1.5 MB.** Everything it depends on — SDL3, the image and audio
decoders, any VM cores a game uses — links statically alongside it, which puts the floor for
a shipped binary at roughly 3.2 MB before a game adds its own code and content, with nothing to
install beside it. Release builds dead-strip at link, so those are shipped sizes rather than pre-trim
ones. (Measured on macOS arm64.)

## Status

Active development. The core is in place and exercised end to end by a real consumer:

- **Run loop & timing** — fixed-step simulation with sim/render decoupling, frame
  interpolation across the ticks a frame actually ran, a per-layer declaration of how often a
  world advances so a simulation running on a divider eases across the ticks it takes, and a
  host-selected timing profile.
- **Platform & input** — SDL3 window + `SDL_GPU` device + event pump, native fullscreen,
  high-DPI, and an action-based input surface: a game declares its own actions, binds each to
  any number of sources, and the input surface resolves them per controller family.
- **Rendering** — an `SDL_GPU` pipeline with an internal viewport, a window-filling
  integer/letterbox blit (nearest/bilinear), and a layered compositor: arbitrary Z-sorted
  tile and sprite layers, indexed atlases with runtime palettes, per-layer and per-sprite
  alpha, geometric transforms (scale/rotate/skew/perspective), tilemap wrap modes, PNG image
  ingestion, blend modes, frame-level colour modifier/blend, and region-confined effects with
  analytic and mask-based shapes. Shaders are generated at build time per platform — no
  runtime shader compiler, no committed bytecode.
- **Effects** — a built-in screen-space library (ripple, swirl, row displacement, colour fill,
  gleam, saturation, transparency, stencil, glow, bloom) applied uniformly at frame, layer,
  region and sprite scope, plus a game-registered custom shader stage that can join the
  renderer's own emission grammar and obtain a blur by declaration rather than by gathering.
- **Motion** — value tweening, curve primitives with arc-length parameterisation, and sprite
  paths with sequencing and interrupt policies.
- **Audio** — a mixed multi-voice chain with per-type levels, chiptune routines and PCM audio
  packs, production off the game's thread with a thread per sounding machine, and hosting for a
  game's own resident sound driver as a long-lived addressable machine driven by the player's own
  verbs.
- **VM host** — a system-agnostic VM that runs surgically-extracted original-hardware routines
  (authored as `.asm`, assembled in-process) as ordinary typed C++ functions on the Game Boy
  family. Two cores back it — SameBoy for the Game Boy / Game Boy Color and Snaggletooth for the
  SNES — each behind the same seam, and a game's binary carries only the cores it names.
- **Co-execution** — a game hosts a whole cartridge and **runs it**: the image boots as the
  hardware would boot it and runs continuously on its own thread, at the platform's own speed or
  any fraction or multiple of it, adjustable live, with the places the game declares inside it
  readable and writable while it runs.
  **Guest escapes** hand control the other way — native code runs at declared places in the
  cartridge's own program, either observing a spot as the guest reaches it or replacing one of
  the cartridge's routines outright with a native function that answers every caller in the
  routine's own calling convention. **Access watches** do the same for its memory: a declared
  place's reads and writes are decided by native code as they happen, which lets a read answer
  with a byte the cartridge does not contain and a write be refused or replaced.
  And native code **calls back into the guest**: a routine the
  cartridge already has can be bound where it sits and called like a typed C++ function, in the
  guest's own context and to any depth, so a native replacement can build its answer out of the
  cartridge's own routines — or a parked machine's own decoders can be run to reach content the
  game never played its way to. The image itself is never modified. A hosted SNES cartridge boots,
  runs on either clock, draws, sounds, takes both controller ports and keeps its battery save
  through the same verbs; the naming, escape, watch and call verbs are the Game Boy family's.
- **A hosted machine's video** — ask a machine for video and the frames it finishes become a
  layer's content, composited by z among native tile and sprite layers like anything else on
  screen: a game's own art over a running cartridge's picture, a transform or a screen-space
  effect on it, several machines on one screen at once. Either clock drives it — a machine
  advanced by the game's tick answers from the tick boundary, one running on a thread of its own
  hands each finished frame across — and what a game reads is always a completed frame, never one
  being drawn. What a console core provides is a completion, a buffer, dimensions, a pixel layout
  and, for an interlaced picture, which field this is; it is never asked how fast it runs, which is
  what lets a core of another resolution implement the same seam. A picture of any size fits the
  slot a layer gives it, and the fields of an interlaced picture are woven when the game asks. Off
  unless a machine is asked for it, because a raster costs cycles.
- **Persistence** — versioned, atomically-written save documents; a separate store for a
  player's other files; and registration for arbitrary byte assets that are never interpreted.

Planned: positional voices.

For the full per-subsystem surface and current status, see the
[developer guide](docs/guide/README.md).

## How it's consumed

Each consuming game attaches this repository as a git submodule in its own tree
(e.g. `<game>/engine/`). The game's build brings it in with `add_subdirectory(engine)` and links
the target. Polyrhythm ships as source — there is no precompiled-binary distribution. Fork only
if you need to carry your own changes; the submodule points at your fork instead, and nothing
else differs.

The reference consumer is [Kirpich](https://github.com/etroimcasso/Kirpich), a native Game Boy
(DMG) port, which exercises the v1 API surface end to end.

## Build

Requirements:

- CMake 3.28+
- A C++20 compiler: GCC 13+, Clang 16+, or MSVC 19.38+ (Visual Studio 2022 17.8+)
- Git (the SameBoy and Snaggletooth cores are submodules)

Clone with submodules, then configure and build:

```sh
git clone --recurse-submodules git@github.com:RetroPlusPlus/Polyrhythm.git
cd Polyrhythm
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Building Polyrhythm as the top-level project (above) enables its own tests. When it is consumed
via `add_subdirectory`, those tests are off by default; a consumer that wants the test tooling
links the `retropp::testkit` target.

### Targets

| Target | Alias | Purpose |
|---|---|---|
| `retroppengine` | `retropp::engine` | The shipped library; consumers link this. |
| `retropp-testkit` | `retropp::testkit` | Test-tooling library. Linked only into test executables, never into a shipped game binary. |

## Dependencies

- **[SDL3](https://github.com/libsdl-org/SDL)** — vendored as a submodule at
  `third_party/sdl/`, built via `add_subdirectory`. The platform layer (window, GPU
  device, event pump, input) targets `SDL_GPU`. Zlib-licensed; pulled with
  `--recurse-submodules`.
- **[SameBoy](https://github.com/LIJI32/SameBoy)** — vendored as a submodule at
  `third_party/sameboy/`, pinned to v1.0.3. The reference Game Boy / Game Boy Color core;
  its emulation core compiles in to back the runtime VM. MIT-licensed; pulled with
  `--recurse-submodules`.
- **[Snaggletooth](https://github.com/etroimcasso/Snaggletooth)** — vendored as a submodule at
  `third_party/snaggletooth/`, pinned by commit. A clean-room SNES implementation; its machine
  compiles in to back the SNES core, and its two assemblers are linked only by the tests and the
  SNES examples, which assemble their cartridges in process. MIT-licensed; pulled with
  `--recurse-submodules`.
- **[lodepng](https://github.com/lvandeve/lodepng)** — vendored in-tree at
  `third_party/lodepng/` (pinned, zlib/MIT), compiled as a small static lib and linked
  privately. Decodes indexed/grayscale PNGs for the image-ingestion path; no symbol
  reaches a public header.
- **[dr_libs](https://github.com/mackron/dr_libs)** (`dr_wav`) and
  **[stb](https://github.com/nothings/stb)** (`stb_vorbis`) — vendored in-tree at
  `third_party/dr_libs/` and `third_party/stb/`, compiled into a small static lib and linked
  privately for the audio-pack decoders. Each is offered as a choice of public domain or a
  permissive licence (MIT-0 and MIT respectively).
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time
  only when Polyrhythm's own tests are built.

Every one of them links **statically** into the consuming game's binary — the ~3.2 MB floor noted
above. A game ships as one file, with nothing to install beside it.

## License

Polyrhythm is source-available commercial software, licensed two ways: **PolyForm Noncommercial
1.0.0** for noncommercial use, and a separate **commercial license** for any commercial use. See
[`LICENSING.md`](LICENSING.md) and [`LICENSE`](LICENSE). Vendored dependencies retain their own
licenses.
