# The SNES

`Vm::SNES` is a Super Nintendo: the Snaggletooth core behind one backend (`src/vm/snes/`), constructed
for `VMPlatform::Snes`, running `Isa::Wdc65816`. It hosts and runs a whole cartridge — booted, paced on
either clock, drawn, heard, played on both controller ports and its battery save kept — through the same
verbs as every console. This page is what the SNES is behind the surface: its clock, its vocabulary in
`snes.h`, its picture and sound, its save, what its core refuses and how, and its examples. The verbs
are on [co-execution.md](co-execution.md).

```cpp
#include "retropp/vm.h"    // Vm::SNES, VMPlatform::Snes
#include "retropp/snes.h"  // snes::Button, snes::Buttons, snes::Ports, snes::held
```

## Contents

- [One machine, its cartridge's region](#one-machine-its-cartridges-region)
- [What the core answers, and what it refuses](#what-the-core-answers-and-what-it-refuses)
- [Its clock](#its-clock)
- [`snes.h` — the pad, both ports](#snesh--the-pad-both-ports)
- [Its picture](#its-picture)
- [Its sound](#its-sound)
- [Its save: `.srm`](#its-save-srm)
- [Examples](#examples)
- [Where the files are](#where-the-files-are)

## One machine, its cartridge's region

```cpp
Vm::SNES snes{VmConfig{.key = "snes-ticked", .video = true}};   // VMPlatform::Snes, TimingProfile::Snes
snes.hostRom(image);
snes.run(Vm::Advance::OnTick);
```

The core is the console alone: a cartridge whose header names a coprocessor does not boot. Hosting an
image builds the machine at power-on — work RAM cleared, the program counter at the reset vector — and
reads the cartridge's own header for its video standard, so the machine is a 60 Hz console or a 50 Hz one
by what it hosts; an image whose header does not say runs at 60 Hz, and so does a machine before it hosts
anything. `run()` is that same power-on with the save carried across, as a cartridge's battery is
through a power cycle, and `reset()` on a hosted cartridge is the same again.

## What the core answers, and what it refuses

| Verb | On this core |
|---|---|
| `hostRom` · `run` · `speed` · `stop` · `reset` · `advanceTick` | answers |
| `video` · `VmConfig{.video}` | answers — [Its picture](#its-picture) |
| `enableAudio` | answers — [Its sound](#its-sound) |
| `buttons` | answers, both ports — [`snes.h`](#snesh--the-pad-both-ports) |
| `VmConfig{.key}` · `batterySave` | answers — [Its save](#its-save-srm) |
| `registerRegions` · `registerEscapes` · `registerWatches` · `bindRoutine` | **refuses at the declaration, `std::invalid_argument`**: the batch reports every entry as not reachable on this machine, because this core names no place in guest memory by address |
| `uploadRoutine` · `registerRoutine` | **refuses, `std::invalid_argument`** for a binding naming a register or an address — this core has neither to bind; a binding naming nothing reaches the core, which keeps no routine arena, `std::logic_error` |
| `read` / `write` of a place built on the spot, on a parked machine | **refuses, `std::logic_error`** — the core names no places in guest memory |
| `assemble` · `advanceClock` · `hostDriver` | **refuses, `std::logic_error`** — the core assembles no routine source, advances only by running its cartridge, and hosts no driver |
| `reset` before any cartridge is hosted | `std::logic_error` |

Each refusal is an exception naming what the core does instead, thrown where the program asked. The
escape and watch tables exist on the machine — `escapes()` and `watches()` answer — and hold nothing,
because no declaration reaches them.

## Its clock

The machine's clock is the cartridge's, read from its header when the image is hosted:

| Region | Master clock | One frame | Refresh | Run-loop profile |
|---|---|---|---|---|
| NTSC (60 Hz) | 236'250'000 / 11 Hz | 357'366 cycles — 16'639'263 ns | 60.0988 Hz | `TimingProfile::Snes` · `TickPeriodNs::Snes` |
| PAL (50 Hz) | 21'281'370 Hz | 425'568 cycles — 19'997'208 ns | 50.0070 Hz | `TimingProfile::SnesPal` · `TickPeriodNs::SnesPal` |

The NTSC clock is not a whole number of hertz and is carried as the exact ratio it is; `speed(num, den)`
scales it with the sub-cycle remainder carried, and hosting a PAL image on a machine that was NTSC
rebuilds its pacing at the new rate and keeps the factor. The console has no double-speed mode, so the
profile's double-speed budget repeats the frame budget.

**A machine on your tick spends one of its own frames per tick**, so a run loop advancing a SNES machine
sets its profile to the console's — the run loop's default is the Game Boy Color cadence, and left there
a SNES machine runs at that rate instead of its own:

```cpp
const EngineConfig config{
    // …
    .timing = TimingProfile::Snes,   // or SnesPal, for a machine hosting a 50 Hz cartridge
};
```

A machine free-running on a thread of its own holds the cartridge's cadence whatever the loop does.

## `snes.h` — the pad, both ports

```cpp
enum class Button : ActionId { B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R };
```

The twelve buttons as an Actions enum, in the pad's own shift order — the order the console's auto-read
registers hold them. Bind them like any action and read them back each tick with `snes::held(input)`,
which counts a button as down if it is held at the tick or was pressed since the last one, so a tap
shorter than a tick still reaches the guest. Bound to keys and a gamepad:

```cpp
ActionMap controls{
    {snes::Button::A,      {SDL_SCANCODE_X, PadButton::FaceLabelA}},
    {snes::Button::B,      {SDL_SCANCODE_Z, PadButton::FaceLabelB}},
    {snes::Button::X,      {SDL_SCANCODE_S, PadButton::FaceLabelX}},
    {snes::Button::Y,      {SDL_SCANCODE_A, PadButton::FaceLabelY}},
    {snes::Button::L,      {SDL_SCANCODE_Q, PadButton::ShoulderL}},
    {snes::Button::R,      {SDL_SCANCODE_W, PadButton::ShoulderR}},
    {snes::Button::Select, {SDL_SCANCODE_RSHIFT, PadButton::Select}},
    {snes::Button::Start,  {SDL_SCANCODE_RETURN, PadButton::Start}},
};
controls.add(presets::directional(snes::Button::Up, snes::Button::Down, snes::Button::Left,
                                  snes::Button::Right));
platform.actions(controls);
```

A game with actions of its own puts them in the same map, numbered clear of the twelve; there is one
action space, 64 wide.

`snes::held` returns a `snes::Buttons` — twelve `bool`s, `b` … `r` — and `vm.buttons` takes it as
**port one, with port two an empty socket**. The console has two controller ports, and `snes::Ports`
names both:

```cpp
vm.buttons(snes::Ports{.one = snes::held(input), .two = std::nullopt});   // one pad, port two empty
```

**An empty `std::optional` is an empty socket**, and a cartridge tells one apart from a pad with nothing
held the way it does on hardware: the auto-read at `$4218`–`$421F` reads `$0000` for both, but a program
that strobes `$4016` and clocks the serial port past the sixteenth bit reads 1 from a pad and 0 from a
socket with nothing in it. `snes::Ports{}` is a console with nothing plugged in. Which sockets are filled
is the game's to say, and it can change while the machine runs, at the next step boundary like any
`buttons` call.

## Its picture

`video()` hands back the last frame the picture chip finished, as `RasterContent` in `Rgba8888`, at the
dimensions the cartridge's program has the chip draw — 256 or 512 wide, 224 or 239 tall. The layer shows
the picture at those dimensions and draws nothing outside them. Everything else about video —
declaring it at construction, either clock, when a frame becomes visible, `generation` — is
[co-execution.md](co-execution.md#video-showing-its-picture).

## Its sound

The sound chip produces one stereo frame every 32 cycles of its own 1'024'000 Hz clock — 32'000 frames a
second, whatever the region. `enableAudio(rate, onSample)` converts them to `rate` on their way to your
function, so the cartridge's pitch is the same at 48'000 Hz and at 44'100 Hz. A zero rate throws
`std::invalid_argument`; a second call replaces both the rate and the function.

The frames reach your function four times a frame, from inside the step, on the thread that steps the
machine — the game's under `Advance::OnTick`, the machine's own under `Advance::Continuously`. A machine
with no function drops its frames as it runs. The hand-off to an
[`AudioSink`](../audio.md#output-the-audiosink) — a queue, the device pulling on its own thread — is the
game's, and its shape is on [co-execution.md](co-execution.md#sound-hearing-it).

## Its save: `.srm`

A keyed machine keeps the cartridge's battery-backed save RAM — how many bytes an image keeps is the
image's own answer — and the file is the `.srm` every other program that reads a SNES save expects:

```
<your game's per-user data directory>/VM/<key>/battery.srm
```

The save survives `reset()` and `run()`, as a battery does through a power cycle. A stored file is put
back into the machine only at the size the cartridge keeps. `batterySave(name)` names the file per
cartridge (`VM/<key>/<name>.srm`); the mechanism is on
[vm-and-routines.md](vm-and-routines.md#keeping-what-the-guest-writes).

## Examples

| | |
|---|---|
| `examples/snes/player` | windowed: one cartridge on two machines side by side — one on the tick, one free-running — the same two pads driving both, each machine's sound in a queue of its own with a key choosing which is heard, a scope of what the device took, the second port plugged and unplugged at a key. Asks for a ROM through the native file picker; `--verify` asserts each clock's cadence and the sound reaching a sink, headless |
| `examples/snes/cartridge/cartridge.h` | the demo cartridge the player runs when no ROM is chosen — authored as 65816 and SPC700 source and assembled as the program starts: one 32×32 sprite the d-pad moves, A and B recolor it, Start writes the battery save, and a four-note round on the sound chip |

## Where the files are

| What | Where |
|---|---|
| The vocabulary — the pad, both ports | `include/retropp/snes.h` |
| The backend — the region, the two-port word, the frame and save observers, the refusals | `src/vm/snes/snes_backend.cpp`, `snes_backend.h` |
| The sound chip's rate to the sink's | `src/vm/snes/resampler.h` |
| The core | `third_party/snaggletooth` |
