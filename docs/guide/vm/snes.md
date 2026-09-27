# The SNES

`Vm::SNES` is a Super Nintendo: the Snaggletooth core behind one backend (`src/vm/snes/`), constructed
for `VMPlatform::Snes`, running `Isa::Wdc65816`. It hosts and runs a whole cartridge — booted, paced on
either clock, drawn, heard, played on both controller ports and its battery save kept — and it runs
routines: 65816 source placed into an image of its own and called as typed C++ functions, and a routine a
cartridge already holds, bound where it sits; and it weaves native code into a running cartridge through
escapes and watches — through the same verbs as every console. This page is
what the SNES is behind the surface: its clock, its vocabulary in `snes.h`, what its hardware imposes,
its assembler's dialect, its picture and sound, its save, what its core refuses and how, and its
examples. The verbs are on [vm-and-routines.md](vm-and-routines.md) and
[co-execution.md](co-execution.md).

```cpp
#include "retropp/vm.h"    // Vm::SNES, VMPlatform::Snes
#include "retropp/snes.h"  // snes::A … snes::PC, snes::rtl; snes::WorkRam … snes::AudioRam, snes::videoRam …; snes::Button … snes::Ports
```

## Contents

- [One machine, its cartridge's region](#one-machine-its-cartridges-region)
- [What the core answers, and what it refuses](#what-the-core-answers-and-what-it-refuses)
- [Its clock](#its-clock)
- [`snes.h` — the vocabulary](#snesh--the-vocabulary)
  - [Registers](#registers)
  - [The machine's memories](#the-machines-memories)
  - [Memories the bus cannot name](#memories-the-bus-cannot-name)
  - [A routine's return: `snes::rtl`](#a-routines-return-snesrtl)
  - [The pad, both ports](#the-pad-both-ports)
- [What the hardware imposes](#what-the-hardware-imposes)
- [The assembler's dialect](#the-assemblers-dialect)
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

A machine that hosts no cartridge is a routine machine: the first routine placed on it builds an image
of the core's own — a LoROM cartridge with a header the console accepts and an idle loop every vector
points at — and `reset()` rebuilds that image with every placed routine intact. The two are exclusive:
a machine holding the core's own image refuses `hostRom`, and one hosting a game's cartridge refuses
`uploadRoutine` / `registerRoutine` (`std::logic_error` either way).

## What the core answers, and what it refuses

| Verb | On this core |
|---|---|
| `hostRom` · `run` · `speed` · `stop` · `reset` · `advanceTick` | answers |
| `video` · `VmConfig{.video}` | answers — [Its picture](#its-picture) |
| `enableAudio` | answers — [Its sound](#its-sound) |
| `buttons` | answers, both ports — [The pad, both ports](#the-pad-both-ports) |
| `VmConfig{.key}` · `batterySave` | answers — [Its save](#its-save-srm) |
| `registerRegions` · `read` / `write`, declared and built on the spot | answers — [`snes.h` — the vocabulary](#snesh--the-vocabulary) |
| `uploadRoutine` · `registerRoutine` · `assemble` | answers, on a machine hosting no cartridge — [Registers](#registers), [The assembler's dialect](#the-assemblers-dialect) |
| `bindRoutine` | answers — a routine the cartridge holds, called in the guest's own context; [What the hardware imposes](#what-the-hardware-imposes) says where the call lands |
| `advanceClock` | answers, on a machine holding the core's own image — the machine idles on that image's own loop; on a machine hosting a game's cartridge, **refuses, `std::logic_error`** — the cartridge advances by running |
| `registerEscapes` · `registerWatches` | answers — [What the hardware imposes](#what-the-hardware-imposes) says what an escape and a watch are on this console |
| `hostDriver` | **refuses, `std::logic_error`** — the core hosts no driver |
| `reset` before any cartridge is hosted or routine placed | `std::logic_error` |

Each refusal is an exception naming what the core does instead, thrown where the program asked.

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

`vm.h` is system-agnostic; `snes.h` is the console's half of the surface — the register file, the
machine's memories, the memories the bus cannot name, a routine's return and the pad — as the typed
constants a binding, a declaration or an `ActionMap` names.

### Registers

```cpp
enum class Reg : std::uint16_t { A, B, P, DB, PB, C, X, Y, D, S, PC };
```

The 65816 register file, as `Location` constants: `snes::A snes::B snes::P snes::DB snes::PB` are
8-bit, `snes::C snes::X snes::Y snes::D snes::S snes::PC` 16-bit. `snes::A` is the accumulator's low
byte and `snes::B` its high byte, the chip's names for the two halves; `snes::C` is the whole 16-bit
accumulator. `snes::X` and `snes::Y` are 16 bits wide whatever the index-width flag says — in 8-bit index
mode the chip uses the low byte — so a binding that carries a byte in one carries it as a
`std::uint16_t`. The enumerator order is the backend's register id and fixes each register's width, so a
binding that puts a `std::uint8_t` in `snes::X` is refused at registration (`std::invalid_argument`),
as is one naming a register this machine does not have. A binding on this console says its ISA:

```cpp
auto mix = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint16_t)>(
    "routines/mix.asm", {.inputs = {snes::A, snes::X}, .output = snes::A, .isa = Isa::Wdc65816});
std::uint8_t half = mix(3, 5);   // 4
```

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

**A register reads as it stands.** A place at a register's bus address — `0x004210`, or the same offset
in any system bank — answers the byte the program's own read would answer, with nothing moved: the NMI
flag a read of `$4210` clears stays set, the port a read of `$2140` takes keeps its byte, the video
address a read of `$2139` steps stands. A run over registers is a run over the register windows, which
have open bus between them, so every byte of it has to be a register. A register takes no write through
a place (`std::logic_error`): writing one is a program's own store, with the effects a store has, and
goes through a routine. An address no memory answers — open bus — does not resolve, and a batch naming
one reports it as not reachable on this machine.

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

A watch is on the 65816's bus, which these memories are not on, so it cannot name one — the batch
refuses it, `std::invalid_argument`.

### A routine's return: `snes::rtl`

```cpp
constexpr std::uint32_t rtl(std::uint32_t entry) noexcept;
```

The 65816 has two calls and two returns: a routine a `JSR` enters ends in `RTS`, and one a `JSL` enters
ends in `RTL`, three bytes on the stack against two. A bare address names a routine an `RTS` leaves;
`snes::rtl(entry)` names one an `RTL` leaves, and the call into it pushes the landing a `JSL` would:

```cpp
auto decode = vm.bindRoutine<std::uint16_t(std::uint16_t)>(snes::rtl(0x018000),
                                                          {.inputs = {snes::C}, .output = snes::C, .isa = Isa::Wdc65816});
```

An escape's `.at` takes it the same way: a routine replaced at `snes::rtl(entry)` answers with an `RTL`
standing in, so the caller's `JSL` gets its three bytes back.

The bit rides the top of the 32-bit address, above the 24-bit bus; it names code, so with a memory the
bus cannot name it resolves nothing. A routine the core places from source returns with `RTS`, always —
the core places it and calls it, so the convention is the core's.

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

## What the hardware imposes

Facts a program meets on this console that the capability pages state in the console-agnostic form;
here is what they are on this hardware.

- **A placed routine begins in a frame of the core's own:** native mode, 8-bit accumulator and index
  registers with interrupts off (`P = $34`), direct page `$0000`, data bank `$00`, the stack at `$1FFF`,
  every other register zero, then the binding's inputs over it. It returns with `RTS` to the image's idle
  loop, and the register file it left is what a register output reads. A routine that wants 16-bit
  registers sets them itself (`REP #$30`) and says so to the assembler (`A16` / `X16`).
- **A routine lands where its source says, or where it fits.** A source that says `ORG` is placed at
  that address, and the core's image grows to a second bank for one that asks for it; a source with no
  `ORG` is assembled at the first gap of the arena that holds it — from `$00:8000` up, past every placed
  routine — so its labels resolve for the address it lands at. Two routines never share a byte: an `ORG`
  over a placed routine, the idle loop or the header is refused, `std::invalid_argument`, as is one at an
  address no byte of the image is at (work RAM, a register page). Raw bytes handed to `uploadRoutine`
  take the next gap the same way, so they are position-independent or were assembled for it.
- **A call into a running cartridge lands at the next instruction boundary.** A cycle budget parks the
  machine wherever the cycle fell — inside an instruction, more often than not — and the routine runs
  between two of the guest's instructions: the one in flight finishes first, then the landing is pushed.
  The register file that goes back afterwards is the file at that boundary, with the interrupt lines as
  they then stand.
- **The guest's stack is page one in emulation mode.** A call on the core's own scratch stack pushes
  its landing at `$1FFF` for a guest in native mode and at `$01FF` for one in emulation mode, where the
  chip keeps the stack in page one.
- **A call spends the machine's own time.** The cycles a routine runs are the machine's — the beam moves,
  the sound chip is paced — and `advanceClock(cycles)` on a routine machine idles it for `cycles` more
  from where its clock stands, the register file put back afterwards.
- **A register's value is answered as the machine stands.** A program's read ticks the beam through
  its own cycle before it answers and a place's read spends no cycle, so a bit the beam decides —
  `$4212`'s blank flags, `$213F`'s field bit — is answered at the position the machine is parked at,
  and the program's own read an instruction later can see it otherwise. The place itself is under
  [The machine's memories](#the-machines-memories).
- **An escape and a watch are on a byte, not an address.** Every address that reaches a byte fires what
  is armed on it: an escape at `$80:8100` hears the program running through `$00:8100`, and a watch
  declared at `$7E:0040` hears a direct-page store to `$40`. An escape is reported as it was armed; a
  watch's handler is told the 24-bit address the access drove — `0x000040` for that store.
- **An escape names code: work RAM, the cartridge image or its save.** One at a register or at an
  address no memory answers is refused, `std::invalid_argument`. It is told once per instruction the
  chip begins — a block move begins one for each byte it moves — at a boundary the CPU takes: not while
  a transfer engine holds the bus, not while the core is halted, and an instruction a hardware interrupt
  lands on is told when the handler returns to it.
- **A replaced routine's bytes stay as the cartridge holds them.** The return a `.replaces` escape
  stands at the entry — an `RTS`, or an `RTL` for `snes::rtl` — answers the one fetch that begins the
  routine, and costs what a real return costs. A data read of the entry byte, from the program or through a place, answers the
  cartridge's own byte. Two escapes on one byte that would stand different answers there are refused,
  `std::invalid_argument`.
- **A watch names work RAM, the cartridge image, its save or a register**, and sees every access to it:
  the CPU's, both transfer engines', and the work-RAM port's.
- **A 16-bit access is two byte accesses**, on two cycles, each told and answered alone — a veto on the
  high byte of a store lands the low byte. An opcode fetch is a read, so answering one changes the
  instruction that runs.
- **A register's read is told after its effect.** A read of `$4210` has already cleared the NMI flag,
  and a read of `$2140` has already taken the port's byte, when the handler is told; its answer changes
  only what the program receives.
- **A routine is called from an escape, not from a watch.** A watch is told part-way through the access
  it decides, and a call from inside its handler throws `std::logic_error` with nothing moved. An escape
  is told between instructions, so a call from inside one is the same call a parked machine takes, at
  any depth.

## The assembler's dialect

`registerRoutine` and `assemble` run 65816 source through Snaggletooth's assembler
(`third_party/snaggletooth/docs/65816-assembly.md`), which is absolute: `ORG $00:8400` says where the
bytes go, and a source with none is placed where it fits, as above. The dialect carries the register
widths along the file — `REP` and `SEP` move them, `XCE` after `CLC` or `SEC` changes the mode, and
`A8` / `A16` / `X8` / `X16` / `EMULATION` / `NATIVE` say what the instructions cannot — and an
immediate under a width nothing has said is an error, never a guess, so a routine says `A8` before its
first `LDA #`. `!` marks an absolute operand (`JMP !done`, `LDA !$2140`) and `$` a direct-page one.
A bad mnemonic, a malformed operand or a width nothing said throws at registration, `std::runtime_error`
naming the line. The bytes assembled for a call's `Isa::Wdc65816` binding are baked into the binary at
build time under the `Embed` policy, exactly as an SM83 routine's are; the two registration forms and
the policy are on [vm-and-routines.md](vm-and-routines.md#authoring-in-assembly-register-from-a-asm-file).

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
| `examples/snes/coexecution` | windowed: the demo cartridge running on its own thread, its picture in a slot the size of the console's largest, beside a panel drawn from its own memory every tick through six declared places — the 256 palette words as swatches (`snes::Palette`), the first sixteen tiles of video RAM decoded from 4 bpp, the sprite's X and Y in work RAM, the step byte in the image, and two registers read as they stand: JOY1 at `$4218`, the buttons held as the console latched them, and RDNMI at `$4210`. Keys write the first four while it runs: the sprite's palette word, its X, and the step patched in the image; SPACE parks and resumes. A second machine holds no cartridge and runs `mix`, the average of two bytes, from `routines/mix.asm` placed where it fits; the cartridge carries the same routine at `$00:8400`, bound where it sits — a key calls either for a value and writes it to the sprite's color, the cartridge's own while it is parked, and the panel counts how many of those calls answered exactly. Two escapes and two watches, declared before it runs and switched at a key while it does: `pace` answered natively — the sprite moved in every direction by the step averaged with 8, the average computed by the cartridge's own `mix`, called from inside the escape — an escape at `report` hearing every frame, a watch on the sprite's Y that vetoes its stores or holds them in a band, and one answering every read of the step byte with 3; the panel counts what each did. `--verify` round-trips a palette word and the sprite's X, checks a patched step moves the sprite that far a frame, reads JOY1 with Right held and released, calls both routines, calls the cartridge's own routine parked at 240 points of its frame, each answer exact, and switches each escape and watch on in turn and checks what it does to the running cartridge, headless |
| `examples/snes/cartridge/cartridge.h` | the demo cartridge every SNES example hosts — authored as 65816 and SPC700 source and assembled as the program starts: one 32×32 sprite of one-pixel stripes the d-pad moves by a step it reads from a byte of its own image each frame, A and B recolor it, Start writes the battery save, Y switches the screen mode between 1 and 5 so the frame is drawn in half-pixels, X switches the chip to interlace so it hands over fields, a four-note round on the sound chip, and three routines of its own a program reaches: `pace` at `$00:8380`, which the frame handler calls for every held direction with the coordinate in A and the direction in X, answering the coordinate one step that way; `report` at `$00:83A0`, an empty routine the frame handler calls as it finishes; and `mix` at `$00:8400`, the average of A and X |

## Where the files are

| What | Where |
|---|---|
| The vocabulary — the registers, the machine's memories, the space helpers, `snes::rtl`, the pad, both ports | `include/retropp/snes.h` |
| The backend — the region, places resolved to their byte, the routine frame and the calls, the escapes and watches, the two-port word, the frame and save observers, the refusals | `src/vm/snes/snes_backend.cpp`, `snes_backend.h` |
| An address decoded to its space and its return kind, and a byte's bus address | `src/vm/snes/snes_address.h` |
| The image a routine machine writes — its header, its idle loop, where a routine fits | `src/vm/snes/snes_image.cpp`, `snes_image.h` |
| The assembler, and the build step that bakes a routine's source | `third_party/snaggletooth/tools/cpu65816/`, `cmake/bake_snaggletooth_routine.cmake` |
| The sound chip's rate to the sink's | `src/vm/snes/resampler.h` |
| The core | `third_party/snaggletooth` |
