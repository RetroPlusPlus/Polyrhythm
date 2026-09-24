# Audio

Headers: `retropp/audio_system.h` (cue), `retropp/audio_library.h` (register), `retropp/gb_audio.h`
(Game Boy presets) — plus `retropp/sdl_platform.h` only if you hand a system a custom sink.

The platform plays sound in two halves. You **register** audio once on the program-wide **`AudioLibrary`**,
getting back an `AudioId`. You **cue** it by handle on an **`AudioSystem`** — `play(id)` / `stop()`.
Everything underneath — assembling the driver, running it at the right speed on its own thread, feeding
the audio device — the AudioSystem handles for you. You work in audio terms; you never touch the
machinery that makes the sound.

```cpp
#include "retropp/audio_system.h"
#include "retropp/audio_library.h"

retropp::AudioSystem audio{retropp::AudioKind::Chiptune};  // a chiptune system — owns its output, no sink to declare

retropp::AudioLibrary& library = retropp::AudioLibrary::instance();
const retropp::AudioId song =
    library.registerAudio("sound/overworld.asm", retropp::AudioType::Music, retropp::Isa::Sm83);

audio.play(song);

// That's all — production runs on the AudioSystem's own thread. You cue with play()/stop();
// you never step the audio from your game loop.
```

## Contents

- [The model](#the-model)
- [Registering audio — two forms](#registering-audio--two-forms)
  - [The Game Boy diagnostic tone](#the-game-boy-diagnostic-tone)
  - [`AudioType`: Music, Sfx, Vocals](#audiotype-music-sfx-vocals)
- [Cueing: the `AudioSystem`](#cueing-the-audiosystem)
- [Threads: what runs where](#threads-what-runs-where)
- [Hosting your own sound driver](#hosting-your-own-sound-driver)
- [Volume: the `AudioMixer`](#volume-the-audiomixer)
- [Output: the `AudioSink`](#output-the-audiosink)
- [Many audio systems at once](#many-audio-systems-at-once)
- [Choosing the console](#choosing-the-console)
- [What works today / what's planned](#what-works-today--whats-planned)

## The model

- **A system is one backend — chiptune or PCM.** The first constructor argument is an `AudioKind`
  (`Chiptune` or `Pcm`, no default), fixed for the system's life. A chiptune system runs a sound driver
  on a small VM it owns; a PCM system decodes and streams an audio file (`.wav` / `.ogg` / `.flac` /
  `.mp3`) and has no VM.
  `play()` throws if you cue an id of the other kind, so a system only ever produces its own kind.
- **Register on the library, cue on a system.** Registration is program-wide and lives on the single
  `AudioLibrary` (`AudioLibrary::instance()`), not on an `AudioSystem`. `registerAudio(...)` hands the
  library a piece of audio and returns an `AudioId`; an `AudioSystem` only **cues** it (`play(id)` cues,
  `stop()` silences). One registration plays on any AudioSystem whose console ISA matches the one you
  selected at registration — register once, cue from anywhere.
- **Production runs off your game's thread, a machine to a thread.** Once you cue audio, the AudioSystem
  produces it autonomously: each sounding machine runs on its own thread, and the system mixes what they
  produce and feeds the PCM the device drains. You never step any of it from your game loop, and a slow
  simulation frame can't starve the sound. Cue with `play()`/`stop()`; that is the whole game-facing
  surface. [Threads: what runs where](#threads-what-runs-where) has the arrangement.
- **One-shot SFX close themselves; Music and Vocals you close.** An `AudioType::Sfx` cue stops on its own
  once its sound has finished — the AudioSystem notices the output has gone silent and stops producing for
  it, so you never call `stop()` for a fire-and-forget effect (and it stops costing anything once quiet).
  `AudioType::Music` and `AudioType::Vocals` are yours to manage: they play until you `stop()` them, and
  the platform never cuts them short (a track may rest mid-song). `isPlaying()` reports whether a cued sound
  is still being produced.
- **You never see a VM.** An AudioSystem owns whatever it needs internally to make sound — for a
  chiptune system that's a small virtual machine running a sound driver at the original hardware clock,
  so the music plays at the correct pitch. None of that is on the surface: you register *audio*, not a
  routine.

## Registering audio — two forms

Registration lives on `AudioLibrary` and comes in two forms, mirroring the renderer's
`uploadAtlas` / `loadAtlas`:

```cpp
// RAW form — you hand over ready bytes (pre-assembled driver bytecode). No embed/load policy: you
// brought the bytes. The library copies them into its own storage (the span need not outlive the call).
AudioId AudioLibrary::uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa);

// PATH form — you hand over a compile-time LITERAL path; the build's Embed / LoadFromPath policy
// decides whether the assembled bytes are baked into the binary or the file ships beside it. The
// with-Isa (chiptune) overload takes a ChiptunePath, whose consteval ctor rejects a PCM extension
// (.wav/.ogg/.flac/.mp3) — an audio file cannot be registered as chiptune by mistake.
AudioId AudioLibrary::registerAudio(ChiptunePath resourcePath, AudioType type, Isa isa,
                                    std::optional<AssetPolicy> policy = {});

// The no-Isa overload takes a plain LiteralPath and registers a PCM audio file
// (.wav/.ogg/.flac/.mp3), decoded and streamed on an AudioKind::Pcm system — no ISA, no driver.
AudioId AudioLibrary::registerAudio(LiteralPath resourcePath, AudioType type,
                                    std::optional<AssetPolicy> policy = {});
```

Both mint an `AudioId` whose lifetime is the library's (the whole program). `uploadAudio` (bytes) is
**always chiptune**; `registerAudio` by path infers the *kind* from the extension (`.asm` → chiptune,
`.wav`/`.ogg`/`.flac`/`.mp3` → PCM), and the chiptune overload's `ChiptunePath` rejects a PCM extension
at compile time — so the kind is frozen into the entry with no way to mis-file it.

- **`isa`** — the instruction set the chiptune driver is written for, **selected by you** at registration
  (`Isa::Sm83` for the Game Boy family). It is the true compatibility unit: `play()` throws if you cue
  the id on an AudioSystem whose console runs a different ISA, so a mismatch fails loudly, not silently.
- **`type`** — `AudioType::Music`, `AudioType::Sfx`, or `AudioType::Vocals` (the routing tag; see below).
- **`policy`** (path form only) — `AssetPolicy::Embed` bakes the assembled bytes into the binary;
  `AssetPolicy::LoadFromPath` ships the `.asm` beside the binary and assembles it at registration. Omit
  it to take the per-type default (chiptune → `Embed`). The only way to deviate is the explicit per-call
  token, so the policy is always visible at the call site.

The path form takes a **compile-time literal** path (a `LiteralPath`), so a build-time scan can find and
bake it; a genuinely runtime path is not accepted — read the bytes yourself and use `uploadAudio`.

```cpp
auto& lib = retropp::AudioLibrary::instance();
const AudioId overworld = lib.registerAudio("audio/overworld.asm", AudioType::Music, Isa::Sm83);                     // Embed (default)
const AudioId hit       = lib.registerAudio("audio/sfx/hit.asm",   AudioType::Sfx,   Isa::Sm83, AssetPolicy::Embed);
const AudioId credits   = lib.registerAudio("audio/credits.asm",   AudioType::Music, Isa::Sm83, AssetPolicy::LoadFromPath);
```

`registerAudio` / `uploadAudio` take a **sound-driver** — the assembly (`.asm`) that synthesizes a track
or effect (for a Game Boy port, your extracted/authored sound code), or its pre-assembled bytes. A path
is assembled in the `isa` you selected: `Isa::Sm83` uses the platform's own SM83 assembler; another console
backend assembles that console's instruction set. You never pick an assembler — the `isa` choice decides it.

> **Audio files (PCM):** the same `registerAudio` name also accepts an **audio file** through its no-Isa
> overload — a `.wav` / `.ogg` / `.flac` / `.mp3` path is recognized as PCM (by extension) and plays on an
> `AudioKind::Pcm` system, which decodes and streams it instead of running a chiptune driver. A registered
> sound is therefore either synthesized chiptune or a decoded file; construct the matching system kind to play it.

### The Game Boy diagnostic tone

```cpp
// retropp/gb_audio.h — a free function that REGISTERS a built-in test tone on the library and returns
// its handle. It is NOT a method on AudioSystem and takes no AudioSystem.
namespace retropp::sameboy { AudioId diagnosticTone(AudioType type = AudioType::Sfx); }
```

`diagnosticTone()` registers the platform's built-in gentle test tone (a soft, low-pitch ~250 Hz triangle
on the wave channel) on the `AudioLibrary` and returns its `AudioId` — cue it on a Game Boy `AudioSystem`
exactly like any registered audio. Handy to confirm your audio output is wired up before you have real
sound data.

### `AudioType`: Music, Sfx, Vocals

`AudioType` tags each registration as **`Music`**, **`Sfx`**, or **`Vocals`**. It does two things: it sets
the auto-close behavior — `Sfx` is fire-and-forget and closes itself when its sound finishes, while `Music`
and `Vocals` are sustained and stay until you `stop()` them — and it picks the **mixer bus** the source is
scaled by (see [Volume](#volume-the-audiomixer)). A track is on whichever bus its `AudioType` names.
`Vocals` is simply a third bus alongside `Music` and `SFX` — a separate volume channel a game can tag
voice/dialogue-style audio with; it is not tied to any particular kind (it works for chiptune or PCM
sources alike). Like `Music`, it is sustained.

> **Small routines vs. real drivers.** A short `.asm` like the diagnostic tone is assembled by the
> platform's built-in SM83 assembler. A *real* sound driver is much bigger and written in full
> assembly (macros, sections, banked data): Game Boy music is typically composed in a **tracker**,
> which exports a driver plus per-song data, driven by an init entry and a once-per-frame tick
> entry. A faithful *port* runs the original game's own sound engine instead. Either way those big
> drivers are assembled to bytes by their own native toolchain at your project's build, not by the
> platform. To run one as a **resident** driver — driven by song number — host it through the
> driver-hosting surface (see [Hosting your own sound driver](#hosting-your-own-sound-driver)),
> whose per-frame tick is exactly that once-per-frame entry; the platform runs the assembled machine
> code, it does not reassemble a full driver itself.

## Cueing: the `AudioSystem`

```cpp
void AudioSystem::play(AudioId id, CueMode mode = CueMode::Layer);
                                      // start producing it as a NEW voice beside whatever is already sounding
void AudioSystem::stop();             // silence the system: every voice stops; the output drains (the audio stays registered)
```

You cue an `AudioId` minted by the library. **Under the fixed `Layer` default, `play()` never cuts off
audio that is already playing** — every cued sound is its own *voice*, and the system mixes all of its
voices into its output. Cue a one-shot effect over sustained music on the same system and both sound;
cue the same effect twice in quick succession and the two copies layer. Each chiptune voice runs its
driver on its own VM with the console's full set of sound channels, so any channel contention that
exists is a single driver's own doing *inside* its VM — a faithful port hosting the game's original
sound engine as one driver keeps that driver's authentic channel allocation (including its stealing),
and separately cued sounds simply coexist. `play()` verifies the entry's ISA matches this system's
console (throws on a mismatch) and materializes the driver lazily — so a single registration drives
whichever AudioSystem cues it.

**`CueMode::Retrigger` restarts a sound instead of layering it.** Passing it replaces only the voices
already playing the *same id* — the classic arcade re-fire, where a rapid second shot restarts the
shot sound rather than overlapping it — and every other sound on the system plays on untouched. The
choice is per-call and the `Layer` default never changes (the `AssetPolicy` shape): the only way to
deviate is the explicit token at the call site, so the behavior is always visible where it happens.

```cpp
sfx.play(shot, retropp::CueMode::Retrigger);  // a repeat fire restarts the effect
```

`stop()` is system-wide (one-shot `Sfx` voices also close themselves when their sound finishes);
per-voice control arrives with the planned `play()` voice-handle surface.

**A closing voice never clicks.** Cutting a waveform at amplitude is an audible click, so any close that
isn't already at silence rides a short (~8 ms) release fade to zero: the voice a `Retrigger` replaces,
every voice on `stop()`, and a PCM file whose final sample sits off zero (the tail decays from that last
frame). A one-shot chiptune SFX that auto-closed is already silent — nothing to fade.

`isPlaying()` reports whether a cued sound is still being produced. `audioStats()` returns an
`AudioStats` — the whole diagnostic picture of the audio path in one read:

```cpp
retropp::AudioStats stats = music.audioStats();

stats.framesBuffered   // frames queued for the sink right now
stats.framesDropped    // mixed frames a full ring discarded
stats.outputUnderflow  // frames the sink asked for and the ring did not have
stats.laneUnderflow    // silence substituted for machines that were late
```

`framesBuffered` is live state — the queue depth at the moment you ask. The other three are running
totals since the system was created. Read the struct once per polling pass and take your fields from it;
the four numbers are read independently, so a production pass can land between two of them.

The two underflow counts are different shortfalls, and they sit adjacent so the pair reads as a pair:
`outputUnderflow` is the device going hungry, `laneUnderflow` is machines being late. A machine can be
late without the device ever going hungry — that is what the output buffer's cushion is for.

## Threads: what runs where

Four kinds of thread carry a sounding audio system, and your game owns exactly one of them:

- **Your game's thread** cues. `play()` / `stop()`, a hosted driver's verbs, and a mixer level all cross to
  the audio side on a lock-free channel and are applied in the order you issued them. Every one of these
  calls returns immediately; none of them runs a machine.
- **A thread per machine.** Every sounding chiptune voice — a cued sound, a hosted resident driver — owns
  a machine, and that machine steps on its own thread, producing its frames into its own lane. Machines
  therefore cost each other nothing but the cores they run on: a machine with an expensive frame slows
  itself, not the voice beside it.
- **The system's production thread** mixes the lanes into the output buffer and keeps that buffer topped
  up to its latency target (~50 ms). It is the one thread that touches the mix.
- **The device's own thread** drains the buffer at the output's rate.

**A machine runs ahead only to the latency target.** Each one steps while what is waiting downstream of it
— its own lane plus the output buffer those frames are mixed into — is under that target, and parks when
it is not. So the frames standing between a machine and your speakers are that target and the step the
machine was already committed to, whether one thread steps every machine or each machine steps itself:
threading buys throughput and costs no latency.

**The mix never waits on a machine.** While the output still holds a cushion a pass takes only what every
machine has ready and comes back for the rest, so a machine a few milliseconds late costs nothing. Once
the output is down to its floor the waiting is over: the pass advances at the output's pace, a machine
with less to give contributes silence for the shortfall, and that shortfall is counted against it —
`HostedDriver::underflowFrames()` for a machine of your own, `audioStats().laneUnderflow` for every
machine summed, and `audioStats().outputUnderflow` for the whole mix arriving late at the device. A
machine that falls behind mutes itself and nothing else.

What this costs is bounded by cores, not by the mix: on a current development machine a system carries
somewhere around two hundred hosted machines before the tone breaks up, and the per-machine counters
register their first substituted frames well before anything is audible. `examples/vm_threads/` is that
measurement under a machine count you raise from one.

## Hosting your own sound driver

`play()` cues a piece of audio the library assembled for you. A game can also **host its own resident sound
driver** — a long-lived machine that runs on the system and that you drive with the same verbs: `play(id)`
selects a song or effect *by the driver's own number*, `stop()` silences it, and declared `slots` read and
write its state. This is how a faithful port runs the game's original sound engine, or how a tracker-exported driver is wired on.

A driver is registered on the `AudioLibrary` through a registration that returns a **typed** id, then hosted
on a Chiptune `AudioSystem`:

```cpp
#include "retropp/audio_system.h"
#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/gb.h"

struct Slots { std::optional<std::uint8_t> nowPlaying; };   // the driver state you want to read

// Register: place the driver's extracted image(s), name its per-frame tick, declare its verbs + slots.
retropp::DriverBinding binding;
binding.images    = {{.bytes = engineBytes, .base = 0x6000}};  // one or more placed images
binding.tickEntry = 0x6100;                                    // the platform calls this once per frame
binding.init      = retropp::Instruction::call(0x6000, retropp::gb::A);  // the platform runs it once at host()
binding.isa       = retropp::Isa::Sm83;

const retropp::DriverVerbs verbs{
    .play = {.music = retropp::Instruction::write(retropp::Location::memory(0xC010), 1)},  // a mailbox byte
    .stop = retropp::Instruction::write(retropp::Location::memory(0xC012), 1, /*fixedValue=*/1),
};

auto& lib = retropp::AudioLibrary::instance();
const retropp::DriverId<Slots> engine = lib.uploadDriver(
    binding, verbs,
    retropp::slots(retropp::slot(&Slots::nowPlaying, 0xC020, retropp::SlotDirection::Read)));

// Host: one resident machine on this system, then drive it with the player's own verbs.
retropp::AudioSystem::GBC     audio{retropp::AudioKind::Chiptune};
retropp::HostedDriver<Slots>  driver = audio.host(engine);

driver.play(0x05);                            // select the driver's song 5 (its own number, not an AudioId)
driver.play(0x12, retropp::AudioType::Sfx);   // a second lane, if the driver declares one
Slots s = driver.slots();                     // read the published state (s.nowPlaying)
driver.slots(Slots{.nowPlaying = 0});         // write a slot (applied once at the next tick)
driver.stop();                                // the driver's stop verb; the machine stays resident
driver.restart();                             // perform the declared .init again — put it back how it started
std::size_t behind = driver.underflowFrames();  // frames of silence the mix substituted for this machine
driver.close();                               // close the resident voice
```

### The verbs are the player's; the realization is declared once

A `HostedDriver` exposes the same verbs as the `AudioSystem` — `play(id[, lane])` / `stop()` — plus
`slots(...)` and `restart()`, with no hardware idiom at the call site. HOW each verb reaches the driver is
declared once, at registration, as an `Instruction`:

- **`Instruction::write(location, width[, fixedValue])`** — the id lands in a memory mailbox the driver
  polls each tick (the mailbox family, common in a game's own hand-written sound engine).
- **`Instruction::call(entry, register[, fixedValue])`** — the id rides a CPU register into an entry the
  platform calls (the argument family, common in tracker-exported drivers).

**`restart()` performs the declared `.init` again** — the same gesture the platform performs once when the
driver is placed. That is what a console reset IS, so a game reproducing one says this rather than
reproducing the routine's effects by hand: a driver's own init entry often does not clear its state RAM,
and the game's startup is what does, which is exactly the routine `.init` already names. It is neither
`stop()` (the driver's own declared stop, meaning whatever that driver decided) nor `close()` (a release
fade that removes the voice) — a restart leaves the voice playing. It is performed at the next tick
boundary, ordered with `play` / `stop` / `slots`, in the same frame cycle budget every other gesture gets.
A driver declaring no `.init` throws rather than silently doing nothing.

Both families are driven by the identical call site; only the declared `Instruction` differs. `play` is a
per-lane table keyed by `AudioType` — `.music` is required, `.sfx` / `.vocals` are optional; `driver.play(id)`
cues Music and `driver.play(id, AudioType::Sfx)` names another lane. Cueing a lane the driver did not
declare throws, and `stop()` on a driver that declares no `.stop` throws.

### Slots: the driver's state as a typed struct

`slots` map named fields of a game-defined struct onto the driver's memory. A field's type carries the slot
width, declaration order is the slot index, and each slot declares a direction (`Read` / `Write` /
`ReadWrite`). `DriverId<Slots>` carries the struct type, so the handle recovers it with nothing
re-declared — a field typo is a compile error.

- `driver.slots(Slots{.field = v})` writes only the fields it names, applied **once** at the next tick
  (mailbox semantics — never retained or re-asserted; writing a `Read` slot throws).
- `driver.slots()` reads the whole published value back, wait-free; write-only fields come back disengaged.

A driver with no state is `DriverId<NoSlots>` — pass no slots batch.

### Registration forms, placement, and the mixer bus

- **Two forms, mirroring audio registration.** `uploadDriver(binding, verbs, slots)` takes ready image bytes
  for every image; `registerDriver(binding, verbs, slots)` takes a `HostedDriverBinding`, whose images are
  named one at a time — each either a literal path under the Embed / LoadFromPath policy, or inline bytes.
  An `.asm` image is assembled in the binding's `isa`; another extension is raw bytes.
- **The policy is per image, and the build honours each one separately.** `DriverImagePath::policy` is
  optional; an image that names none resolves to `Embed`. The build scan reads each `DriverImagePath`
  initializer, so a binding can mix freely — the usual shape is an `Embed` boot image beside a
  `LoadFromPath` one carrying content a game has no right to ship inside its binary, and only the first is
  baked. As everywhere else the policy must be a literal `AssetPolicy::…` token to be seen, and the images
  are read from the binding's initializer rather than the `registerDriver` call, so building the binding in
  a helper is fine but computing a path at runtime is not. See
  [assets-and-embedding.md](assets-and-embedding.md#choosing-the-policy).
- **Placement is declared, banked when a driver needs it.** Each image names a base; `gb::banked(bank, addr)`
  places a bank-qualified image and the VM bank-switches through the driver's own placed code, with the
  mapper (`gb::Mbc3`) declared on the binding. The Vm-layer placement mechanics are in
  [vm-and-routines.md](vm/vm-and-routines.md#hosting-a-resident-driver).
- **A hosted driver rides the `vmDriver` mixer bus** (see [Volume](#volume-the-audiomixer)) — a straight
  amplifier over the whole driver voice, unity by default. The system's `stop()` does **not** close a
  resident driver (that would discard its song position); only `HostedDriver::close()` or the system's
  destruction does. A second `host()` of the same id on the same system throws — one resident machine per
  registration per system.

#### Mixing byte images and path images in one binding

A `HostedDriverBinding`'s `.images` holds a `DriverImagePath` or a `DriverImage` per entry, so one driver
takes some images from paths the build resolves and others from bytes the game already has:

```cpp
HostedDriverBinding binding{
    .images = {DriverImage{.bytes = audioSection, .base = 0x6000},   // bytes in hand — nothing reaches the binary
               DriverImagePath{.base   = 0x7FF0,
                               .path   = "src/audio_boot.asm",
                               .policy = AssetPolicy::Embed}},       // resolved by the build, baked in
    .tickEntry = 0x7FF0,
    .init      = Instruction::call(0x6000, gb::A),
    .isa       = Isa::Sm83,
};
const DriverId<Slots> id = AudioLibrary::instance().registerDriver(binding, verbs, slots(...));
```

That is the shape for a game whose driver is partly its own and partly the player's: startup code the
project wrote and ships, beside a section read at runtime out of content it may not ship inside its binary.

A byte image names no policy — it carries no path for the build to resolve, and nothing about it reaches
any registry. Its span need only outlive the `registerDriver` call: the library copies it, so whatever
produced the bytes may be destroyed before `host()` runs — a game may host a cartridge purely to read it
and reclaim the machine when the reads return. An image whose span is empty is refused at registration,
naming its base, rather than failing later where a game has no seam to catch it.

`examples/driver_mixed_images/` is this end to end: it reads a tick routine out of a hosted cartridge,
destroys the cartridge, and hosts a driver from those bytes beside a baked `.asm` setup image.

### How a machine is keeping up

`driver.underflowFrames()` is how many frames of silence the mix has substituted for this machine since it
first produced. A machine that keeps up reads zero however long it plays; a rising count means this machine
is not producing as fast as the output drains, and the frames it owed were filled with silence so every
other machine could play on. Read it from the game thread at any time — a machine that has been closed, and
one hosted on another system, read zero.

It names the machine, which is what makes it worth asking for: `audioStats().laneUnderflow` sums this
same starvation over every machine the system hosts, and `audioStats().outputUnderflow` counts the whole
mix arriving late at the device — neither can say which of a dozen hosted drivers is the one behind.
`examples/vm_threads/` reads all three, live, against a machine count you raise until they move.

`examples/driver_hosting/` hosts two synthetic drivers — one of each family — behind one identical panel,
every verb under a control. Both drivers zero their state RAM in `.init`, so pressing RESET after moving
the DRIVER VOL fader drops the readout to 0 — the restart is visible rather than asserted.

## Volume: the `AudioMixer`

Volume lives on the program-wide **`AudioMixer`** (`AudioMixer::instance()`, one per program like the
`AudioLibrary`). It carries five levels — a **Master** that scales everything, and one per bus: **Music**,
**Sfx**, **Vocals**, and **VMDriver** (the bus a [hosted driver](#hosting-your-own-sound-driver) rides).
Each is a `std::uint8_t` slider position: `0` mutes, `255` is unity (0 dB). Every
`AudioSystem` reads it, scaling each source it produces by `Master` composed with the source's
`AudioType` bus.

You set levels by handing over an **`AudioLevels`** aggregate — a designated-init literal that names
exactly the channels it changes. Every field is optional, so an unmentioned channel keeps its current
level:

```cpp
#include "retropp/audio_mixer.h"

retropp::AudioMixer& mix = retropp::AudioMixer::instance();
mix.levels(retropp::AudioLevels{.master = 200, .music = 128});  // Master down a little, Music at the midpoint...
mix.levels(retropp::AudioLevels{.sfx = 255});                   // ...SFX to full; Master/Music/Vocals untouched
```

- **Default unity.** Every level starts at `255`, which scales by exactly 1.0 — a mixer you never touch
  reproduces the produced stream sample for sample. This is the faithful default: install the port, change
  nothing, and it sounds exactly as it did with no mixer at all.
- **The slider is perceptual.** The levels between mute and unity follow a half-loudness taper, so the
  midpoint (`128`) sounds like about half — not the near-silence a straight linear scale gives at half.
  You set slider positions; the mixer handles the curve.
- **A partial literal leaves the rest alone.** `AudioLevels{.sfx = 100}` moves only SFX; `AudioLevels{}` is
  a no-op. There is no way to change a channel you did not name.
- **Set from anywhere, applied on the audio thread.** You set levels wherever your settings UI runs; each
  `AudioSystem` picks up the change on its production thread within a sample. Setting a level never affects
  the simulation — it scales output only.

A settings screen binds its Master / Music / SFX / Vocals sliders to `levels()`. Values are `0`–`255`, so a
slider maps to a level with no conversion. Read one channel back with `levels(AudioLevelType::Master)` (or
`Music` / `Sfx` / `Vocals` / `VMDriver`) — each returns the current `std::uint8_t` position, so the settings
screen initialises its sliders from them.

## Output: the `AudioSink`

The common case needs **no sink at all** — a bare `AudioSystem` owns an internal production
`SdlAudioSink`:

```cpp
retropp::AudioSystem audio{retropp::AudioKind::Chiptune};   // owns its SdlAudioSink; opens on start, releases on destruction
```

Hand the system a sink only when you need a *specific* output — a capture sink for tests, or a custom
(file / network) sink:

```cpp
retropp::SdlAudioSink sink;                                              // explicit output stream
retropp::AudioSystem  borrowed{retropp::AudioKind::Chiptune, sink};      // BORROW: `sink` must outlive `borrowed`

retropp::AudioSystem  owned{retropp::AudioKind::Chiptune,                // OWN: the system ties the sink to its lifetime
                            std::make_unique<MyCustomSink>()};
```

The auto-created `SdlAudioSink` needs SDL audio initialised, which `SdlPlatform`'s constructor performs —
true for any real game (it already has a platform for its window). Headless / test contexts pass a
`CaptureAudioSink` (borrowed or owned), which opens no device.

A custom sink implements the `AudioSink` interface — two pure virtuals: `start(unsigned rate, int
channels, AudioPullFn pull)` opens the device and begins draining by calling `pull` on the sink's own
audio thread, and `stop()` releases it. `AudioPullFn` is a
`std::function<std::size_t(std::span<AudioFrame>)>` the sink invokes to fill a buffer of `AudioFrame`s
(stereo 16-bit — `kAudioChannels == 2`, `kAudioSampleRate == 48'000`). Implement those two and a file or
network sink drops in wherever `SdlAudioSink` goes.

## Many audio systems at once

An `AudioSystem` is freely instantiable, like everything else in the platform — **run as many as you
want.** Each owns its own resources and its own output stream, and the OS mixes the streams, so you can
have one audio system for chiptune music and another for sampled effects, or several layered however your
game needs:

```cpp
retropp::AudioSystem music{retropp::AudioKind::Chiptune};  // each owns its own output stream — no sink to declare
retropp::AudioSystem sfx{retropp::AudioKind::Pcm};         // e.g. a PCM system for sampled effects
```

There is no global audio singleton and no fixed channel budget beyond what each system's backend models.
(The one program-wide singleton is the `AudioLibrary` *catalog* — where audio is registered — not the
*output*; outputs are per-system.)

## Choosing the console

```cpp
// Every ctor takes the AudioKind FIRST (no default). The sink-less form (preferred) follows it with the
// chiptune-path console knobs; the borrow / own-sink ctors take the sink right after the kind.
AudioSystem(AudioKind     kind,
            VMPlatform    platform   = VMPlatform::GameBoyColor,
            TimingProfile timing     = TimingProfile::GameBoyColor,
            unsigned      sampleRate = kAudioSampleRate /* 48 kHz */);
```

The console is a `VMPlatform`. `GameBoy` / `GameBoyColor` are the backends available today; the defaults
reproduce the faithful Game Boy Color baseline. Other consoles (SNES, Genesis, …) are drop-in backends —
the same `AudioSystem` surface drives them, with that console's sound chip and assembler behind it.
`platform` / `timing` configure the chiptune VM; a PCM system has no VM and ignores them, using only
`sampleRate` as its decode target.

## What works today / what's planned

| Capability | Status |
|---|---|
| Chiptune audio (register a sound-driver `.asm` / bytes, cue by handle, autonomous production thread) | available |
| Two registration forms — raw `uploadAudio` (bytes) / `registerAudio` (literal path) | available |
| `AssetPolicy` Embed / LoadFromPath on the path form | available |
| Sink-less default `AudioSystem` (auto-owns an `SdlAudioSink`) | available |
| Multiple independent audio systems | available |
| `SdlAudioSink` output (48 kHz stereo) | available |
| `AudioType` Music/Sfx/Vocals tag on registration | available (sets auto-close + mixer bus) |
| Concurrent voices per system (`play()` never preempts; voices mix into the system's output) | available |
| PCM audio-pack backend (register + play a `.wav` / `.ogg` / `.flac` / `.mp3` file on an `AudioKind::Pcm` system) | available |
| `AudioMixer` volume levels (Master + Music/Sfx/Vocals, perceptual slider, default unity) | available |
| Per-voice gain + pan/balance (developer-owned, the `play()` voice handle) | planned |
| Anti-channel-stealing (splitting ONE driver's channel writes across parallel sound chips, so a driver whose own allocation steals channels stops stealing them) | planned |

When a planned capability lands, the same registration/cue surface gains it without changing how you
call it.
