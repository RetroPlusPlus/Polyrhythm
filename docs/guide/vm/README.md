# Conductor — one host, a core per console

**Conductor** is the platform's VM layer. One host, `Vm`, owns one machine of one console, and every
verb a program issues goes through the same surface whichever console the machine is. What the host
does with a machine is two capabilities, each with its own page:

- **[vm-and-routines.md](vm-and-routines.md)** — the routine surface: a routine registered once and
  called as a typed C++ function, the free-running clock between calls, the `Throttle` pacing seam, and
  hosting a resident sound driver.
- **[co-execution.md](co-execution.md)** — a whole cartridge running inside your game: hosting it,
  naming places in it, running it on either clock, its video, sound and input, escaping into your code,
  deciding its reads and writes, and calling its own routines.

What a console *is* behind that surface — its core, its clock, its vocabulary header, its save, what its
core answers and what it refuses, and every fact whose form comes from its hardware — is that console's
page:

- **[gameboy.md](gameboy.md)** — the Game Boy and the Game Boy Color.
- **[snes.md](snes.md)** — the SNES.

This page is the map between the two: how the host and a core fit together, which verbs every console
shares, and which depend on the core behind them.

```cpp
#include "retropp/vm.h"     // Vm, VMPlatform, VmConfig, Vm::GB / Vm::GBC / Vm::SNES
#include "retropp/gb.h"     // the Game Boy family's vocabulary
#include "retropp/snes.h"   // the SNES's vocabulary
#include "retropp/isa.h"    // Isa, the instruction set a routine's bytes are written for
```

## Contents

- [The host and the seam](#the-host-and-the-seam)
- [Selecting a console](#selecting-a-console)
- [A machine on your tick runs at its own rate](#a-machine-on-your-tick-runs-at-its-own-rate)
- [What every console shares](#what-every-console-shares)
- [What depends on the core](#what-depends-on-the-core)
- [Where the files are](#where-the-files-are)

## The host and the seam

`Vm` (`include/retropp/vm.h`) is system-agnostic. It owns one backend — a `VmBackend`
(`src/vm/vm_backend.h`, internal) selected by the `VMPlatform` it is constructed for — and drives it
through that one interface; it knows nothing about a CPU, a memory map or the library behind a core.
Each console is a folder under `src/vm/` holding its backend, and this directory mirrors that layout:
`src/vm/gameboy/` is [gameboy.md](gameboy.md), `src/vm/snes/` is [snes.md](snes.md).

The seam carries everything the host needs to run a machine without knowing which one it is:

| What the seam carries | What the host does with it |
|---|---|
| the machine's own clock — its rate as an exact ratio and the cycles one of its frames takes | every pacing decision, on both clocks; the host reads a rate from nowhere else |
| a routine arena, an assembler and the call lifecycle (place, marshal, run, read back) | `uploadRoutine` / `registerRoutine` / `assemble` and a `Routine<Sig>` call |
| loading an image, booting it, running it for a cycle budget | `hostRom`, `run`, `speed`, `stop`, `advanceTick` |
| whether a place is reachable, and reading and writing one | `registerRegions`, `read`, `write` |
| a per-instruction hook and a per-access hook | `registerEscapes`, `registerWatches`, and the two tables |
| a completed frame, with its dimensions and pixel layout | `video()` as a layer's `RasterContent` |
| the sound chip's frames, converted to the rate asked for | `enableAudio` |
| a button word | `buttons` |
| whether the core keeps battery-backed data, and its file extension | `VmConfig{.key}` and `batterySave` |

A core answers each of those or refuses it at the verb. A refusal is an exception naming what the core
does instead, thrown where the program asked — never a declaration accepted and silently ignored.
Which verbs each shipped core answers is the table under [What depends on the core](#what-depends-on-the-core).

## Selecting a console

```cpp
enum class VMPlatform { GameBoy, GameBoyColor, Snes, Nes, Genesis, MasterSystem };
```

Three spellings construct a machine, and they are the same machine:

```cpp
Vm::GBC  gbc;                                              // platform and timing pre-bound
Vm::SNES snes{VmConfig{.key = "adventure", .video = true}}; // the same, with its outputs declared
Vm       any{VMPlatform::Snes, VmConfig{.video = true}};    // the platform named at construction
```

`Vm::GB`, `Vm::GBC` and `Vm::SNES` are `Vm`s with the console fixed at the type — they add no state, so
one goes anywhere a `Vm` is expected. `vm.platform()` reports the console back. Constructing a `Vm` for
an enumerator whose core is not built throws `std::runtime_error` at construction; `Nes`, `Genesis` and
`MasterSystem` are enumerated so a program can name one.

Each console maps to an instruction-set architecture, `Isa` (`retropp/isa.h`), through
`isaFor(VMPlatform)`: the Game Boy and the Game Boy Color both run `Isa::Sm83`, the SNES `Isa::Wdc65816`.
The ISA is the compatibility unit for a routine's bytes and for a chiptune driver — either runs on any
machine of the same ISA, whichever console it is.

## A machine on your tick runs at its own rate

Every machine keeps its own clock, read from its core: a Game Boy runs at 4'194'304 Hz and a frame of it
is 70'224 cycles; a SNES cartridge runs at the rate its region names. `speed(num, den)` is what scales a
machine, and nothing else does.

A machine advanced on your tick — `run(Vm::Advance::OnTick)` then `advanceTick()` once per tick —
spends **one frame of its own clock per call**. The run loop's tick period is `EngineConfig::timing`,
and its default is the Game Boy Color cadence, so a run loop left at the default ticks a SNES machine
one SNES frame per Game Boy frame: the machine runs at the loop's frame rate rather than its own. Set the
profile to the console's, as the SNES player does:

```cpp
const EngineConfig config{
    .identity     = {.organization = "Retro++", .application = "SnesPlayer"},
    .window       = {.title = "Polyrhythm — SNES player (tick-advanced | free-running)"},
    .viewport     = ViewportResolution{kViewW, kViewH},
    .timing       = TimingProfile::Snes,  // The run loop needs the SNES timing profile to tick-advance correctly
    .enhancements = {.windowScale = kScale}};
```

A loop that ticks at a period that is not the machine's — one console hosted while the loop runs at
another's rate — passes the period it is actually running, `advanceTick(loop.tickPeriod())`, and the
machine spends what that period is worth at its own rate, carrying the fraction of a cycle left over
(see [vm-and-routines.md](vm-and-routines.md#a-machine-running-at-a-cadence-that-is-not-its-own)).
A machine on a thread of its own (`Vm::Advance::Continuously`) holds the hardware's cadence whatever
the loop does.

## What every console shares

The verbs below run on every shipped core, on the same terms. Each is documented once, on the page
that owns it; a console page adds only what its hardware makes different.

| Verb | Page |
|---|---|
| `hostRom(bytes)` — an image on the machine, every byte addressable | [co-execution.md § Hosting a cartridge](co-execution.md#hosting-a-cartridge) |
| `run(Advance)` · `speed(num, den)` · `stop()` · `reset()` — boot, pace, park, fresh boot | [co-execution.md § Running it](co-execution.md#running-it) |
| `advanceTick()` — the step, on your tick | [co-execution.md § Advancing it on your own tick instead](co-execution.md#advancing-it-on-your-own-tick-instead) |
| `video(on)` · `video()` · `VmConfig{.video}` — the picture as a layer's content | [co-execution.md § Video](co-execution.md#video-showing-its-picture) |
| `enableAudio(rate, onSample)` — the sound chip's frames at the rate you name | [co-execution.md § Sound](co-execution.md#sound-hearing-it) |
| `buttons(held)` — the pad, a level landing at the next step boundary | [co-execution.md § Input](co-execution.md#input-playing-it) |
| `VmConfig{.key}` · `batterySave(name)` — the guest's battery-backed data, kept for the player | [vm-and-routines.md § Keeping what the guest writes](vm-and-routines.md#keeping-what-the-guest-writes) |
| the threading rule — a running machine belongs to the thread stepping it | [co-execution.md § While it runs](co-execution.md#while-it-runs-one-thread-owns-the-machine) |

## What depends on the core

Three capabilities depend on what a core offers rather than on the surface: escapes need a
per-instruction hook, watches a per-access one, and a routine surface an arena to place code in and a
register file to bind. A core without one refuses at the declaration rather than accepting it, and the
exception names what the core does instead.

| Capability | Game Boy / Game Boy Color | SNES |
|---|---|---|
| host, boot, run, pace, park a cartridge | answers | answers |
| video, sound, both as above | answers | answers |
| input — the pad | one pad, `gb::held` | two ports, `snes::Ports` |
| battery-backed save data | `.sav` | `.srm` |
| a routine registered from bytes or `.asm` and called for a value | answers | refuses |
| `advanceClock` — the free-running clock between calls | answers | refuses |
| naming places, `read` / `write` by address | answers | refuses |
| escapes, watches, `bindRoutine` | answers | refuses |
| hosting a resident sound driver | answers | refuses |

The exception each refusal throws, and the message, is on the console's page:
[gameboy.md](gameboy.md#two-models-one-core), [snes.md](snes.md#what-the-core-answers-and-what-it-refuses).

## Where the files are

| What | Where |
|---|---|
| The public surface | `include/retropp/vm.h`, `guest_escape.h`, `guest_watch.h`, `guest_buttons.h`, `memory_region.h`, `raster_content.h`, `isa.h` |
| A console's vocabulary | `include/retropp/gb.h`, `include/retropp/snes.h` |
| The host: declarations, validation, the tables, the two clocks | `src/vm/vm.cpp`, `src/vm/vm_runner.cpp`, `src/vm/run_governor.h` |
| What a core provides | `src/vm/vm_backend.h` |
| The Game Boy family's core | `src/vm/gameboy/` — [gameboy.md](gameboy.md) |
| The SNES's core | `src/vm/snes/` — [snes.md](snes.md) |
| The save writer every keyed machine shares | `src/vm/save_writer.cpp` |

Adding a console is a folder under `src/vm/` with its backend, its `VMPlatform` enumerator, its core
hook and its pre-bound `Vm::` type, and a `<console>.h` vocabulary header beside `gb.h` and `snes.h`;
the public `vm.h` surface does not change
([vm-and-routines.md § Where to change things](vm-and-routines.md#where-to-change-things)). Its page
lands in this directory.
