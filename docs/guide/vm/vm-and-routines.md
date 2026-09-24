# Conductor — the VM host & routines

**Conductor** is the platform's VM layer: it hosts the machines, paces them, and coordinates them with
your own code. This page is the routine surface; a whole cartridge running inside your game is
[co-execution.md](co-execution.md). The map of the layer — the seam, the consoles, which verbs every
console shares — is [README.md](README.md), and each console has a page of its own for what its core is
and answers: [gameboy.md](gameboy.md), [snes.md](snes.md).

The VM host runs the narrow set of original-machine routines whose output a native re-implementation
**cannot** reproduce exactly — gameplay RNG that reads a free-running hardware register, and a
cycle-driven sound driver — and exposes each as an ordinary typed C++ function. Everything
else in a port is native code; this is the surgical exception.

```cpp
#include "retropp/vm.h"          // VMPlatform, Vm, Routine, Location, RoutineBinding, Throttle
#include "retropp/gb.h"          // gb::Reg + gb::A … gb::PC — the Game Boy register vocabulary
#include "retropp/gb_routines.h" // retropp::sameboy::divRng — the ready-made GB routine preset
```

## Contents

- [The shape of it](#the-shape-of-it)
- [Selecting a system: `VMPlatform`](#selecting-a-system-vmplatform)
- [Registering your own routine](#registering-your-own-routine)
  - [Authoring in assembly: register from a `.asm` file](#authoring-in-assembly-register-from-a-asm-file)
  - [Assembling source to bytes yourself](#assembling-source-to-bytes-yourself)
  - [State persists across calls](#state-persists-across-calls)
  - [Time-based registers: advance the clock between calls](#time-based-registers-advance-the-clock-between-calls)
- [Pacing: `Throttle` (and the audio seam)](#pacing-throttle-and-the-audio-seam)
- [Hosting a resident driver](#hosting-a-resident-driver)
- [Hosting a whole cartridge](#hosting-a-whole-cartridge)
  - [Keeping what the guest writes](#keeping-what-the-guest-writes)
- [The typed callable: `Routine<Sig>`](#the-typed-callable-routinesig)
- [Ready-made presets: `retropp::sameboy`](#ready-made-presets-retroppsameboy)
- [Where to change things](#where-to-change-things)
- [Status](#status)

## The shape of it

```cpp
retropp::Vm vm{retropp::VMPlatform::GameBoyColor};

// Register a routine ONCE, declaring where its inputs/output live. Then call it like a function.
auto rng = retropp::sameboy::divRng(vm);     // a ready-made preset — no bytes, no addresses
std::uint8_t roll = rng();                // plain C++ at the call site: no register/memory idiom
```

Two ideas carry the whole design:

1. **Call it like a function.** A routine is registered once and thereafter called as a typed C++
   callable. Registers, memory addresses, and entry offsets appear **only** in the registration's
   binding — never at a call site. This is the platform's "no hardware-register variables" principle
   carried to the VM boundary.
2. **The routine path needs no ROM.** A ported game's extracted routines — authored as SM83 `.asm`
   source the platform assembles in-process, or supplied as pre-assembled bytes — run in a cartridge
   the platform builds itself; nothing original is loaded to call them. A game that owns a whole
   cartridge takes the other path: `hostRom` makes the image addressable and `run` makes it live
   (see [Hosting a whole cartridge](#hosting-a-whole-cartridge)). One VM does one or the other.

## Selecting a system: `VMPlatform`

```cpp
enum class VMPlatform { GameBoy, GameBoyColor, Snes, Nes, Genesis, MasterSystem };
```

`VMPlatform` selects a per-system backend. The call surface is identical across systems because each
routine's convention is sealed in its own binding; only the register/memory *vocabulary* a binding
names is system-specific (and lives in a per-system header — `gb.h` for the Game Boy family).

The **GameBoy / GameBoyColor** backend and the **Snes** backend are built. A GameBoy machine runs
routines (this page); a hosted SNES cartridge runs whole (`co-execution.md`) — `Vm::SNES` and its pad
vocabulary in `snes.h`, with no `uploadRoutine` / `assemble` surface. The SNES core is the console
alone: a cartridge whose header names a coprocessor does not boot. Any other enumerator throws
`std::runtime_error` ("no backend built") at `Vm` construction; a new system is a drop-in backend,
not a change to this surface.
(`vm.platform()` reports back the system a `Vm` was constructed for.)

A machine runs at its core's own speed — a Game Boy at 4'194'304 Hz, one tick of it one 70'224-cycle
frame — and `speed()` is what scales it. A `Vm` is non-copyable but movable.

Each platform maps to an instruction-set architecture — `Isa` (`retropp/isa.h`), via
`isaFor(VMPlatform)`. The ISA is the real compatibility unit: several consoles can share one (the Game
Boy and Game Boy Color both run `Isa::Sm83`), so a routine's bytes run on any VM of the same ISA. The
audio library uses it to verify a chiptune is cued on a compatible VM; a routine consumer rarely names
it directly.

## Registering your own routine

There are two forms, mirroring the audio library's `uploadAudio` / `registerAudio` (and the renderer's
`uploadAtlas` / `loadAtlas`): **`uploadRoutine`** takes ready bytes; **`registerRoutine`** takes a `.asm`
file path. Start with the byte form — the `.asm` form (below) assembles down to it.

```cpp
template <typename Sig>
Routine<Sig> Vm::uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances = 1);
```

`Sig` is the function signature you want to call it as — e.g. `std::uint8_t(std::uint8_t)`. The
**signature** fixes the width of each value (`uint8_t` → 1 byte, `uint16_t` → 2, `uint32_t` → 4); the
**binding** fixes where each value lives and how the routine is paced:

```cpp
struct RoutineBinding {
    std::vector<Location>   inputs;          // argument i is marshalled to inputs[i]
    std::optional<Location> output{};        // return value is read from output (nullopt = void)
    Throttle                throttle = Throttle::HostSpeed;
    std::uint32_t           entryOffset = 0; // first instruction's offset WITHIN routineBytes
};
```

A `Location` is a register or an absolute memory address:

```cpp
Location::reg(id)        // a register, by backend id — use the typed constants below, not raw ids
Location::memory(addr)   // an absolute address in the target machine's memory map
```

For the Game Boy family, name registers with the `gb::` constants instead of raw ids:

```cpp
// ADD A,B ; RET   — sum two bytes, both in registers
static constexpr std::uint8_t kAdd[] = {0x80, 0xC9};
auto add = vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    kAdd, {.inputs = {gb::A, gb::B}, .output = gb::A});
std::uint8_t s = add(3, 4);    // 7

// a routine that reads a byte from HRAM and writes one back — bound by address
auto f = vm.uploadRoutine<std::uint8_t(std::uint8_t)>(
    bytes, {.inputs = {retropp::Location::memory(0xFF90)}, .output = retropp::Location::memory(0xFF91)});
```

The Game Boy register constants (`retropp/gb.h`): `gb::A gb::F gb::B gb::C gb::D gb::E gb::H gb::L`
(8-bit) and `gb::AF gb::BC gb::DE gb::HL gb::SP gb::PC` (16-bit). A value's width must match its bound
register (a `uint16_t` bound to `gb::A` throws at registration).

Both forms validate the binding and throw (`std::invalid_argument`) on an inputs/arity mismatch, a
width/location mismatch, an unknown register, an inaccessible address, a void signature that binds an
output (or a value-returning one that binds none), empty routine bytes, or an `entryOffset` past the end
of the bytes — so a malformed binding fails loudly at registration, not silently at call time.

### Authoring in assembly: register from a `.asm` file

You normally don't hand-write byte arrays — you write the routine as **SM83 assembly in a `.asm`
file** and point `registerRoutine` at it. The VM reads the file and assembles it in-process with an
built-in SM83 assembler — no external toolchain, no build step, nothing to install:

```cpp
template <typename Sig>
Routine<Sig> Vm::registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy = {}, int instances = 1);
```

```cpp
// my_add.asm:
//     add a, b
//     ret
auto add = vm.registerRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
    "routines/my_add.asm", {.inputs = {gb::A, gb::B}, .output = gb::A});   // Embed (default policy)
std::uint8_t s = add(3, 4);   // 7
```

The path is a **compile-time `LiteralPath`** (a string literal), so a build-time scan can find it — not a
runtime string. A genuinely runtime path is not accepted: read its bytes yourself and use `uploadRoutine`.
The optional `policy` is the same `AssetPolicy` the asset and audio forms use:

- **`Embed`** (default) — use the bytes the build baked into the binary for this logical path. If none
  were baked (no scan ran), it falls through to an on-disk read so the path still works during
  development, and logs a warning naming the routine. Take that warning seriously in a build you intend
  to ship: `Embed` promises the bytecode is inside the binary, and the disk read behind it succeeds only
  where the `.asm` is present. Registering from a static library needs no link settings of its own — see
  [build-and-consume.md](../build-and-consume.md#registering-code-in-a-library).
- **`LoadFromPath`** — resolve `asmFilePath` against the platform's single `assetRoot()`, read it at
  registration, and assemble it in-process — the form for a copyright-derived routine you ship beside
  the binary rather than bake in. There is no separate routine root: routines resolve against the same
  `assetRoot()` as `loadAtlas` / `loadMapPng`.

Omit `policy` to take the per-type default (`Embed`); the only way to deviate is the explicit per-call
token, so the policy reads at the call site.

The assembly follows the conventional Game Boy dialect — the same syntax the published disassemblies
are written in: `;` line comments, `label:` definitions, `$hex` / `%bin` / decimal literals, `[hl]` /
`[$FF04]` memory operands, condition codes, and the standard SM83 instruction set. Game Boy hardware registers are
predefined by name, so you write `ldh a, [rDIV]` rather than `ldh a, [$FF04]`; labels are resolved
across the routine (e.g. `jr` targets). The same `Sig` / `binding` rules as the byte form apply, and
the routine's entry is its first byte. A bad mnemonic, a malformed operand, an unknown symbol, or an
unreadable file throws at registration with the offending line — a typo fails loudly, not at call time.

The byte form (`uploadRoutine(span, …)`) is the low-level path the `.asm` form assembles down to; reach
for it only when you already hold assembled bytes.

### Assembling source to bytes yourself

```cpp
std::vector<std::uint8_t> Vm::assemble(std::string_view source);
```

`assemble` runs SM83 source through this VM's ISA assembler and hands back the machine-code bytes
**without registering anything** — the path when you hold routine source as a *runtime* string (not a
compile-time literal path) and want bytes to pass to `uploadRoutine`. It throws on a source error, with
the offending line. The VM's platform fixes the ISA, so the right assembler is always selected.

### State persists across calls

Routines registered on one `Vm` share its machine memory, so state a routine writes (e.g. an RNG seed
in HRAM) carries into the next call — that is exactly what makes an evolving RNG work. `vm.reset()`
returns the machine to its post-reset state (clearing that state); the registered routines stay
registered.

> **Power-on state is not zero.** Like real hardware, the machine powers on with arbitrary RAM. A
> routine that reads a seed should have that seed written first (the game does this during init).
> Write a known byte with a one-line routine whose input binds to the address, or call a routine that
> seeds it.

### Time-based registers: advance the clock between calls

Some routines read a **free-running hardware register** — most importantly the Game Boy's `rDIV`
divider, which an RNG folds into its seed. On real hardware that register ticks continuously while
the game runs between calls, so it holds an unpredictable value each time. The VM, by contrast, only
runs *during* a routine and is frozen between calls — so without help `rDIV` barely moves and an RNG
degenerates into a slow counter.

`Vm::advanceClock(cycles)` ticks the machine's free-running clock without executing a routine, so
those registers keep advancing between calls exactly as on hardware. Drive it from your run loop —
one tick's worth of cycles per tick — and read the amount from the timing profile (don't hardcode it):

```cpp
loop.simTick([&](const retropp::InputState&) {
    vm.advanceTick();            // rDIV (and any time-based register) free-runs with the platform's time
    // ... game logic, which may call rng() ...
});
```

`advanceTick()` says *a tick happened* and lets the machine work out what that is worth to it. The
lower-level `advanceClock(cycles)` is still there when you want to spend an explicit amount.

Either way, only timing/divider state advances — registers, RAM, and the RNG seed are untouched. A
routine that reads no time-based register (pure computation — math, decompression) does not need it.

#### A machine running at a cadence that is not its own

`advanceTick()` uses this VM's own cadence, which is the ordinary case. Pass the period explicitly
when the run loop is ticking at something else — hosting a machine from one console while the loop
runs at another console's rate:

```cpp
vm.advanceTick(loop.tickPeriod());   // spend what THIS machine is worth at the loop's actual rate
```

The cycles come from the machine's clock rate and the period actually being run, never from its own
frame count — a frame count is right only when the two cadences coincide. A clock that does not
divide the tick period leaves a fraction of a cycle behind each tick; the fraction is **carried**, not
dropped, so the running total stays exact over any number of ticks and the error at any instant is
under one cycle. Each VM carries its own, so two machines at different rates under one tick simply
carry independently.

At its own cadence a machine uses its stored frame budget instead, and that is deliberate: a Game Boy
frame *is* 70'224 cycles at 4'194'304 Hz, which works out to 16'742'706.3 ns — and a tick period has
to be a whole number of nanoseconds, so deriving the count back out of the stored period would lose a
cycle a frame. The stored count is the exact fact; the period is the rounded one.

Three `TimingProfile` helpers keep cadence values out of your code (`#include "retropp/timing.h"`):

| Helper | Returns | Use |
|---|---|---|
| `cpuCyclesPerTick()` | `std::uint32_t` | this machine's cycles in one tick of its OWN cadence (0 if the profile has no CPU model) |
| `cyclesForTick(period, carry)` | `CycleDraw` | the cycles one tick of `period` is worth, plus the remainder to carry into the next call |
| `ticksForDuration(std::chrono::duration)` | `std::uint64_t` | a wall-clock interval as a tick count, e.g. `ticksForDuration(std::chrono::seconds{2})` — the same `uint64_t` `RunLoop::tickCount()` uses |

## Pacing: `Throttle` (and the audio seam)

```cpp
enum class Throttle { HostSpeed, HardwareSpeed };
```

`HostSpeed` runs the routine as fast as the host allows — correct for a routine you CALL for a return
value (RNG). `HardwareSpeed` throttles the routine to the CPU clock for a real-time consumer like a
sound driver; it is **realized** and drives the audio chain — but you don't register a HardwareSpeed
routine directly, the [`AudioSystem`](../audio.md) does it for you (you register *audio*, not a routine).
`instances > 1` (multiple independent copies of a routine, for anti-channel-stealing audio) remains a
**declared seam**: registering with it throws `std::logic_error` today and is realized with the
anti-stealing backend. It is in the surface now so that work plugs in without an API break.

The raw driver chain underneath the `AudioSystem` is three `Vm` members: `enableAudio(rate, onSample)`
turns on the APU and routes each produced PCM frame, `startDriver(routine)` positions a `HardwareSpeed`
routine to run continuously, and `stepDriver(cpuCycles)` advances it one cycle budget (producing audio
into the sink). You normally let the [`AudioSystem`](../audio.md) own these; reach for them directly only
to host a driver yourself. A hosted cartridge's own sound comes through the same `enableAudio`, on
every console — see [co-execution.md](co-execution.md#sound-hearing-it).

## Hosting a resident driver

A sound driver is more than a called routine: it is a **resident machine** — one or more placed code
images, a per-frame tick, and its own RAM — that runs for the life of the system. The normal way to host
one is [`AudioSystem::host`](../audio.md#hosting-your-own-sound-driver), which owns the VM and drives the
driver through a typed handle; `Vm` exposes the raw placement + step surface underneath, for hosting a
driver directly.

The driver's shape is a `DriverBinding` (`retropp/driver_binding.h`): its placed `images`, the `tickEntry`
called once per frame, an optional `init` run once, declared `slots`, an optional `stackTop`, and the
`isa`. `Vm::hostDriver(binding)` places the images and runs `init`; `Vm::tickDriver(queued, cyclesPerFrame)`
applies queued gestures, calls the tick, publishes the read-slot snapshot, and idles the rest of the frame
budget; `Vm::readSlot(index)` reads one published slot.

### Placing images, banked when a driver needs it

Each `DriverImage` names its bytes and a **base** address:

```cpp
binding.images = {{.bytes = engineBytes, .base = 0x6000}};                    // a flat image at $6000
binding.images = {{.bytes = bankData,    .base = retropp::gb::banked(0x3A, 0x4000)}};  // bank $3A at $4000
```

`gb::banked(bank, addr)` folds a ROM bank into the base so a driver's banked data lands where its own code
expects it; the VM bank-switches through the **driver's own placed code** (its writes to the mapper
registers), exactly as on hardware — you place the images, the driver drives the banking. The cartridge
mapper is declared on the binding with a hardware constant — `gb::Mbc3`, the common banked-cartridge mapper;
flat images with no banking need none, and the backend sizes the cartridge to hold the highest placed bank.

Placement is validated at `hostDriver`: overlapping images throw, and placing into the boot-ROM-overlaid
low window throws with a clear message. A driver whose audio RAM would collide with the default scratch
stack declares its own `stackTop` on the binding (the default is the platform scratch top).

### The resident tick

Unlike a called routine — which runs to a `ret` and hands back a value — the tick is **call-and-return
within the frame's cycle budget**: `tickDriver` applies any queued slot writes and cued gestures in
submission order, calls `tickEntry`, publishes the read-slot snapshot, then idles the machine for the
remainder of `cyclesPerFrame` so the APU keeps producing at the console's rate. A resident driver is never
called for a return value; its output is sound and its published slots.

**Under `AudioSystem::host` the machine steps on a thread of its own.** The system gives each machine a
runner that owns it: the machine, the mailbox its gestures arrive on, and the step. A verb you issue
crosses to that runner and is performed at the next tick boundary in the order you issued it, and the
runner steps only while the frames waiting downstream of it are under the output's latency target. The
`Vm` itself is untouched by any of this — one thread owns a machine and calls its surface, the same
`hostDriver` / `tickDriver` surface you call yourself when you host a driver directly.

## Hosting a whole cartridge

A `Vm` also hosts a **whole cartridge** your game supplies: `hostRom(bytes)` makes every byte of the
image addressable, `run()` boots it and runs its own code — on a thread of its own, or one tick at a
time on yours — and native code and the cartridge's code call each other as subroutines. That surface
— declaring places, reading and writing them, running and pacing the machine, escaping out of the
guest and calling back into it — is its own page: **[co-execution.md](co-execution.md)**.

Hosting a cartridge and hosting a resident driver are **exclusive**, and each refuses the other.
`hostDriver` synthesizes an image the platform owns; `hostRom` takes one your game owns. On a hosted
cartridge `uploadRoutine` and `registerRoutine` throw as well, since a game's image has no arena to
inject into. One `Vm` does one or the other; use two if you need both.

### Keeping what the guest writes

A cartridge holds on to some of what its guest writes when the power goes off. **Give a machine a key**
and the platform keeps that for the player, and gives it back on the next run:

```cpp
Vm machine{VMPlatform::GameBoyColor, VmConfig{.key = "adventure", .video = true}};
machine.hostRom(image);
machine.run();
```

Your game calls nothing else. The stored data is read back as the cartridge is hosted, and written out
after the guest changes it — so a player who saves inside the game finds that save next time whether or
not your code thought to ask. The file lands beside everything else you keep for that player:

```
<your game's per-user data directory>/VM/adventure/battery.sav
```

The extension is the **core's**, and it always ends the file. The bytes are the console's own format, so
what a file of them is called is that console's answer — a Game Boy machine's data lands as the `.sav`
every other program that reads one already expects, and a player can take their save elsewhere.

`VmConfig::key` is the machine's own, and rides the same aggregate its outputs are declared in —
`EngineConfig`'s shape, one step down. Leave it out and the machine keeps nothing, which is every
machine that does not ask for this.

A machine that plays **whatever it is handed** needs a file per cartridge, and cannot say which at
construction because it does not have the image yet. `batterySave` says it once the image is in hand:

```cpp
Vm machine{model, VmConfig{.key = "player", .video = true}};
machine.batterySave(nameOf(image));   // → VM/player/<that image>.sav
machine.hostRom(image);
```

Either order works — set before hosting, the stored copy is read back as the cartridge arrives; set
after, it is read back there. Spell the extension out and it is left alone (`"zelda.sav"` stays), leave
it off and it is added, carry another one and it keeps that and gets the core's after it
(`"zelda.bak"` → `"zelda.bak.sav"`). The key and the file's name are each **one path component**:
`"."`, `".."`, or anything carrying a separator throws `std::invalid_argument` where it was given.
`batterySave` throws `std::logic_error` on a machine with no key, and on a running machine like every
verb that touches the machine — park it first.

What the bytes mean belongs to the machine, never to your game: the platform never reads one, so a
cartridge that keeps nothing produces no file, and a machine on another console keeps whatever that
console keeps. Writing happens off every paced thread, so a save never costs you a frame, and a file is
rewritten only when rewriting it would change it.

## The typed callable: `Routine<Sig>`

`registerRoutine` returns a `Routine<Sig>` — a small copyable value handle you call like a function.
It refers back to its `Vm`, so keep the `Vm` alive for as long as you hold the routine (the same
lifetime rule as the renderer's `AtlasId` / `PaletteId`). Arguments and the return value must be
`uint8_t` / `uint16_t` / `uint32_t` (or `void` return).

## Ready-made presets: `retropp::sameboy`

A routine gets a preset only when it is a **hardware technique rather than an algorithm** — an
operation whose form is dictated by the instruction set and the register it touches, so any
independent implementation writes the same instructions. Those the platform ships: authored as a `.asm`
file the build assembles to bytecode and bakes into the binary, then registered and bound for you.
You pass nothing but the `Vm&`:

```cpp
auto rng = retropp::sameboy::divRng(vm);  // ldh a,[rDIV]; ret — a raw DIV read (stateless)
std::uint8_t x = rng();
```

`divRng` returns the free-running DIV register as a random byte. It is stateless, so the stream is
only as varied as the divider is — advance the clock between calls or it barely moves.

**Anything with design choices in it is yours, not the platform's.** A mixing scheme, a seed layout, a
compression format — those belong to the game that authors them, and the platform's job is the machine,
the binding and the clock. Write the `.asm`, point `registerRoutine` at it, and it is baked into your
binary exactly as a preset is. `examples/vm_routines` does that with an RNG of its own: an 8-bit
xorshift over a high-RAM seed it picks and seeds itself, mixed with the divider on the way out, shown
side by side with `divRng` so the two paths are visible together.

A purely-algorithmic PRNG that reads no hardware register belongs in neither place: it is
byte-reproducible in plain C++, so it is a porting job rather than a VM routine.

## Where to change things

- **Add a routine preset for the Game Boy family:** write the routine as a `.asm` file under
  `src/vm/gameboy/routines/`, add it to the build's routine-bake list so it is assembled to compile-time
  bytecode (`src/vm/gameboy/gb_routine_bytecode.h`), then add a factory (declared in
  `include/retropp/gb_routines.h`, defined in `src/vm/gameboy/gb_routines.cpp`) that registers the baked
  byte span with `uploadRoutine` and builds the binding — mirror `divRng`. Only add one if the
  routine is a hardware technique; an algorithm belongs to the game, registered from its own `.asm`.
- **Add register/memory vocabulary for the Game Boy family:** extend `include/retropp/gb.h` — CPU
  registers as `Location` constants, hardware memories as `MemoryRegion` constants. A new memory also
  needs the backend to serve it: a `GbHardwareMemory` value, its direct-access mapping in
  `src/vm/gameboy/sameboy_machine.cpp`, and its range in `regionFor` / `regionIsAddressable`
  (`src/vm/gameboy/sameboy_backend.cpp`). Updating one and not the others reddens the suite.
- **Add a whole new system (NES, Genesis, …):** add a `src/vm/<system>/` folder with its backend and
  its `detail::<system>Core` hook, its `VMPlatform` enumerators, its `detail::coreFor` case and its
  pre-bound `Vm::` types in `vm.h` — the public `vm.h` surface does not change. Every system's machine
  idiom stays behind its own backend; `vm.h` stays system-agnostic.

## Status

Available: the Game Boy / Game Boy Color backend, both registration forms — `uploadRoutine`
(pre-assembled bytes) and `registerRoutine` (a `.asm` file the platform assembles in-process, with the
`Embed` / `LoadFromPath` policy) — the built-in SM83 assembler, the `Location` / `RoutineBinding`
surface, the `gb::` register vocabulary, the `divRng` preset, `advanceTick` / `advanceClock` (the
free-running-divider model), the host-speed / single-instance path, the `HardwareSpeed` throttle
(driving the [audio chain](../audio.md)), and the resident-driver surface (`hostDriver` / `tickDriver` /
`readSlot`, with banked placement via `gb::banked` + `gb::Mbc3`), and cartridge hosting (`hostRom`
with the `MemoryRegion` / `registerRegions` / `read` / `write` surface and the `gb::` memory
constants); and the SNES backend — `Vm::SNES`, hosting a whole cartridge with its pad vocabulary in
`snes.h` (`co-execution.md`). Declared seams: `instances > 1` (registering with more than one throws
`std::logic_error`) and binding a location by label name.
