# The Game Boy family

`Vm::GB` is a Game Boy and `Vm::GBC` a Game Boy Color: the same core, SameBoy, behind one backend
(`src/vm/gameboy/`), constructed for `VMPlatform::GameBoy` or `VMPlatform::GameBoyColor`. Both run
`Isa::Sm83`, so a routine's bytes and a chiptune driver run on either. This page is what the family is
behind the surface: its clock, its vocabulary in `gb.h`, the facts its hardware imposes on a program,
its save, its presets and its examples. The verbs themselves are on the two capability pages — the
routine surface in [vm-and-routines.md](vm-and-routines.md), a running cartridge in
[co-execution.md](co-execution.md) — and every one of them runs on this family.

```cpp
#include "retropp/vm.h"           // Vm::GB, Vm::GBC, VMPlatform::GameBoy / GameBoyColor
#include "retropp/gb.h"           // gb::A … gb::PC, gb::VRam … gb::Io, gb::banked, gb::Mbc3, gb::Button, gb::held
#include "retropp/gb_routines.h"  // retropp::sameboy::divRng
```

## Contents

- [Two models, one core](#two-models-one-core)
- [Its clock](#its-clock)
- [`gb.h` — the vocabulary](#gbh--the-vocabulary)
  - [Registers](#registers)
  - [The machine's memories](#the-machines-memories)
  - [Bank-qualified addresses: `gb::banked`](#bank-qualified-addresses-gbbanked)
  - [The mapper: `gb::Mbc3`](#the-mapper-gbmbc3)
  - [The pad](#the-pad)
- [What the hardware imposes](#what-the-hardware-imposes)
- [The assembler's dialect](#the-assemblers-dialect)
- [Its save: `.sav`](#its-save-sav)
- [Presets: `retropp::sameboy`](#presets-retroppsameboy)
- [Examples](#examples)
- [Where the files are](#where-the-files-are)

## Two models, one core

```cpp
Vm::GB  dmg;                                           // VMPlatform::GameBoy,      TimingProfile::GameBoy
Vm::GBC cgb{VmConfig{.key = "adventure", .video = true}};  // VMPlatform::GameBoyColor, TimingProfile::GameBoyColor
```

A `Vm::GB` is the monochrome model. A `Vm::GBC` runs a CGB-flagged image in CGB mode and anything else
in DMG compatibility, exactly as the hardware decides it from the image's own header. Both spellings are
`Vm`s with the console fixed at the type; `Vm{VMPlatform::GameBoyColor, VmConfig{…}}` is the same
machine with the console named at construction.

**The core answers every verb on both capability pages:** routines from bytes or `.asm`, the
free-running clock between calls, the resident driver, a hosted cartridge with its places named and its
code escaped, watched and called, video, sound, the pad and the save. Two refusals are the family's own,
both `std::logic_error`: a machine hosting a cartridge refuses `uploadRoutine` / `registerRoutine`
(a game's image has no arena to place a routine into), and a machine hosting a resident driver refuses
`hostRom` (the driver's image is one the platform built, and a game's cartridge cannot share it).

## Its clock

| | |
|---|---|
| CPU clock | 4'194'304 Hz, on both models |
| one frame | 70'224 cycles — 16'742'706 ns, 59.7275 Hz |
| double speed (Color) | 140'448 cycles a frame at the same refresh — a larger budget the guest asks for, not a second clock |
| run-loop profile | `TimingProfile::GameBoy` / `TimingProfile::GameBoyColor` — the same period; `TickPeriodNs::GameBoy` |

`TimingProfile::GameBoyColor` is the run loop's default, so a Game Boy machine advanced on the loop's
tick runs one of its own frames per tick with nothing to configure. A frame *is* 70'224 cycles; the
stored count is the exact fact and the nanosecond period the rounded one, which is why
`advanceTick()` spends the frame count at the machine's own cadence and derives from the rate only when
the loop runs at another period
([vm-and-routines.md](vm-and-routines.md#a-machine-running-at-a-cadence-that-is-not-its-own)).

## `gb.h` — the vocabulary

`vm.h` is system-agnostic; `gb.h` is the family's half of the surface — the register file, the
hardware's memory areas, bank-qualified addressing, the common mapper and the pad — as the typed
constants a binding, a declaration or an `ActionMap` names.

### Registers

```cpp
enum class Reg : std::uint16_t { A, F, B, C, D, E, H, L, AF, BC, DE, HL, SP, PC };
```

The SM83 register file, as `Location` constants: `gb::A gb::F gb::B gb::C gb::D gb::E gb::H gb::L`
are 8-bit, `gb::AF gb::BC gb::DE gb::HL gb::SP gb::PC` 16-bit. The enumerator order is the backend's
register id and fixes each register's width, so a binding that puts a `std::uint16_t` in `gb::A` is
refused at registration (`std::invalid_argument`), as is one naming a register this machine does not
have. A memory location in the family's map is the generic `Location::memory(address)`.

### The machine's memories

The areas that are the same size on every cartridge, as `MemoryRegion` constants — read or write one
whole, or name a piece of one at an address inside it:

| Constant | Where | Size | What |
|---|---|---|---|
| `gb::VRam` | `0x8000` | `0x2000` | tile data and maps — the mapped view; on the Color the second bank is reached the way the machine reaches it |
| `gb::WorkRam` | `0xC000` | `0x2000` | work RAM |
| `gb::Oam` | `0xFE00` | `0x00A0` | the forty sprite entries — the mapped view |
| `gb::Io` | `0xFF00` | `0x0080` | the hardware registers **as stored** — see below |
| `gb::Hram` | `0xFF80` | `0x007F` | high RAM |

Each has `count == 1`, so `vm.read(gb::VRam)` with no index hands back the whole area. The cartridge is
not among them — its size is whatever image is hosted — so a place inside it is named with an address.

**`gb::Io` is raw storage, not the CPU's view.** Several registers are synthesized when the CPU reads
them: `rDIV` at `0xFF04` is answered from the divider counter, and others carry bits that always read
high. A region read of `gb::Io` returns the stored byte, which for those is not what a routine reading
the same address sees. Read a synthesized register through a routine (`ldh a, [rDIV]`). The four RAM
areas have no such gap.

### Bank-qualified addresses: `gb::banked`

```cpp
constexpr std::uint32_t banked(unsigned bank, std::uint16_t addr16) noexcept;
```

A place, an escape, a routine to bind or a driver image's base in a switchable ROM bank is named as the
window address `addr16` (`0x4000`–`0x7FFF`) seen from CPU bank `bank`; the bank rides the high sixteen
bits of the 32-bit value and the backend decodes it to a cartridge offset. `bank` is at least 1 — bank 0,
the fixed region `0x0000`–`0x3FFF`, is a plain address. A declaration reaches into any bank; **a call
into a banked routine goes through only while its bank is the mapped one** (`std::logic_error` naming
both banks otherwise), because selecting a bank is the guest's own act through the mapper, never the
platform's.

### The mapper: `gb::Mbc3`

`gb::Mbc3` is the MBC3 cartridge mapper (MBC3 + TIMER + RAM + BATTERY, cartridge-type byte `$10`), the
common mapper for a banked cartridge. A resident driver placed across banks declares it on its
`DriverBinding`; the backend sizes the cartridge it builds to the highest placed bank, and the driver's
own writes to the mapper registers switch banks exactly as on hardware. A flat driver leaves the mapper
at its default.

### The pad

```cpp
enum class Button : ActionId { Right, Left, Up, Down, A, B, Select, Start };
```

The eight buttons as an Actions enum, bound in an `ActionMap` like any action and read back each tick
with `gb::held(input)` as a `gb::Buttons` — eight `bool`s, `right` … `start` — which `vm.buttons` takes.
The machine takes the joypad's own bit order: the four directions in the low nibble, the four action
buttons in the high one. `gb::held` counts a button as down if it is held at the tick **or was pressed
since the last one**, so a tap shorter than one tick still reaches the guest as one frame of held. A
game with actions of its own numbers them clear of these eight; there is one action space, 64 wide.
The binding and the tick are on [co-execution.md](co-execution.md#input-playing-it).

## What the hardware imposes

Facts a program meets on this family that the capability pages state in the console-agnostic form; here
is what they are on this hardware.

- **Booting seeds the firmware-exit state; no boot ROM is shipped or loaded.** `run()` resets the
  machine, then reproduces what the boot firmware leaves — the boot overlay unmapped, the mode the
  image's header selects, the documented registers, including the header title checksum a
  DMG-compatibility boot of a licensed image hands over in B — through the machine's own bus, so every
  latch performs. Execution begins at the cartridge's entry point.
- **Power-on RAM is filled the way hardware fills it**, and `reset()` does not restore RAM to a fixed
  state. A seed a routine reads is written first; a value the guest should find is written after `run()`.
- **A registered routine is placed in the arena** — the boot-safe window `0x0160`–`0x01FF` of a
  cartridge the platform builds for it. A machine hosting a game's cartridge has no arena, so
  `uploadRoutine` / `registerRoutine` throw `std::logic_error` there; an arena with no room left for a
  routine throws `std::runtime_error`.
- **A hosted image's header is the mapper and the size.** SameBoy reads it — the platform reads none of
  it — and rounds the image up to a power-of-two bank count, so the addressable size is the one the
  machine reports back, not the byte count handed in.
- **The free-running divider moves only while the machine runs.** A routine called for a value runs the
  machine for exactly that call, so between calls `rDIV` stands still unless the program advances the
  clock — `advanceTick()` once per tick, or `advanceClock(cycles)` — which moves the timing and divider
  state and nothing a routine marshals
  ([vm-and-routines.md](vm-and-routines.md#time-based-registers-advance-the-clock-between-calls)).
- **Accesses are byte-granular.** One `ld [$C300], sp` fires a watch twice, at `$C300` and `$C301`, each
  with its own byte.
- **An instruction fetch is a read**, so a watch on an address holding code answers the fetch, and
  `instead(v)` there substitutes the opcode that executes. A watch and an escape at one address order
  themselves fetch first, then the escape.
- **A banked entry is reachable only while its bank is mapped** — above, under `gb::banked`.

## The assembler's dialect

`registerRoutine` and `assemble` run SM83 source through the built-in assembler, in the conventional
Game Boy dialect the published disassemblies are written in: `;` comments, `label:` definitions, `$hex`
/ `%bin` / decimal literals, `[hl]` / `[$FF04]` memory operands, condition codes, and the standard
instruction set. The hardware registers are predefined by name, so a routine writes `ldh a, [rDIV]`
rather than `ldh a, [$FF04]`, and labels resolve across the routine. The two registration forms and the
`Embed` / `LoadFromPath` policy are on
[vm-and-routines.md](vm-and-routines.md#authoring-in-assembly-register-from-a-asm-file).

## Its save: `.sav`

A keyed machine keeps the cartridge's battery-backed memory — what the image's own header declares it
has — and the file is the `.sav` every other program that reads one expects:

```
<your game's per-user data directory>/VM/<key>/battery.sav
```

`batterySave(name)` names the file per cartridge for a machine that plays whatever it is handed
(`VM/<key>/<name>.sav`). The mechanism — read back as the image is hosted, written after the guest
changes it, off every paced thread — is on
[vm-and-routines.md](vm-and-routines.md#keeping-what-the-guest-writes).

## Presets: `retropp::sameboy`

A routine is shipped as a preset only when it is a hardware technique — an operation whose SM83 form the
instruction set and the register it touches dictate, so any independent implementation writes the same
instructions. The family ships one:

```cpp
auto rng = retropp::sameboy::divRng(vm);  // ldh a,[rDIV]; ret — a raw DIV read, stateless
std::uint8_t x = rng();
```

`divRng` returns the free-running divider as a random byte; the stream is only as varied as the divider
is, so advance the clock between calls. Its source is `src/vm/gameboy/routines/div_rng.asm`, assembled at
build time into `src/vm/gameboy/gb_routine_bytecode.h`. Anything with design choices in it — a mixing
scheme, a seed layout — is the game's own `.asm`, registered through `registerRoutine`
([vm-and-routines.md](vm-and-routines.md#ready-made-presets-retroppsameboy)).

## Examples

| | |
|---|---|
| `examples/vm_routines` | `divRng` beside the program's own xorshift routine, registered from its own `.asm` |
| `examples/driver_hosting` | two synthetic resident sound drivers on one `AudioSystem`, driven through the same control column |
| `examples/driver_mixed_images` | one driver built from a tick routine read out of a cartridge and a setup routine baked from `.asm` |
| `examples/cartridge_assets` | an authored cartridge hosted for its tile art and name table, patched in place, `gb::WorkRam` read |
| `examples/rom_run` | a cartridge run and measured at four speeds, a write round-tripped through its loop, parked and resumed |
| `examples/guest_escape` | an escape counting the guest's loop, then a routine replaced and answered natively |
| `examples/guest_nesting` | a replacement calling the cartridge's own generator from inside the escape; a decompressor called parked |
| `examples/coexecution` | windowed: every co-execution verb acting on one picture |
| `examples/gb_player` | windowed: one ROM on two machines, one on the tick and one free-running, both as layer content |

The long descriptions are on [co-execution.md](co-execution.md#try-it). Every cartridge among them is
authored in-code or by a committed generator, except the player's, which takes yours at runtime.

## Where the files are

| What | Where |
|---|---|
| The vocabulary | `include/retropp/gb.h` |
| The presets | `include/retropp/gb_routines.h`, `src/vm/gameboy/gb_routines.cpp`, `src/vm/gameboy/routines/*.asm`, `src/vm/gameboy/gb_routine_bytecode.h` |
| The backend — register ids, the memory map, the arena, the hooks | `src/vm/gameboy/sameboy_backend.cpp`, `sameboy_backend.h` |
| The machine over the core | `src/vm/gameboy/sameboy_machine.cpp`, `sameboy_machine.h` |
| The SM83 assembler | `src/vm/gameboy/sm83_assembler.h` |
| The hardware register names the assembler predefines | `src/vm/gameboy/gb_symbols.h` |
| The core | `third_party/sameboy` |

Adding a preset or extending the vocabulary — a register, a memory area, and the backend mapping each
needs — is on [vm-and-routines.md](vm-and-routines.md#where-to-change-things).
