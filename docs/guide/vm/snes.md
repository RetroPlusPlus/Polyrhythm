# The SNES

`Vm::SNES` is a Super Nintendo: the Snaggletooth core behind one backend (`src/vm/snes/`), constructed
for `VMPlatform::Snes`, running `Isa::Wdc65816`. It hosts and runs a whole cartridge — booted, paced on
either clock, drawn, heard, played on both controller ports and its battery save kept — through the same
verbs as every console. This page is what the SNES is behind the surface: its clock, its vocabulary in
`snes.h`, its picture and sound, its save, what its core refuses and how, and its examples. The verbs
are on [co-execution.md](co-execution.md).

```cpp
#include "retropp/vm.h"    // Vm::SNES, VMPlatform::Snes
#include "retropp/snes.h"  // snes::Button … snes::held, snes::Ports; snes::WorkRam … snes::AudioRam, snes::videoRam …
```

## Contents

- [One machine, its cartridge's region](#one-machine-its-cartridges-region)
- [What the core answers, and what it refuses](#what-the-core-answers-and-what-it-refuses)
- [Its clock](#its-clock)
- [`snes.h` — the vocabulary](#snesh--the-vocabulary)
  - [The machine's memories](#the-machines-memories)
  - [Memories the bus cannot name](#memories-the-bus-cannot-name)
  - [The pad, both ports](#the-pad-both-ports)
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
| `buttons` | answers, both ports — [The pad, both ports](#the-pad-both-ports) |
| `VmConfig{.key}` · `batterySave` | answers — [Its save](#its-save-srm) |
| `registerRegions` · `read` / `write`, declared and built on the spot | answers — [`snes.h` — the vocabulary](#snesh--the-vocabulary) |
| `registerEscapes` · `registerWatches` | **refuses at the arming, `std::logic_error`** — the core answers no escapes and no watches. A batch with an entry declared armed throws as it registers; an entry declared switched off is recorded, and throws the same when it is switched on |
| `bindRoutine` | declares against a cartridge address; **the call refuses, `std::logic_error`** — the core makes no routine calls. A binding naming a register refuses at the declaration, `std::invalid_argument` — this core has none to bind |
| `uploadRoutine` · `registerRoutine` | **refuses, `std::logic_error`** — the core keeps no routine arena. A binding naming a register refuses first, `std::invalid_argument` |
| `assemble` · `advanceClock` · `hostDriver` | **refuses, `std::logic_error`** — the core assembles no routine source, advances only by running its cartridge, and hosts no driver |
| `reset` before any cartridge is hosted | `std::logic_error` |

Each refusal is an exception naming what the core does instead, thrown where the program asked. The
escape and watch tables exist on the machine — `escapes()` and `watches()` answer — and hold only
entries declared switched off.

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

## `snes.h` — the vocabulary

### The machine's memories

`snes.h` ships the console's memories as `MemoryRegion` constants, the same value a game fills in for a
place of its own:

```cpp
const std::vector<std::uint8_t> colors = vm.read(snes::Palette);   // all 256 palette words
```

| Constant | `.at` | `.size` | What |
|---|---|---|---|
| `snes::WorkRam` | `0x7E0000` | `0x20000` | the 128 KB of work RAM, banks `$7E`–`$7F` |
| `snes::VideoRam` | `snes::videoRam(0)` | `0x10000` | the picture chip's 64 KB, by byte: tiles and maps |
| `snes::Palette` | `snes::palette(0)` | `0x200` | CGRAM: the 256 palette words |
| `snes::Sprites` | `snes::sprites(0)` | `0x220` | OAM: the 128 sprite entries and the 32 bytes of their high bits |
| `snes::AudioRam` | `snes::audioRam(0)` | `0x10000` | the audio unit's 64 KB |

Each has `count == 1`, so reading one with no index hands back the whole memory, and each can be
declared in a batch like any other place. The cartridge and its save are not constants — their sizes
are the image's — so a place inside either is a plain bus address, `0x008000` or `0x700000`.

**Every alias of a byte names that byte.** The bus reaches the low 8 KB of work RAM at `$7E:0000`–`$1FFF`
and at `$0000`–`$1FFF` of every system bank, and the image and the save in every bank the cartridge's map
repeats them in; `0x000010`, `0x7E0010` and `0xBF0010` are one cell of work RAM, and a place declared at
any of them reads and writes it. A place's entries stride through the memory it starts in, so an array
that runs past the end of a LoROM bank's window reads the image's next bytes rather than whatever the
next address mirrors — the index is a parameter for that reason, as on every console.

A write into the cartridge patches the machine's copy of the image and the image a `reset()` or a
`run()` rebuilds it from, so a patch survives both; the file the bytes came from is untouched.

**Registers are not places.** Reading one on this console can change it — a read of `$4210` clears the
NMI flag — so a place at a register address, like one at an address no memory answers, does not
resolve, and a batch naming one reports it as not reachable on this machine.

### Memories the bus cannot name

The picture chip's three memories and the audio unit's RAM are not on the 65816's bus: a program
reaches them through ports, one byte at a time. A place inside one says which memory in the top byte of
its address, through a helper:

```cpp
MemoryRegion{.at = snes::videoRam(0x2000), .size = 32}          // one 4 bpp tile, 0x2000 bytes in
MemoryRegion{.at = snes::palette(129 * 2), .size = 2}          // palette word 129
MemoryRegion{.at = snes::videoRam(0), .size = 32, .count = 16}  // the first sixteen tiles, one entry each
```

`snes::videoRam`, `snes::palette`, `snes::sprites` and `snes::audioRam` each take the byte's offset in
the memory; `snes::inSpace(snes::Space, offset)` is the form they share. A place is read and written the
way the chip reads the memory — no port address steps, no latch moves, no register is driven — and the
picture chip reads its memories at every dot, so a write shows from the next one. A place past the end
of its memory does not resolve.

A watch on the game's own reads and writes (`AccessSource::GuestAndGame`) is answered in the flat space
the CPU sees, which these memories are not in, so it cannot name one — the batch refuses it,
`std::invalid_argument`.

### The pad, both ports

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
dimensions the cartridge's program has the chip draw — 256 wide, or 512 when any line of the frame was
drawn in half-pixels (BG modes 5 and 6, or SETINI's pseudo-hi-res bit), and 224 tall, or 239 when SETINI
asks for the taller picture. With SETINI's interlace bit set, the chip draws one field a frame — every
other line of the picture, at alternating parity — and each is handed over as a field; woven, the two
make a 448-line picture (478 for the taller one). Which size arrives is the program's, from frame to
frame, so a layer shows the picture through its `fit` and the viewport gives it the room:

| The picture | Its size | `ViewportResolution` |
|---|---|---|
| progressive | 256×224 | `Snes` |
| drawn in half-pixels | 512×224 | `SnesHiRes` |
| two fields woven | 256×448 | `SnesInterlaced` |
| both | 512×448 | `SnesHiResInterlaced` |

```cpp
Vm::SNES snes{VmConfig{.key = "snes", .video = true}};
snes.video(true, {.interlacing = {.on = true}});   // weave the fields when the program interlaces

DrawLayer screen{.key = "screen"};
screen.size = PixelSize{512, 448};                 // a viewport of ViewportResolution::SnesHiResInterlaced
RasterContent picture = snes.video();
picture.fit    = PixelSize{512, 448};              // 256×224, 512×224, 256×448 or 512×448: one slot
screen.content = picture;
```

A slot the size of the largest picture shows every size exactly — a 256-wide picture doubles each
pixel, a 224-line one doubles each line, and the 512×448 picture is one to one. A slot of 256×224 shows
every size too, dropping the half-pixels and the second field's lines on the grid the frame composes on.
The 239-line picture and its woven 478 are raw values, `{256, 239}` and `{512, 478}`. Everything else
about video — declaring it at construction, either clock, when a frame becomes visible, `generation`,
the settings — is [co-execution.md](co-execution.md#video-showing-its-picture).

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
| `examples/snes/player` | windowed: one cartridge on two machines side by side — one on the tick, one free-running — each in a slot the size of the console's largest picture, so a frame of any size lands in place; the same two pads driving both, each machine's sound in a queue of its own with a key choosing which is heard, a scope of what the device took, the second port plugged and unplugged at a key, and a key cycling how an interlaced picture is shown: each field as it comes, woven straight, woven blended. Asks for a ROM through the native file picker; `--verify` asserts each clock's cadence, the sound reaching a sink, and the picture taking each size the demo cartridge draws, headless |
| `examples/snes/coexecution` | windowed: the demo cartridge running on its own thread, its picture in a slot the size of the console's largest, beside a panel drawn from its own memory every tick through four declared places — the 256 palette words as swatches (`snes::Palette`), the first sixteen tiles of video RAM decoded from 4 bpp, the sprite's X and Y in work RAM, and the step byte in the image. Keys write each of them while it runs: the sprite's palette word, its X, and the step patched in the image; SPACE parks and resumes. `--verify` round-trips a palette word and the sprite's X and checks a patched step moves the sprite that far a frame, headless |
| `examples/snes/cartridge/cartridge.h` | the demo cartridge every SNES example hosts — authored as 65816 and SPC700 source and assembled as the program starts: one 32×32 sprite of one-pixel stripes the d-pad moves by a step it reads from a byte of its own image each frame, A and B recolor it, Start writes the battery save, Y switches the screen mode between 1 and 5 so the frame is drawn in half-pixels, X switches the chip to interlace so it hands over fields, and a four-note round on the sound chip |

## Where the files are

| What | Where |
|---|---|
| The vocabulary — the machine's memories, the space helpers, the pad, both ports | `include/retropp/snes.h` |
| The backend — the region, places resolved to their byte, the two-port word, the frame and save observers, the refusals | `src/vm/snes/snes_backend.cpp`, `snes_backend.h` |
| An address decoded to its space, and a byte's bus address | `src/vm/snes/snes_address.h` |
| The sound chip's rate to the sink's | `src/vm/snes/resampler.h` |
| The core | `third_party/snaggletooth` |
