#pragma once

// The AudioSystem: a game's developer-facing audio OUTPUT surface.
//
// A game CUES audio HERE, in audio terms: it plays a registered sound by handle and stops it, whenever it
// likes. REGISTRATION IS NOT HERE — audio is registered on the single AudioLibrary
// (retropp/audio_library.h, the program-wide catalog), which mints an AudioId; an AudioSystem only CUES
// that handle. Every cued sound is a VOICE: cue as many as you like and they all sound together, mixed
// into this system's output — play() never cuts off audio that is already playing. The game never
// touches the machinery that makes a voice's sound — for a chiptune voice the AudioSystem owns a VM
// hosting that voice's driver at the hardware CPU clock with the console's sound chip enabled, and steps
// every voice on its OWN dedicated production thread, all internally. The game never steps anything: it
// just cues with play()/stop(); production self-paces on another core, robust to sim hitches. What's
// hidden is the voice plumbing (Vm / Routine / throttle / mixdown) and that thread; what's EXPOSED is
// the cue surface: play() / stop().
//
// FREELY INSTANTIABLE, like a Vm — no singleton. A game runs as MANY AudioSystems as it wants: a
// chiptune one here, a PCM-playback one there, several at once. Each owns its own resources and drains to
// its own AudioSink output stream; the OS mixes the streams. This is the same generalized, no-ceiling
// posture as VMPlatform / ViewportResolution everywhere in the platform.
//
// SYSTEM-AGNOSTIC: the console is a VMPlatform (GameBoy / GameBoyColor in v1; SNES / Genesis as
// backends land). The console picked at construction decides EVERYTHING console-specific, including the
// ISA a registered .asm is assembled in — SM83 for the Game Boy family (the platform's own assembler),
// the per-console assembler for others. The game writes audio source in its console's assembly; it
// never selects an assembler.
//
// pimpl'd so this public header pulls no VM or ring-buffer type.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "retropp/audio.h"
#include "retropp/audio_library.h"  // AudioId, AudioType, AudioKind, DriverId, SlotSpec, SlotAccessor — the
                                    // audio + driver-hosting vocabulary the catalog owns
#include "retropp/timing.h"
#include "retropp/vm.h"  // VMPlatform

namespace retropp {

namespace detail {
struct AudioSystemTestAccess;  // the internal synchronous test seam (src/audio/audio_system_testing.h)
}

// The durable typed handle to a hosted resident sound driver, returned by AudioSystem::host(). Defined
// below the AudioSystem class (its verbs enqueue onto the system) — forward-declared here so host() can
// name it as a return type.
template <class SlotsStruct>
class HostedDriver;

// How a play() treats voices already playing the SAME AudioId on this system. Layer (the fixed
// default) starts the new voice beside them — re-fires overlap and decay naturally. Retrigger
// restarts the sound: voices of the same id stop and the fresh voice takes their place — the classic
// arcade re-fire — while every OTHER sound on the system plays on untouched. The only way to deviate
// from Layer is this explicit per-call token (the AssetPolicy shape): the choice is always visible at
// the call site, never ambient system state.
enum class CueMode { Layer, Retrigger };

// How an AudioSystem's output is doing, gathered in one read (AudioSystem::audioStats()).
// `framesBuffered` is live state — the depth of the queue between production and the sink at the moment
// of the call. The other three are running totals since the system was created.
//
// The two underflow counts are different shortfalls and sit adjacent so the pair reads as a pair:
// `outputUnderflow` is the device asking for frames the ring did not have, `laneUnderflow` is the mix
// substituting silence for machines that had not produced theirs yet. A machine can be late without the
// device ever going hungry, which is what the ring's cushion is for; the numbers separate the two.
//
// The four fields are read independently, so a production pass can land between two of them. This is a
// snapshot to look at, not a coherent set — nothing relates the four numbers to each other.
struct AudioStats {
    std::size_t framesBuffered  = 0;  // frames queued for the sink right now
    std::size_t framesDropped   = 0;  // mixed frames a full ring discarded
    std::size_t outputUnderflow = 0;  // frames the sink asked for and the ring did not have
    std::size_t laneUnderflow   = 0;  // silence substituted for machines that were late
};

// AudioId / AudioType / AudioKind live in retropp/audio_library.h (the audio vocabulary the single
// AudioLibrary owns); this header consumes them. Registering on an AudioSystem forwards to
// AudioLibrary::instance(); an AudioId's lifetime is the library's (the whole program), not the system's.

class AudioSystem {
public:
    // The first argument of EVERY constructor is `kind` — the system's fixed backend, stated explicitly
    // (there is no default): AudioKind::Chiptune runs the VM-hosted sound driver, AudioKind::Pcm decodes
    // and streams an audio file (its VM never runs). A system is ONE kind for its whole life; play()
    // rejects an id of the other kind.

    // BORROW a sink. Create a `kind` audio system for `platform` (GameBoy / GameBoyColor in v1),
    // draining produced PCM to `sink`, which the system does NOT own — `sink` must outlive the
    // AudioSystem. A Chiptune system owns the VM that hosts the game's sound driver (the game never sees
    // it); a Pcm system has no VM (it decodes a file instead). `timing` supplies the per-tick CPU cycle
    // budget for the chiptune path (it must carry a CPU
    // block — the GB-family presets do); `sampleRate` is both the sound chip's output rate and the rate
    // `sink` is opened at. The tail defaults reproduce the faithful Game Boy Color baseline. Prefer the
    // sink-less ctor below for the common case; this borrow form is for tests (a capture sink) and
    // pre-owned custom sinks.
    explicit AudioSystem(AudioKind kind,
                         AudioSink& sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(detail::coreFor(platform), kind, sink, platform, timing, sampleRate) {}

    // OWN a sink. As above, but the system TAKES OWNERSHIP of `sink` and ties it to its own lifetime —
    // no caller-side keep-alive. This is the custom-sink path ("hand me a sink, you keep nothing": a
    // network sink, a file-capture sink, …) and the device-free test seam for the ownership machinery
    // (pass a unique_ptr<CaptureAudioSink>). The sink-less ctor below delegates here.
    explicit AudioSystem(AudioKind kind,
                         std::unique_ptr<AudioSink> sink,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(detail::coreFor(platform), kind, std::move(sink), platform, timing, sampleRate) {}

    // MAKE YOUR OWN sink — the zero-boilerplate default. Owns an internally-constructed production
    // SdlAudioSink, so `AudioSystem music{AudioKind::Chiptune};` just works: no sink to declare, no
    // lifetime to manage. Assumes a Platform exists (the auto-created SdlAudioSink needs SDL_INIT_AUDIO,
    // which SdlPlatform's constructor performs) — true for any real game, and the same assumption the
    // manual SdlAudioSink path already makes. Headless / test contexts use a borrowed or owned
    // CaptureAudioSink (the two ctors above) instead, which open no device.
    explicit AudioSystem(AudioKind kind,
                         VMPlatform platform = VMPlatform::GameBoyColor,
                         TimingProfile timing = TimingProfile::GameBoyColor,
                         unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(detail::coreFor(platform), kind, platform, timing, sampleRate) {}
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // REGISTRATION LIVES ON AudioLibrary, NOT HERE. An AudioSystem does not register audio — it CUES
    // already-registered audio by handle. A game registers ONCE on the single catalog —
    // `AudioLibrary::instance().uploadAudio(bytes, type, isa)` (raw) /
    // `AudioLibrary::instance().registerAudio("song.asm", type, isa, policy)` (path) — and the resulting
    // AudioId plays on ANY AudioSystem whose VM ISA matches the one selected at registration (play()
    // throws otherwise). Console-specific catalogue helpers (the Game Boy diagnostic tone, a future
    // tracker-driver adapter) live in that console's preset namespace (retropp/gb_audio.h) and register on
    // the library too — none are methods here, so nothing console-specific leaks into this surface.

    // Cue a registered audio: the production thread begins producing it on its next pass, as a NEW
    // voice beside whatever is already sounding. Under the fixed Layer default play() NEVER cuts off
    // playing audio — every cued sound coexists, mixed into this system's output. Passing
    // CueMode::Retrigger restarts THIS sound: only voices already playing the same id are replaced (the
    // arcade re-fire), everything else plays on. Any channel contention lives INSIDE a single voice's
    // VM (the console's sound channels, allocated by the driver running there, exactly as on the
    // original hardware), never at the system level: a faithful port that hosts the game's original
    // sound engine as ONE driver keeps that driver's authentic channel-stealing, and separately cued
    // sounds simply layer.
    void play(AudioId id, CueMode mode = CueMode::Layer);

    // Silence the system: every voice rides a short release fade (~8 ms) to zero and closes — a hard
    // cut at amplitude would click — and the output drains to silence. Registered audio stays
    // registered — play() again to cue afresh. (One-shot Sfx voices also close themselves when their
    // sound finishes, already at silence; per-voice control arrives with the play() voice-handle
    // surface.) A HOSTED RESIDENT DRIVER is NOT silenced by stop() — it is always-running and closes only
    // through its own handle (HostedDriver::close) or this system's destruction, so stop() never destroys
    // mid-game driver RAM (a song position). stop() release-fades the cued voices around it.
    void stop();

    // ── Hosted resident driver ────────────────────────────────────────────────────────────────────
    // Host a game's own sound driver (registered on the library through AudioLibrary::uploadDriver /
    // registerDriver — retropp/driver_binding.h, retropp/audio_library.h) as a long-lived, addressable
    // machine on this Chiptune system: place its images into a VM this system owns, run its declared .init
    // once, and step its per-frame tick on the production thread with the console's sound chip enabled — a
    // sustained voice on the VMDriver bus. Returns a durable typed handle whose call sites are the PLAYER's
    // own verbs — driver.play(id[, lane]) / driver.stop() / driver.slots(...) — the same grammar as the
    // system's own play()/stop(); the handle recovers the game's slots struct from DriverId<SlotsStruct>,
    // so a slot typo is a compile error and nothing is re-declared after registration.
    //
    // The driver voice is sustained (never auto-closed) and there is one per host() call. This system must
    // be AudioKind::Chiptune; the id must be a driver registration (AudioKind::Driver); a second host() of
    // the same id on the same system throws (one resident machine per registration per system — multi-
    // instance routing arrives with the anti-channel-stealing unit). The binding's ISA is verified against
    // this system's console on the production thread, exactly as play() verifies a chiptune's.
    template <class SlotsStruct>
    HostedDriver<SlotsStruct> host(DriverId<SlotsStruct> driver);

    // Nested platform-bound instantiation types — the all-caps hardware spelling (the driver-hosting design
    // decision), symmetric with Vm::{GB,GBC}. Each fixes the console (VMPlatform + TimingProfile) so a game
    // names the hardware once at the type and never repeats it at construction: `AudioSystem::GBC music{
    // AudioKind::Chiptune};`. Platform namespaces (gb::, …) stay HARDWARE vocabulary only — a system type
    // never lives in one. Defined below the class; they ARE AudioSystems (they add no state).
    class GB;
    class GBC;

    // ── Diagnostics (tests / dev) ────────────────────────────────────────────────────────────────
    [[nodiscard]] bool       isPlaying() const noexcept;   // a cued audio is currently being stepped
    [[nodiscard]] AudioStats audioStats() const noexcept;  // the queue depth and the three running totals

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Manual (thread-suppressed) construction for the internal test seam
    // (src/audio/audio_system_testing.h): builds the system WITHOUT starting the production thread, so a
    // device-free test drives production synchronously on its own thread. Borrows `sink`.
    struct ManualTag {};
    AudioSystem(ManualTag, AudioKind kind, AudioSink& sink, VMPlatform platform, TimingProfile timing,
                unsigned sampleRate)
        : AudioSystem(ManualTag{}, detail::coreFor(platform), kind, sink, platform, timing, sampleRate) {}
    friend struct detail::AudioSystemTestAccess;

    AudioSystem(detail::CoreFactory core, AudioKind kind, AudioSink& sink, VMPlatform platform,
                TimingProfile timing, unsigned sampleRate);
    AudioSystem(detail::CoreFactory core, AudioKind kind, std::unique_ptr<AudioSink> sink,
                VMPlatform platform, TimingProfile timing, unsigned sampleRate);
    AudioSystem(detail::CoreFactory core, AudioKind kind, VMPlatform platform, TimingProfile timing,
                unsigned sampleRate);
    AudioSystem(ManualTag, detail::CoreFactory core, AudioKind kind, AudioSink& sink, VMPlatform platform,
                TimingProfile timing, unsigned sampleRate);

    // ── Hosted-driver plumbing (the non-template core host() and the handle drive) ──────────────────
    // host()'s non-template half: validate the kind + the one-instance rule, create the resident-driver
    // voice (sized to `slotCount` published read-slots) on the production thread through the host inbox,
    // and mark the system playing. Throws on a non-Chiptune system or a duplicate host() of `driver`.
    void hostResolvedDriver(AudioId driver, std::size_t slotCount);

    // The HostedDriver handle's game-thread verbs, marshaled onto the cue channel and applied at the next
    // tick boundary (submission order). `value` is the played id (DriverPlay) or the slot write value
    // (SlotWrite); `lane` selects a DriverPlay's realization.
    void driverEnqueuePlay(AudioId driver, AudioType lane, std::uint64_t value);
    void driverEnqueueStop(AudioId driver);
    void driverEnqueueRestart(AudioId driver);
    void driverEnqueueSlotWrite(AudioId driver, std::uint32_t slotIndex, std::uint64_t value);
    void driverClose(AudioId driver);

    // The handle's coherent slot read: the whole published read-slot snapshot for `driver` (double-buffered,
    // wait-free), indexed by declaration order. A driver never hosted here reads back empty.
    [[nodiscard]] std::vector<std::uint64_t> driverReadSnapshot(AudioId driver) const;

    // The handle's starvation count: frames of silence the mix substituted for `driver`'s machine.
    // Zero for a driver never hosted here, and for one whose handle has been closed.
    [[nodiscard]] std::size_t driverUnderflowFrames(AudioId driver) const;

    template <class SlotsStruct>
    friend class HostedDriver;
};

// The nested platform-bound AudioSystem types (declared above): a Game Boy and a Game Boy Color audio
// system with their console + timing pre-bound. Each forwards AudioSystem's three sink forms (make-own /
// borrow / own) with only the platform + timing fixed; they add no state, so they ARE AudioSystems.
class AudioSystem::GB : public AudioSystem {
public:
    explicit GB(AudioKind kind, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, VMPlatform::GameBoy, TimingProfile::GameBoy, sampleRate) {}
    GB(AudioKind kind, AudioSink& sink, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, sink, VMPlatform::GameBoy, TimingProfile::GameBoy,
                      sampleRate) {}
    GB(AudioKind kind, std::unique_ptr<AudioSink> sink, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, std::move(sink), VMPlatform::GameBoy,
                      TimingProfile::GameBoy, sampleRate) {}
};

class AudioSystem::GBC : public AudioSystem {
public:
    explicit GBC(AudioKind kind, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, VMPlatform::GameBoyColor, TimingProfile::GameBoyColor,
                      sampleRate) {}
    GBC(AudioKind kind, AudioSink& sink, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, sink, VMPlatform::GameBoyColor,
                      TimingProfile::GameBoyColor, sampleRate) {}
    GBC(AudioKind kind, std::unique_ptr<AudioSink> sink, unsigned sampleRate = kAudioSampleRate)
        : AudioSystem(&detail::gameBoyCore, kind, std::move(sink), VMPlatform::GameBoyColor,
                      TimingProfile::GameBoyColor, sampleRate) {}
};

// ── HostedDriver<SlotsStruct> — the durable typed handle ───────────────────────────────────────────
//
// Returned by AudioSystem::host(). Its call sites are the player's OWN verbs — the same grammar as the
// AudioSystem — carrying no machine idiom (the binding's declared Instructions realize each verb, performed
// by the platform at the tick boundary):
//   * play(id[, lane]) — cue the driver's sound `id` (a song / SFX number the driver interprets, NOT a
//     platform AudioId) on a play lane (Music by default; AudioType::Sfx / Vocals name another). Throws if
//     the driver declares no realization for the named lane.
//   * stop()           — perform the driver's declared stop realization. Throws if none is declared.
//   * restart()        — perform the driver's declared `.init` again, the gesture that placed it. Throws
//     if the driver declares none.
//   * slots(GameStruct{.field = v, …}) — a partial write batch naming exactly the fields that change,
//     applied ONCE at the next tick boundary (mailbox semantics — never retained or re-asserted; a
//     Read-only field engaged here is a loud error).
//   * slots()          — read the whole game-struct value back from the published snapshot; write-only
//     fields come back disengaged.
//   * close()          — release-fade and close the resident voice (the driver stops running).
//   * underflowFrames() — how many frames of silence the mix has substituted for this machine.
//
// All verbs are game-thread and wait-free — they marshal onto the system's cue channel (writes) or read the
// wait-free published snapshot (reads). The handle carries the slot layout + the type-erased accessors
// copied at host(), so it is self-contained and stays valid while its AudioSystem lives (the AtlasId /
// Routine non-owning-handle lifetime contract — do not outlive the system).
template <class SlotsStruct>
class HostedDriver {
public:
    // Cue the driver's sound `id` on a play lane (Music by default). `id` is the driver's own sound number.
    void play(std::uint32_t id, AudioType lane = AudioType::Music) const {
        switch (lane) {
            case AudioType::Music:
                break;  // always declared (required at registration)
            case AudioType::Sfx:
                if (!hasSfx_) {
                    throw std::logic_error(
                        "HostedDriver::play: this driver declares no Sfx play lane");
                }
                break;
            case AudioType::Vocals:
                if (!hasVocals_) {
                    throw std::logic_error(
                        "HostedDriver::play: this driver declares no Vocals play lane");
                }
                break;
            case AudioType::VMDriver:
                throw std::invalid_argument(
                    "HostedDriver::play: VMDriver is the driver bus, not a play lane");
        }
        system_->driverEnqueuePlay(id_, lane, static_cast<std::uint64_t>(id));
    }

    // Perform the driver's declared stop realization. Throws if the driver declares no stop verb.
    void stop() const {
        if (!hasStop_) {
            throw std::logic_error("HostedDriver::stop: this driver declares no stop verb");
        }
        system_->driverEnqueueStop(id_);
    }

    // Put the driver back the way it started: perform its declared `.init` again — the same gesture
    // the platform performs once when the driver is placed. This is what a console reset IS, so a game
    // reproducing one says this rather than reproducing the routine's effects by hand.
    //
    // It is neither stop() nor close(). stop() performs the driver's own declared stop, which means
    // whatever that driver decided it means; close() begins a release fade and removes the voice
    // entirely. A restart leaves the voice playing and re-runs the placement gesture.
    //
    // Performed at the next tick boundary, ordered with play / stop / slots, and inside the same
    // frame cycle budget every other gesture gets — a restart is a gesture, not a special mode.
    // (At placement `.init` gets a far larger one-time budget, because the machine is not ticking yet
    // and there is no frame to budget against.)
    //
    // Throws if the driver declares no `.init`.
    void restart() const {
        if (!hasInit_) {
            throw std::logic_error(
                "HostedDriver::restart: this driver declares no init gesture to perform again");
        }
        system_->driverEnqueueRestart(id_);
    }

    // Apply a partial slot batch: for each ENGAGED field of `batch`, queue a write of its value to the
    // matching declared slot (applied once at the next tick boundary). An engaged Read-only field throws.
    void slots(const SlotsStruct& batch) const {
        for (std::size_t i = 0; i < accessors_.size(); ++i) {
            std::uint64_t value = 0;
            if (!accessors_[i].read(&batch, value)) {
                continue;  // field disengaged — this batch does not name it
            }
            if (specs_[i].direction == SlotDirection::Read) {
                throw std::logic_error(
                    "HostedDriver::slots: a Read-only slot cannot be written");
            }
            system_->driverEnqueueSlotWrite(id_, static_cast<std::uint32_t>(i), value);
        }
    }

    // Read the whole game-struct value back from the published snapshot. Readable slots (Read / ReadWrite)
    // come back engaged with their current value; write-only slots come back disengaged.
    [[nodiscard]] SlotsStruct slots() const {
        const std::vector<std::uint64_t> snapshot = system_->driverReadSnapshot(id_);
        SlotsStruct out{};
        for (std::size_t i = 0; i < accessors_.size(); ++i) {
            if (specs_[i].direction == SlotDirection::Write) {
                continue;  // write-only — nothing to read back
            }
            if (i < snapshot.size()) {
                accessors_[i].write(&out, snapshot[i]);
            }
        }
        return out;
    }

    // Release-fade and close the resident driver voice. After this the handle is spent (further verbs cue
    // nothing); the registration stays in the library.
    void close() const { system_->driverClose(id_); }

    // How many frames of silence the mix has substituted for this machine since it first produced —
    // the machine's own starvation, counted where the mix takes delivery of its frames. A machine that
    // keeps up reads zero however long it plays; a rising count means this machine is not producing as
    // fast as the output drains, and the frames it owed were filled with silence so every other machine
    // could play on.
    //
    // Distinct from AudioStats::outputUnderflow, which counts the ring coming up short at the device —
    // the whole mix arriving late, rather than one machine inside it. (AudioStats::laneUnderflow is this
    // same starvation summed over every machine the system hosts.) Read from the game thread at any
    // time; a machine never hosted here, and one whose handle has been closed, reads zero.
    [[nodiscard]] std::size_t underflowFrames() const { return system_->driverUnderflowFrames(id_); }

private:
    friend class AudioSystem;
    HostedDriver(AudioSystem* system, AudioId id, std::vector<SlotSpec> specs,
                 std::vector<SlotAccessor> accessors, bool hasSfx, bool hasVocals, bool hasStop,
                 bool hasInit)
        : system_(system),
          id_(id),
          specs_(std::move(specs)),
          accessors_(std::move(accessors)),
          hasSfx_(hasSfx),
          hasVocals_(hasVocals),
          hasStop_(hasStop),
          hasInit_(hasInit) {}

    AudioSystem*              system_ = nullptr;
    AudioId                   id_{};
    std::vector<SlotSpec>     specs_;      // index-aligned with accessors_ — carries each slot's direction
    std::vector<SlotAccessor> accessors_;  // type-erased read/write over SlotsStruct (sound: DriverId<S>)
    bool                      hasSfx_    = false;
    bool                      hasVocals_ = false;
    bool                      hasStop_   = false;
    bool                      hasInit_   = false;  // an .init was declared, so restart() has a gesture
};

template <class SlotsStruct>
HostedDriver<SlotsStruct> AudioSystem::host(DriverId<SlotsStruct> driver) {
    const AudioLibrary::Entry& entry = AudioLibrary::instance().entry(driver.id());
    if (entry.kind != AudioKind::Driver || !entry.driver.has_value()) {
        throw std::invalid_argument(
            "AudioSystem::host: this AudioId is not a hosted-driver registration (uploadDriver / "
            "registerDriver)");
    }
    const DriverDefinition& def = *entry.driver;
    hostResolvedDriver(driver.id(), def.slots.size());
    return HostedDriver<SlotsStruct>(this, driver.id(), def.slots, def.accessors,
                                     def.verbs.play.sfx.has_value(),
                                     def.verbs.play.vocals.has_value(), def.verbs.stop.has_value(),
                                     def.init.has_value());
}

}  // namespace retropp
