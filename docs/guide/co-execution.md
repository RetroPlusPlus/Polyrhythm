# Co-execution — a cartridge running inside your game

```cpp
#include "retropp/vm.h"            // Vm, MemoryRegion declarations, bindRoutine, run/speed/stop
#include "retropp/guest_escape.h"  // GuestEscape, escapes(), the escape table
#include "retropp/guest_watch.h"   // GuestWatch, AccessVerdict, watches(), the watch table
#include "retropp/guest_frame.h"   // GuestFrameContent — a machine's video as layer content
#include "retropp/memory_region.h" // MemoryRegion — where a place is
#include "retropp/gb.h"            // gb::A … gb::PC, gb::VRam … gb::Hram, gb::banked
```

Your game hands the platform a cartridge image and gets a whole machine: one that boots, runs its own
code at its own speed on a thread of its own, and whose memory and code your program reaches into
while it lives. Native code and the cartridge's code call each other as subroutines, nested to any
depth, each side resuming exactly as it was.

That is a general instrument, not a feature with one use. A console's CPU is at your program's
disposal: you decide what it runs, when it runs, how fast, what it reads, what its routines answer,
and which of your functions its own code calls. What you build on top of that — an enhanced port, a
game that extends a cartridge it ships beside, a tool that pulls content out of one — is yours.

This is the deep end of **Conductor**, the platform's VM layer — the routine surface is
[vm-and-routines.md](vm-and-routines.md).

**One property holds across all of it: the image is never modified.** Everything here happens against
bytes exactly as they shipped, in memory this process owns, with the behaviour living in your code.

> **Platform support today: the Game Boy and Game Boy Color only.** Every verb on this page is
> available on `Vm::GB` and `Vm::GBC`, and constructing a `Vm` for any other `VMPlatform` throws.
> **More consoles are planned**, and the surface is built for them: nothing on this page is
> Game-Boy-shaped except the `gb::` vocabulary it names registers and memory areas with. A second
> console brings its own backend and its own `<console>::` header, and the verbs, the declarations and
> the calling conventions read identically. See [Status](#status).

## Contents

- [The shape of it](#the-shape-of-it)
- [Hosting a cartridge](#hosting-a-cartridge)
- [Naming places in it](#naming-places-in-it)
- [Reading and writing them](#reading-and-writing-them)
- [The machine's own memories](#the-machines-own-memories)
- [Running it](#running-it)
  - [Advancing it on your own tick instead](#advancing-it-on-your-own-tick-instead)
- [Video: showing its picture](#video-showing-its-picture)
- [Input: playing it](#input-playing-it)
- [While it runs: one thread owns the machine](#while-it-runs-one-thread-owns-the-machine)
- [Escaping into your own code](#escaping-into-your-own-code)
- [Replacing a routine the cartridge calls](#replacing-a-routine-the-cartridge-calls)
- [Managing the escape table](#managing-the-escape-table)
- [Deciding the guest's own reads and writes](#deciding-the-guests-own-reads-and-writes)
- [Managing the watch table](#managing-the-watch-table)
- [Calling the cartridge's own routines](#calling-the-cartridges-own-routines)
- [Nesting: the whole loop](#nesting-the-whole-loop)
- [What it costs](#what-it-costs)
- [Failure modes worth knowing](#failure-modes-worth-knowing)
- [Try it](#try-it)
- [Where to change things](#where-to-change-things)
- [Status](#status)

## The shape of it

Seven verbs, and everything else is composition:

| | |
|---|---|
| `hostRom(bytes)` | put an image on the machine and make every byte of it addressable |
| `registerRegions(...)` · `read` · `write` | name the places you care about, and move their bytes |
| `run()` · `speed(num, den)` · `stop()` | boot it, pace it, park it |
| `registerEscapes(...)` | name places in the cartridge's **code** where control leaves the guest for yours |
| `registerWatches(...)` | name places in the cartridge's **memory** whose reads and writes your code decides |
| `bindRoutine<Sig>(at, binding)` | call the cartridge's own routines from your code, in the guest's own context |
| `video(on)` · `video()` | let it draw, and put the frames it finishes on a layer |
| `buttons(held)` | hold the guest's buttons, so the cartridge can be played |

The first three reach into the machine from outside. The next two are the machine reaching out to
you — one keyed on the code it is about to execute, one on the memory it is about to touch.
`bindRoutine` is you reaching back in. Together they close the loop: guest code calls your function,
your function calls a routine of the cartridge's, that routine's own execution hits another escape,
and so on, with the register file and the stack coming back untouched at every level.

`video` is the machine's output rather than a way in or out of it: what the machine draws, handed to
your frame as content you compose with.

A `Vm` hosts a game's cartridge **or** a platform-built driver image, never both — `hostDriver`
synthesizes an image the platform owns, and `hostRom` takes one your game owns. On a hosted cartridge
`uploadRoutine` and `registerRoutine` throw, since a game's image has no arena to inject into. Use two
`Vm`s if you need both.

## Hosting a cartridge

```cpp
Vm::GBC vm;
vm.hostRom(image);          // std::span<const std::uint8_t> — bytes, never a path
```

Bytes rather than a path, so where they came from stays open. Registering the image with
`registerData` and passing `data(id)` is the usual route, and it lands on that family's
`LoadFromPath` default — which keeps a cartridge out of your shipped binary unless you say otherwise.

This makes the image **readable**. Running it is the next verb, and a hosted image that is never run
is still fully addressable — which is how you pull content out of a cartridge without playing it.

The platform exposes no cartridge metadata: no title, no mapper, no size. Those are console-shaped, and
you are holding the bytes already.

## Naming places in it

Declare the places you care about as one batch. Every entry is checked when the batch registers, so a
table of two hundred is answered once instead of failing one at a time during play — and a bad batch
names **every** bad entry, not just the first:

```cpp
struct Places {
    MemoryRegion tiles;
    MemoryRegion names;
};

const auto places = vm.registerRegions(regions(
    region(&Places::tiles, MemoryRegion{.at = 0x1000, .size = 16, .count = 384}, "tile art"),
    region(&Places::names, MemoryRegion{.at = 0x2000, .size =  8, .count =  64}, "tile names")));
```

A `MemoryRegion` says where a place starts, how big one entry is, and how many entries follow.
`count` defaults to 1, so a single blob is the ordinary case. The struct is a vocabulary — it is never
instantiated — and naming a field that is not in it is a compile error rather than a bad address.

Each declaration carries a name because a pointer-to-member has none at runtime. For a batch generated
from a disassembly's symbol file, that name is what makes a failure readable.

For a place in a higher bank, qualify the address with `gb::banked(bank, addr16)` — the same encoding
a placed driver image's base uses.

## Reading and writing them

```cpp
const std::vector<std::uint8_t> tile = vm.read(places, &Places::tiles, 12);
vm.write(places, &Places::tiles, patched, 12);
```

The bytes are yours — a plain buffer, not a catalogued handle. Hand it to `uploadData` if you want it
catalogued, or convert it and hand the result to `uploadAtlas`. What the bytes *mean* is never the
the platform's business: a tile in the hardware's two-bitplane layout comes back as sixteen bytes, and
decoding them is your code's job.

Entries resolve in the machine's own decoded address space, so **an array longer than a bank reads
correctly across the boundaries**. A few hundred entries of a couple of kilobytes runs far past the
0x4000 switchable window, and `at + index * size` stops describing that run at the first boundary —
which is why the index is a parameter rather than something you compute yourself.

Writing works everywhere, including into a hosted cartridge: the image is a buffer your process owns,
and patching one is what extending a cartridge means. The write lands in memory only — the file the
bytes came from is untouched, and re-hosting replaces the image.

Most real content is not tabular — a pointer table names variable-length blobs — so a place can also
be built on the spot from bytes you just read, and used without being declared:

```cpp
const std::vector<std::uint8_t> entry = vm.read(places, &Places::pointers, 3);
const std::uint32_t at = entry[0] | (entry[1] << 8);

const MemoryRegion blob{.at = at, .size = length};
const std::vector<std::uint8_t> bytes = vm.read(blob);
```

Those are checked at the call instead of at registration, and they need the machine **stopped** — see
[While it runs](#while-it-runs-one-thread-owns-the-machine).

## The machine's own memories

`gb.h` ships the hardware's areas as `MemoryRegion` constants, exactly as it ships `gb::A` as a
`Location`:

```cpp
const std::vector<std::uint8_t> vram = vm.read(gb::VRam);
```

`gb::VRam`, `gb::WorkRam`, `gb::Oam`, `gb::Io`, `gb::Hram`. Each has `count == 1`, so reading one with
no index hands back the whole area, and each can be declared in a batch like any other place.

A cartridge is 32 KiB or a megabyte depending on the image you host, so the header ships no constant
for it: name a place inside it with an address.

**`gb::Io` is raw storage, not the CPU's view of the hardware registers.** Several registers are
*synthesized* when the CPU reads them rather than kept as a byte — `rDIV` at `0xFF04` is answered from
the divider counter, and others carry bits that always read high. A region read hands back what is
stored, which for those is not what a routine reading the same address would see. Read a synthesized
register the way the machine does, through a routine:

```cpp
auto divider = vm.bindRoutine<std::uint8_t()>(kReadDiv, RoutineBinding{.output = gb::A});
```

The plain RAM areas — `gb::VRam`, `gb::WorkRam`, `gb::Oam`, `gb::Hram` — have no such gap; what is
stored is what the CPU reads.

A single byte is the one-byte case of a place, so whatever a place may name, a routine's memory
binding may name too — the two never disagree about what memory a machine has.

## Running it

```cpp
vm.run();          // boot to the platform's firmware-exit state; run on a thread of its own
vm.speed(2, 1);    // execution speed as a fraction of the hardware's own — {2,1} is double
vm.speed(0, 1);    // {0,1} is paused; the factor is adjustable at any time
vm.stop();         // park where it is — run() again resumes; reset() first for a fresh boot
```

Booting seeds the state the platform's own boot firmware leaves — the boot overlay unmapped so the
cartridge answers everywhere it maps, the mode the image's own header selects, the documented
firmware-exit registers — and execution begins at the cartridge's entry point. On a Game Boy Color
machine, a CGB-flagged image runs in CGB mode and anything else in DMG compatibility, exactly as the
hardware decides it. **No boot ROM is shipped or loaded**; the state is seeded directly.

The machine paces itself against the wall clock: it owes the platform's cycles-per-second times the
factor, steps while it lags what it owes, and parks when it has caught up — so it holds the
hardware's cadence whether or not your game's loop keeps up. A factor of `{num, den}` is exact
arithmetic, the sub-cycle remainder carried rather than rounded at any speed. A factor change steers
the machine's next pacing decision; a step already in flight completes first, so a pause settles one
step after the call.

The machine runs headless. What it is doing is observed through the declared places.

### Advancing it on your own tick instead

`run` takes which clock drives the machine. The default is the one above — a thread of its own at the
hardware's cadence. The other hands the clock to your game:

```cpp
vm.run(Vm::Advance::OnTick);
// then, once per tick of your own loop:
vm.advanceTick();
```

`advanceTick` *is* the step: queued writes and table changes land, the machine runs the cycles the
tick is worth, and the declared places publish — all on the thread that called it, before the call
returns.

What a tick is worth is the machine's own clock rate against the period being run, scaled by the
speed factor: `{2,1}` advances two ticks' worth per tick, `{0,1}` advances none. Both conversions are
exact, each carrying its sub-cycle remainder rather than rounding it, so a machine ticked ten thousand
times has run exactly the cycles those ticks were worth.

One image, one sequence of ticks and one factor put the machine in one state, byte for byte, on every
run and every machine — how the host scheduled your loop does not reach the result. That makes the
guest's execution part of your simulation: it follows a slow-motion tick, holds still through a paused
one, and replays from the same inputs.

A machine running on its own thread refuses `advanceTick` — it keeps its own clock.

## Video: showing its picture

A hosted machine produces nothing until you ask it to. Ask for video, and the frames it finishes
become a layer's content:

```cpp
vm.hostRom(rom);
vm.video(true);                 // this machine draws
vm.run(Vm::Advance::OnTick);

// once per tick:
vm.advanceTick();

// and in the frame you submit:
DrawLayer screen{.key = "screen"};
screen.z       = 0;
screen.size    = PixelSize{160, 144};
screen.content = vm.video();    // the last complete frame it drew
```

That layer composites by `z` like any other, so native layers go over or under a machine's screen —
your own HUD above it, your own background behind it, a transform or a per-layer effect on it — and
the machine knows nothing about any of it.

**Video is off by default because a raster costs cycles.** A machine hosted for its memory, its
routines or its driver never draws a pixel and never pays for one. `video(true)` turns that cost on
per machine and `video(false)` turns it back off; both are idempotent.

**Declare it at construction if you know then**, which is the same setting reached the other way:

```cpp
Vm vm{VMPlatform::GameBoyColor, VmFeatures{.video = true}};
```

A machine declared this way is drawing before it hosts anything. `VmFeatures` is where a machine's
outputs are named, so each one a machine does not use costs it nothing.

**Asked of a RUNNING machine, the change lands at its next step boundary** — where every change issued
to a running machine lands, so the machine is never asked to install anything while its own thread is
using it. Reading the video in that window answers the empty picture rather than throwing: you asked,
so you are early rather than mistaken.

**Either clock draws.** A machine advancing on your tick steps on the thread that called `advanceTick`,
so its picture is written where you read it. A machine advancing on a clock of its own steps on its own
thread and hands each finished frame across; you see completed frames either way, and never a frame
being drawn.

**When a frame becomes visible is what each clock offers.** A tick-advanced machine answers from the
tick boundary: it may finish a frame part-way through a tick, and `video()` keeps answering with the
previous one until `advanceTick` returns — the same boundary queued writes and table changes land at.
A machine on its own clock has no such boundary, so it answers with the newest frame it has finished.
Either way, asking twice in one frame gives the same picture both times.

**One tick is one frame, when the periods agree.** The Game Boy tick period *is* the machine's frame
period, so a Game Boy advanced on a Game Boy tick draws exactly one frame per tick with nothing to
drift. Run it at another period, or at another speed factor, and a tick becomes worth a fraction or
several of its frames — the platform shows the last one it completed, and holds that picture through a
tick where it completed none. A machine slower than the display repeats a frame rather than blanking,
which is what a console on a faster screen does.

**What the picture carries is the machine's, not the platform's:** its own `width` and `height`, its
own `GuestPixelFormat`, and `contentChanged` — the platform's answer for whether this tick brought a
new one. Nothing asks a machine how fast it runs or which frame it is on. `pixels` is valid until the
next `advanceTick`, which is exactly the lifetime a submission needs.

The content type itself is in [draw-state.md](draw-state.md#guestframecontent--a-hosted-machines-picture).

## Input: playing it

The guest's buttons are an Actions enum the platform ships, because a console's button set is fixed by
its hardware. You bind them the way you bind any action:

```cpp
ActionMap controls{
    {gb::Button::A,      {SDL_SCANCODE_X, PadButton::FaceLabelA}},
    {gb::Button::B,      {SDL_SCANCODE_Z, PadButton::FaceLabelB}},
    {gb::Button::Start,  {SDL_SCANCODE_RETURN, PadButton::Start}},
    {gb::Button::Select, {SDL_SCANCODE_RSHIFT}},
};
controls.add(presets::directional(gb::Button::Up, gb::Button::Down,
                                  gb::Button::Left, gb::Button::Right));
platform.actions(controls);
```

Then hand the machine what is held, each tick:

```cpp
loop.simTick([&](const InputState& input) {
    machine.buttons(gb::held(input));
    machine.advanceTick();
});
```

`gb::held(input)` reads those eight actions back as a `gb::Buttons` — the value the machine takes. It
is a pure read: the platform stores no binding of its own and decides nothing, so **rebinding is
editing `controls` and resubmitting it**, and costs no feature. A game with actions of its own numbers
them clear of the guest's; there is one action space, 64 wide.

**The state is a level and the whole set lands at once.** A button absent from the value is released,
so you submit what is held now rather than pressing and releasing. A tap shorter than one tick still
reaches the guest as one frame of held — the run loop accumulates it — and lasts exactly that frame,
so the cartridge's own code tells a tap from a hold the way it does on hardware.

It arrives at the next step boundary, like every other verb: a machine on your tick sees it at the
next `advanceTick`, one running on its own clock at its next step. A core with no input path throws.

## While it runs: one thread owns the machine

This is the rule that governs every other verb on this page, so it is worth stating once, plainly.

**A running machine belongs to the thread stepping it.** Under `Advance::Continuously` that is a
thread the platform owns, and every verb you issue *crosses* to it and lands at a step boundary. Under
`Advance::OnTick` the stepping thread is whichever one calls `advanceTick`, usually your own — and
every verb still lands at that same boundary, on the same terms.

| From your thread, while it runs | What happens |
|---|---|
| `read` of a **declared** place | answers the latest completed step's publish — every declared place captured at one instant, coherent with the others |
| `write` to a **declared** place | crosses to the machine's thread, lands at the next step boundary, in the order issued |
| `speed(num, den)` | steers the next pacing decision; a step in flight finishes first |
| `escapes()[key].armed(...)` / `.remove()` | crosses and lands at the next step boundary, in the order issued |
| `watches()[key].armed(...)` / `.remove()` | crosses and lands at the next step boundary, in the order issued |
| a place built **on the spot** | refused — declare it before `run()`, or `stop()` first |
| `bindRoutine`, `registerRegions`, `registerEscapes`, `registerWatches` | refused — these settle the terms, and the terms settle before the run |
| `reset`, `advanceClock`, `hostRom`, `enableAudio` | refused — `stop()` first |
| `advanceTick` | under `Advance::OnTick`, the step itself; under `Advance::Continuously`, refused — that machine keeps its own clock |
| calling a bound `Routine` | refused — call it from a handler, or `stop()` first |

**A verb that crosses is not visible to the read that follows it.** You write a byte, and a read on
the next line still answers the previous publish; you switch an escape off, and `armed()` still says
true until the boundary. Track what you asked for if you need it immediately — the machine will agree
one step later. This is one rule, not several: everything you issue lands at the same place.

**That holds under both clocks.** Sharing a thread with the machine does not shorten the wait: under
`Advance::OnTick` a read answers the last step's publish, so a write issued between ticks becomes
visible after the next `advanceTick`, not on the line after the write.

**`stop()` is a boundary too.** Parking lands everything you issued before it — the writes, the escape
and watch switches — in the order you issued them, so a verb never waits for a step that will not come.
Read the parked machine on the next line and the byte is there.

**Inside a handler, none of that applies to the handler's own code.** A handler already runs on the
machine's thread, so its escape- and watch-table changes apply at once, and a routine call is legal
there and nowhere else. Its *reads and writes* still ride the publish and the boundary, because those
are how the machine's memory is observed at all.

The machine also stays where it is in memory: `stop()` before moving the `Vm` object.

## Escaping into your own code

Reading and writing reach into the machine from your side. An escape is the other direction: a place
in the cartridge's own code where control leaves the guest, runs your C++, and resumes.

```cpp
vm.registerEscapes(escapes(
    GuestEscape{.key = "battle start", .at = gb::banked(0x03, 0x4A17), .handler = onBattleStart},
    GuestEscape{.key = "rng draw",     .at = 0xC310,                   .handler = onRngDraw}));
```

One batch, checked once, the way places are: every entry names an address this machine can reach,
carries something to run, and uses a key and an address no other escape has taken. A batch with bad
entries throws naming all of them, each by its key.

A handler takes the machine and the address that fired:

```cpp
void onRngDraw(retropp::Vm& machine, std::uint32_t at) {
    machine.write(places, &Places::lastRoll, roll);
}
```

**The instruction at that address still executes.** The handler runs to completion first, then the
guest executes the instruction it was about to execute, exactly once, whatever the handler did. A
`.handler` escape observes a place in the code; when you want the place *answered* rather than
observed, that is `.replaces`, below.

**It costs the guest none of its own time.** The handler runs on your clock, not the machine's — the
guest's cycle count does not advance for it, so a machine with escapes holds the same cadence as one
without.

**Whatever a handler touches, the handler owns the thread-safety of.** It fires on the machine's
thread while your game's thread is doing something else.

Declare escapes before `run()`, or between steps of a machine you drive yourself; declaring on a
machine that is already running throws.

## Replacing a routine the cartridge calls

`.replaces` declares that a native function answers **instead of** the routine at that address. While
the escape is live the guest never executes the routine's body; switch it off and the routine's own
body is what runs again.

```cpp
vm.registerEscapes(escapes(GuestEscape{
    .key      = "damage calc",
    .at       = kDamageCalc,
    .replaces = routine(RoutineBinding{.inputs = {gb::B, gb::C}, .output = gb::A},
                        [](std::uint8_t attack, std::uint8_t defense) -> std::uint8_t {
                            return myDamage(attack, defense);
                        })}));
```

The binding is a **transcription of the calling convention the routine already has** — discovered from
the cartridge's own code, not invented. Somewhere in the ROM its callers do this:

```
ld  b, [wAttackStat]    ; the caller loads the arguments
ld  c, [wDefenseStat]
call DamageCalc
ld  [wDamage], a        ; and reads the answer out of A
```

Inputs in B and C, answer in A — so the binding says exactly that. **The pairing is positional**: the
binding's list and the function's parameter list line up index for index, so `inputs[0]` — `gb::B` —
is read from the parked machine and arrives as the first parameter, `attack`; `inputs[1]` — `gb::C` —
arrives as `defense`; the return value goes to `output`. The function never names a register: by the
time it runs the marshalling has happened, and `attack` *is* B's value, as a plain `uint8_t` whose
width came from the signature.

**Which way the values flow.** This is `registerRoutine`'s binding vocabulary with the direction of
the *call* mirrored, and it is worth being exact about, because the words look the same:

| | who is calling | your arguments come from | your result goes to |
|---|---|---|---|
| `Routine<Sig>` — you call guest code | your C++ | your function arguments — the platform writes them **into** the bound inputs | the platform reads it **out of** the bound output for you |
| `.replaces` — guest code calls you | the cartridge | the bound inputs — the guest's own caller loaded them, the platform reads them **out** for you | the platform writes it **into** the bound output, where the guest's callers read |

When you replace a routine, the ROM is the caller and your function is the callee: the values in B
and C are whatever *that call site* loaded, and your answer lands in the register its compiled code
was always going to read. A different call site invoking the same routine gets the same treatment —
your function answers for all of them, exactly as the original routine did.

**The marshalling is synchronous.** Unlike a `.handler`'s reads and writes, which ride the publish and
the step boundary, a replacement is marshalled while the machine is parked at the entry — so the
caller reads a correct answer in the *same step*, and register conventions work. Routines whose
convention is memory-based bind memory the same way: `Location::memory(0xC000)` in place of a
register.

What remains yours: the replacement answers for **every** caller of that routine, and whatever the
original promised its callers — every output, every side effect — is your function's to establish
through the binding.

## Managing the escape table

The machine answers for its escapes by key:

```cpp
vm.escapes()["rng draw"].armed(false);   // declared, dormant
vm.escapes()["rng draw"].armed();        // whether it is live
vm.escapes()["rng draw"].remove();       // drop the declaration entirely
vm.escapes().contains("rng draw");
vm.escapes().size();
```

Escapes are live when declared. Switching one off keeps its declaration, so switching it back on
needs no re-declaration — and a machine whose escapes are all off costs exactly what a machine with
none costs, because there is nothing left for it to watch.

**Switching and removing work while the machine runs**, and land at the next step boundary in the
order issued, like every other verb ([above](#while-it-runs-one-thread-owns-the-machine)) — so a
switch is not visible to the `armed()` that follows it. Issued from inside a handler the switch
applies immediately instead, because that code already runs on the machine's own thread: a handler can
switch another escape off and the guest will not reach it in that same instruction stream.

A key this machine does not declare throws `std::out_of_range` naming the key, at the call that made
the mistake.

## Deciding the guest's own reads and writes

An escape names a place in the cartridge's **code**. A watch names a place in its **memory**: when
the guest reads or writes a byte there, your code runs and says what that access does — whichever
instruction made it.

```cpp
vm.registerWatches(watches(
    GuestWatch{
        .key     = "hp",
        .at      = MemoryRegion{.at = 0xC0A2, .size = 1},
        .onWrite = [&](Vm&, std::uint32_t at, std::uint8_t value) {
            if (shieldUp)       return AccessVerdict::veto();            // the write never lands
            if (value < kFloor) return AccessVerdict::instead(kFloor);   // this byte lands instead
            return AccessVerdict::proceed();                             // as the cartridge intended
        }}));
```

One batch, checked once, the way places and escapes are: every entry names a place this machine can
reach, carries at least one handler, and uses a key and a place no other watch has taken. A batch
with bad entries throws naming all of them, each by its key.

**The handler returns a verdict**, and the decision is made per access rather than per declaration:

| | |
|---|---|
| `AccessVerdict::proceed()` | the access happens as the cartridge intended |
| `AccessVerdict::veto()` | the write never lands, and nothing the store would have done to hardware happens |
| `AccessVerdict::instead(v)` | `v` is used in place of the byte the access carried |

"Freeze HP only while the shield is up" is therefore a branch inside one handler, not a watch armed
and disarmed from your thread every time the condition moves.

On a read, `veto()` gives the same result as `proceed()`: a read answers with a byte, so the guest
receives the one the machine holds. Use `instead(v)` to change what it sees.

**Declare the direction you want.** A watch carries `.onRead`, `.onWrite`, or both — at least one, or
the entry is refused by its key. The platform arms only the direction declared, which is worth
declaring precisely: a memory access is the hottest path a machine has.

```cpp
GuestWatch{.key    = "coin counter",
           .at     = MemoryRegion{.at = 0xC0B4, .size = 1},
           .onRead = [](Vm&, std::uint32_t, std::uint8_t) { return AccessVerdict::instead(99); }}
```

**A watch is declared over a place, not an address.** `MemoryRegion` is the same value `read` and
`write` name places with, so a span works exactly as it does there and every byte of it is watched.
The handler is handed the address of the byte that moved, so one handler serving a party block knows
which member it was:

```cpp
GuestWatch{.key     = "party hp",
           .at      = MemoryRegion{.at = 0xC0A2, .size = 2, .count = 6},   // six entries, watched
           .onWrite = [](Vm&, std::uint32_t at, std::uint8_t value) { … }}
```

Two watches may not name overlapping places, so which watch answers for a byte is never in question.
A place whose reads *and* writes you want is one watch declaring both handlers.

**Whose accesses fire it.** The cartridge's own code is always watched, including guest code running
inside a `bindRoutine` call — that is still the guest executing. Your own `read` and `write` are
watched when the watch asks for them:

```cpp
GuestWatch{.key     = "hp",
           .at      = MemoryRegion{.at = 0xC0A2, .size = 1},
           .from    = AccessSource::GuestAndGame,   // default is AccessSource::Guest
           .onWrite = onHpWrite}
```

That is for a game whose own logic keys off writes and wants its own writes to drive it too. A
`GuestAndGame` watch names a place the CPU addresses directly, so a bank-qualified place is refused
by its key.

The platform's own stores are never watched under either value — seeding a booted image, planting a
return landing, and the store that realizes `instead(v)`. Those are the platform acting on the machine,
rather than the machine accessing itself.

**Two things about a real memory path worth knowing before you declare a watch:**

- **An instruction fetch is a read.** A watch is on the address bus, so a place holding *code* is
  answered when the CPU fetches it, and `instead(v)` there substitutes the opcode that executes. On
  the Game Boy backend this is also how a watch and an escape at one address order themselves: the
  fetch is answered first, then the escape fires.
- **A 16-bit access is two byte accesses.** One `ld [$C300],sp` fires a watch twice, at `$C300` and
  `$C301`, with each byte's own value. Accesses are byte-granular on this backend; a second console
  states its own granularity.

Declare watches before `run()`, or between steps of a machine you drive yourself; declaring on a
machine that is already running throws.

## Managing the watch table

The machine answers for its watches by key, exactly as it does for escapes:

```cpp
vm.watches()["hp"].armed(false);   // declared, dormant
vm.watches()["hp"].armed();        // whether it is live
vm.watches()["hp"].remove();       // drop the declaration entirely
vm.watches().contains("hp");
vm.watches().size();
```

Watches are live when declared. Switching one off keeps its declaration, and a machine whose watches
are all off costs exactly what a machine with none costs, because the machine's own memory path
carries no hook at all.

**Switching and removing work while the machine runs**, and land at the next step boundary in the
order issued, like every other verb ([above](#while-it-runs-one-thread-owns-the-machine)) — so a
switch is not visible to the `armed()` that follows it. Issued from inside a handler the switch
applies immediately instead, because that code already runs on the machine's own thread.

A key this machine does not declare throws `std::out_of_range` naming the key.

## Calling the cartridge's own routines

`bindRoutine` names a routine that is **already there** — in the hosted image, or in code this VM was
given earlier — and hands back the same `Routine<Sig>` the other two forms do. `uploadRoutine` hands
over bytes, `registerRoutine` hands over a file, `bindRoutine` names an address:

```cpp
auto random = vm.bindRoutine<std::uint8_t()>(gb::banked(1, 0x5A1C),
                                             RoutineBinding{.output = gb::A});
const std::uint8_t roll = random();
```

The binding is the same transcription `.replaces` uses, read the other way round: it says where this
routine expects its arguments and where it leaves its answer, discovered from the cartridge's own
code. `throttle` and `entryOffset` have no meaning for a routine that is already in place — the
address *is* the entry — so a binding that sets either is refused.

Bind before `run()`, or between steps of a machine you drive yourself. An unreachable address, or any
of `uploadRoutine`'s binding failures, throws `std::invalid_argument`. Code in work RAM or high RAM is
as callable as code in a cartridge — a routine a game copies into high RAM to run it there is bound
the same way.

**The call runs in the guest's own context.** The registers and the stack the routine finds are the
ones the machine already had; its return frame is pushed where the guest's own `call` would push it;
and every register is back the way it was by the time the call returns. Code that was interrupted
mid-instruction carries on without noticing. What the routine changed in **memory** stands — a seed it
advanced, a buffer it decoded — because that is the answer it exists to give.

**Where you may call one from:**

- from an escape handler or a replacement's function, on the machine's own thread;
- from your own thread while the machine is **stopped**, or has never run at all — which is how you
  reach content a cartridge stores in a format only its own code understands, without playing the game
  to the point that unpacks it.

Calling one on a running machine from any other thread throws.

## Nesting: the whole loop

The two directions compose without limit, and each side resumes exactly as it was:

```cpp
// The cartridge's own generator, bound where it already sits.
auto random = vm.bindRoutine<std::uint8_t()>(kRandom, RoutineBinding{.output = gb::A});

// The cartridge's own damage rule, answered by this program — which builds its answer by
// calling the cartridge's own generator, from inside the escape, while the machine runs.
vm.registerEscapes(escapes(GuestEscape{
    .key      = "damage",
    .at       = kDamage,
    .replaces = routine(RoutineBinding{.inputs = {gb::B, gb::C}, .output = gb::A},
                        [&](std::uint8_t attack, std::uint8_t defence) -> std::uint8_t {
                            const std::uint8_t roll = random();      // into the guest, nested
                            return (attack - defence) + (roll & 0x0F);
                        })}));
```

The guest's loop calls its damage rule; your function answers; your function calls the cartridge's
generator; that generator runs on the guest's own stack and advances the seed the cartridge keeps in
its own memory; the register file is restored; your answer lands in A; the guest's next instruction
reads it. The routine you called may itself run through an address that escapes, whose handler calls
another routine, to any depth.

**Depth is not a parameter anywhere in the surface**, and nothing assumes the guest is frozen. That is
deliberate: a call from inside a call is the same call.

## What it costs

- **An escape's handler costs the guest nothing.** It runs on the host's clock; the guest's cycle
  count does not advance for it.
- **Arming an escape costs a per-instruction check.** A machine with at least one armed escape pays a
  small fraction of host CPU for it; a machine with none, or with all of them switched off, pays
  nothing.
- **Arming a watch costs a per-access check, in the direction armed.** The machine's memory path tests
  one bit per access against a bitmap over its address space — one load and a mask — and a machine
  with no armed watch in that direction carries no hook at all. Watching writes therefore costs
  nothing on reads, which is why the two directions are declared separately.
- **A watch handler costs the guest nothing**, on the same terms an escape handler does: it runs on
  the host's clock while the machine is parked at the access.
- **A call into the guest costs the guest exactly its own instructions**, entry through its return,
  plus one instruction fetch for the address the return lands on. That is guest time, counted where
  the machine was already being run from. A handler that calls nothing costs the guest nothing; a
  handler that calls a routine costs it that routine.

## Failure modes worth knowing

These are platform behaviour you cannot infer from the surface, so they are stated rather than left to
be discovered.

- **Power-on RAM is filled the way hardware fills it**, so a value written before `run()` is not the
  value the guest reads after it. Write what the guest should see once it is living.
- **`reset()` does not restore RAM to a fixed state.** A test or a demo wanting reproducibility across
  a reset re-establishes game-side state explicitly on both sides.
- **A handler's reads ride the publish**, so a handler acting on a value your game wrote a moment ago
  sees it one step later, and what a handler writes becomes readable a step after that.
- **A banked entry throws unless its bank is the mapped one**, naming both banks. Selecting a bank is
  the guest's own act, not the platform's.
- **A routine that never returns is abandoned** rather than left to hang: the register file comes back
  and the guest carries on, and what the routine had already written to memory stays written.
- **An interrupt arriving inside a routine is the guest's own.** It runs on the guest's stack, inside
  the call, and the call still returns — which is what the hardware does.
- **A handler that removes its own escape or watch destroys the code it is running in.** Switch it off
  and drop it once the call has returned.
- **A watch on a place holding code answers the instruction fetch**, because a fetch is a read like
  any other. Watch a data address, or use `instead(v)` deliberately to change what executes.

## Try it

Five examples, each one act, one that puts the whole surface on a single screen, and one that runs your own cartridge.

| | |
|---|---|
| `examples/cartridge_assets` | hosts an authored cartridge, declares its tile art and name table, prints the tiles as characters, patches one in the image, and reads `gb::WorkRam` through the same verb |
| `examples/rom_run` | hosts a commercial-shaped little game, runs it, and measures it live at `{1,1}`, `{2,1}`, `{1,2}` and paused, then round-trips a write through the guest's own loop and parks and resumes it mid-count |
| `examples/guest_escape` | escapes at a place in the guest's loop to count what it is doing from C++, then declares its routine replaced and answers it natively, in the routine's own registers |
| `examples/guest_nesting` | a replacement that answers the cartridge's damage rule by calling the cartridge's **own** generator, nested inside the escape; then, parked, calls its own decompressor on a table the game never reached |
| `examples/coexecution` | windowed, and every verb on this page acts on one picture: the cartridge marches eight walkers of its own each frame, drawn from the per-step publish. Under its own pace rule they hold a column; with `.replaces` armed they scatter |
| `examples/gb_player` | windowed: one ROM of your own on two machines side by side, one advanced by the engine's tick and one free-running on a clock of its own, both submitting their video as layer content. Asks for a ROM through the native file picker and ships none |

Every cartridge in the first five is authored in-code or by a committed generator script; the player takes a ROM of your own at runtime and ships none.

## Where to change things

| What | Where |
|---|---|
| The public surface | `include/retropp/vm.h`, `include/retropp/guest_escape.h`, `include/retropp/guest_watch.h`, `include/retropp/guest_frame.h`, `include/retropp/memory_region.h` |
| The Game Boy vocabulary — registers, memory areas, `banked` | `include/retropp/gb.h` |
| The host layer: declarations, validation, the escape and watch tables, the run loop | `src/vm/vm.cpp`, `src/vm/vm_runner.cpp` |
| What a core must provide | `src/vm/vm_backend.h` |
| The Game Boy backend | `src/vm/gameboy/sameboy_backend.cpp`, `src/vm/gameboy/sameboy_machine.cpp` |

Registering an extracted routine from bytes or a `.asm` file, the `Throttle` pacing seam, and hosting
a resident sound driver are the neighbouring surface, in
[vm-and-routines.md](vm-and-routines.md).

## Status

**Available on the Game Boy and Game Boy Color backend**, which is the only backend built today:
`hostRom`; the `MemoryRegion` / `registerRegions` / `read` / `write` surface in both its declared and
built-on-the-spot forms; the `gb::` memory constants and `gb::banked` addressing; `run` / `speed` /
`stop` with seeded post-boot state and the per-step publish; `registerEscapes` with both kinds —
`.handler` and `.replaces` — the escape table (`armed` / `remove` / `contains` / `size`) including
changing it while the machine runs; `registerWatches` with both directions, the `AccessVerdict`
outcomes, `AccessSource` and the watch table on the same terms; `bindRoutine`, in the guest's own
context, nested to any depth; and `video`, declared through `VmFeatures` or switched at runtime, on
either clock.

**Constructing a `Vm` for any other `VMPlatform` throws** — `Snes`, `Nes`, `Genesis` and
`MasterSystem` are enumerated so a consumer can name one, and each is a drop-in when its backend
lands.

**Planned, and what a new console changes.** A second console brings a backend and a `<console>::`
header naming its registers and memory areas; the verbs on this page, the declaration grammar and the
binding vocabulary stay as they are, because none of them is console-shaped. Two capabilities depend
on what a core can offer rather than on the surface: escapes need a per-instruction hook and watches
need a per-access one, and a core that has neither refuses at declaration rather than accepting a
declaration that could never fire. Video is a third of that kind — a core reports a completed frame,
its dimensions and its pixel layout, and one that draws nothing refuses rather than accepting a
request that could never be answered. A core with different dimensions or a different layout needs
nothing new from the surface, because neither is the platform's to choose. A console also states its own access granularity — how many times
a wide access fires a watch, and at which addresses.
