// AudioSystem implementation.
//
// The Impl owns everything the public surface hides: the active VOICES (each cued sound that is still
// sounding — a chiptune voice hosts its driver on its own VM with its own APU; a PCM voice streams its
// decoded frames), the SPSC ring their mixed output lands in, the wiring to the sink, and the dedicated
// PRODUCTION THREAD that steps every voice. The public AudioSystem is a thin pimpl over it, so no VM,
// ring-buffer, or thread type reaches the public header.
//
// play() NEVER cuts off audio that is already playing. Each cue starts a NEW voice beside the current
// ones, and the production pass sums every voice's post-gain PCM into the one output ring (saturating).
// Any channel contention that exists lives INSIDE a single voice's VM — the console's sound channels,
// allocated by the driver running there, exactly as on the original hardware — never at the system
// level. stop() silences the whole system (per-voice control arrives with the voice-handle surface);
// a finished one-shot SFX voice closes itself. A closing voice never truncates at amplitude — stop(),
// a Retrigger cut, and a PCM buffer's end all ride a short release fade to zero (a hard cut clicks).
//
// Production runs off the main thread: the game's play()/stop() marshal a command onto a lock-free SPSC
// cue queue and wake the production thread; that thread owns the voices, drains the queue, and runs the
// device-paced refill-to-target loop in frame-quantized steps. Each VM core is deterministic and each
// pass runs whole frames, so a voice's produced PCM does not depend on how production is chunked across
// passes.
#include "retropp/audio_system.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root
#include "retropp/audio_library.h"     // the single catalog play() reads entries from
#include "retropp/audio_mixer.h"       // AudioMixer::instance().effectiveGain — the per-voice output scale
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "retropp/sdl_platform.h"      // SdlAudioSink — the auto-owned production sink (ctor 3)
#include "retropp/snes.h"              // snes::audioRam / dspRegister — the sampler's places
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous test seam
#include "src/audio/auto_close.h"      // detail::shouldAutoStop — the one-shot-SFX lifecycle decision
#include "src/audio/cue_queue.h"       // audio::AudioCommand / CueQueue — the main→production channel
#include "src/audio/pcm_decode.h"      // detail::g_pcmDecode — the Pcm decode hook (installed by the no-ISA registerAudio)
#include "src/audio/produce_step.h"    // detail::mixFrames / rampFrame — the pure mixdown + release fade
#include "src/audio/ring_buffer.h"
#include "src/audio/snes/brr.h"             // audio::brr — the chip's sample format, encoded and decoded
#include "src/audio/snes/default_driver.h"  // audio::snesdriver — the sampler's driver and its layout
#include "src/audio/snes/sample_cache.h"    // audio::snesdriver::SampleCache — where whole samples sit
#include "src/audio/snes/sample_stream.h"   // audio::snesstream — a long file's delivery through a window
#include "src/vm/run_governor.h"     // vm::RunGovernor — the wall-clock pace the audio unit runs at
#include "src/vm/vm_core_access.h"   // vm::VmCoreAccess — builds a voice's machine on the core the game named
#include "src/vm/vm_runner.h"        // vm::VmRunner — the machine a voice steps through

namespace retropp {

namespace {
// The eight stream windows sit between the directory and the sample cache, back to back: the cache starts
// exactly where the last window ends.
static_assert(audio::snesdriver::kWindows + 8 * audio::snesstream::kStreamWindowBytes ==
                  audio::snesdriver::kSamplesStart,
              "the eight stream windows must fill the span from kWindows to kSamplesStart");

// The output buffer is kept filled to ~`targetFrames` (a small latency buffer, sampleRate / 20 ≈ 50 ms)
// and sized far larger (sampleRate / 4 ≈ 250 ms) so device-drain bursts never starve it. Production
// steps every chiptune voice in WHOLE-FRAME cycle units (the frame quantum — one frame of the voice
// machine's own clock) and mixes after each step; the frame quantum is a scheduling choice
// that does not change any voice's samples (each VM core is deterministic).

// How long the production thread parks between periodic refills WHILE PLAYING. Must be strictly less than
// the latency buffer's drain time (targetFrames / sampleRate ≈ 50 ms) so the ring never drains empty
// between wakes; a few ms tops it well ahead of drain and the device-paced refill-to-target self-corrects
// any residual drift. (Idle — no voices — is an UNtimed wait: zero wakeups until a cue arrives,
// preserving the auto-close CPU win.)
constexpr std::chrono::milliseconds kProductionWaitInterval{4};

// The binding every chiptune driver is placed under: it runs at the console's own clock.
constexpr RoutineBinding kChiptuneBinding{.throttle = Throttle::HardwareSpeed};

// Resolve a Chiptune catalog entry (registered on the single AudioLibrary) to the machine-code bytes its
// voice will run. The shared library holds the portable definition (bytes or a path + policy, and the ISA
// the developer SELECTED at registration); each voice gets its OWN copy to place into its own VM. The ISA
// is VERIFIED first — a cheap enum compare of the entry's developer-selected ISA against this VM's ISA,
// before any file read or assembly — so a mismatch throws immediately (in practice at startup, when audio
// is loaded/warmed up), never garbage-running foreign bytes. `cachedAsm` is the per-id system-level cache
// for the assemble path, so replaying a LoadFromPath sound assembles once, not per voice. `vm` supplies
// the platform's assembler and is otherwise untouched — resolution reads a catalog and the disk, and the
// machine is mutated only when the bytes are placed, on the thread that steps it.
std::vector<std::uint8_t> resolveChiptuneBytes(Vm& vm, const AudioLibrary::Entry& entry,
                                               std::optional<std::vector<std::uint8_t>>& cachedAsm) {
    if (entry.isa != isaFor(vm.platform())) {
        throw std::runtime_error(
            "AudioSystem::play: this chiptune was registered for a different ISA than this audio "
            "system's VM — it cannot run here");
    }
    // Raw-bytes entry (AudioLibrary::uploadAudio): the bytes are already this ISA's machine code.
    if (!entry.bytecode.empty()) {
        return std::vector<std::uint8_t>(entry.bytecode.begin(), entry.bytecode.end());
    }
    // Path entry (AudioLibrary::registerAudio): Embed → the build baked the assembled bytes into the
    // routine registry, keyed by the logical path. Falls through to the disk read if none were baked.
    if (resolveAssetPolicy(entry.policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> baked = detail::findEmbeddedRoutine(entry.asmPath);
            !baked.empty()) {
            return std::vector<std::uint8_t>(baked.begin(), baked.end());
        }
        detail::warnEmbedNotBaked("routine", entry.asmPath);
    }
    // LoadFromPath (or an un-baked Embed): resolve the full project-relative path against the engine's
    // single assetRoot(), read it, assemble it in this VM's ISA once (cached per id thereafter).
    if (!cachedAsm.has_value()) {
        const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.asmPath);
        std::ifstream in{full, std::ios::binary};
        if (!in) {
            throw std::runtime_error("AudioSystem::play: cannot open audio .asm file: " + full.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        cachedAsm = vm.assemble(ss.str());
    }
    return *cachedAsm;
}
// ── Hosted resident driver: the coherent slot snapshot + host-time helpers ──────────────────────────
//
// A seqlock over a fixed-size block of slot values: the production thread PUBLISHES a coherent set after
// each resident tick; the game thread READS the whole set wait-free (retrying only while a publish is
// mid-flight). One writer (production) + one reader (game). `values` is sized once at host() and written
// in place — never reallocated — so the reader's copy always reads stable storage and a torn word is caught
// by the version recheck. Double-buffered in spirit — one value block guarded by an even/odd version — with
// no second buffer, so a wide read never blocks the driver's step.
struct DriverSnapshot {
    explicit DriverSnapshot(std::size_t slotCount) : values(slotCount, 0) {}

    std::atomic<std::uint32_t> seq{0};    // even = stable, odd = a publish is in flight
    std::vector<std::uint64_t> values;    // fixed size — written element-wise, never resized

    // Frames of silence the mix has substituted for this machine — the same publishing object, a
    // different writer: the mix makes this count on the PRODUCTION thread, while `values` is published
    // by the machine's own thread. It sits outside the seqlock because the seqlock exists to make a SET
    // of slot values coherent with each other, and a lone counter has nothing to be coherent with.
    std::atomic<std::size_t> laneUnderflow{0};

    // Production thread: publish one frame's read-slot values as a coherent set.
    void publish(const std::vector<std::uint64_t>& v) {
        seq.fetch_add(1, std::memory_order_release);  // -> odd (writing)
        const std::size_t n = std::min(values.size(), v.size());
        for (std::size_t i = 0; i < n; ++i) {
            values[i] = v[i];
        }
        seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
    }

    // Game thread: the latest coherent set. Retries only while a publish is mid-flight (bounded — a publish
    // is a handful of stores), so the read is effectively wait-free for the rare handle.slots() reader.
    [[nodiscard]] std::vector<std::uint64_t> read() const {
        for (;;) {
            const std::uint32_t before = seq.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;  // a publish is in flight — wait for it to finish
            }
            std::vector<std::uint64_t> out(values);  // copy stable storage; a torn word is caught below
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_acquire) == before) {
                return out;
            }
        }
    }
};

// Bake a play verb (the declared Instruction) into a fully-determined gesture carrying `value` — so the
// engine's performInstruction, which reads a queued Instruction's fixed value, cues the driver's sound id.
// A Write carries the id in its mailbox; a Call rides it in the argument register (its fixed presets kept).
Instruction bakeInstructionValue(const Instruction& ins, std::uint64_t value) {
    if (ins.kind() == Instruction::Kind::Write) {
        return Instruction::write(ins.location(), ins.width(), value);
    }
    return Instruction::call(ins.entry(), ins.location(), value, ins.presets());
}

// Resolve a stored driver image to its bytes, on the audio thread, before the machine has a thread of
// its own. uploadDriver images already carry owned bytes; a registerDriver path image is resolved here
// (the host-time byte resolution): a `.asm` assembles in this VM's ISA, any other extension is raw image bytes,
// under the same Embed (baked) / LoadFromPath (on-disk) rule the rest of the asset surface uses. Embed
// falls through to the on-disk read when nothing was baked, so a literal path still resolves in development.
std::vector<std::uint8_t> resolveDriverImageBytes(const StoredDriverImage& img, Vm& vm) {
    if (!img.bytes.empty()) {
        return img.bytes;  // uploadDriver — already-owned bytes
    }
    const bool        isAsm  = detail::endsWith(img.path, ".asm");
    const AssetPolicy policy = resolveAssetPolicy(img.policy, AssetPolicy::Embed);
    if (policy == AssetPolicy::Embed) {
        const std::span<const std::uint8_t> baked =
            isAsm ? detail::findEmbeddedRoutine(img.path) : detail::findEmbeddedAsset(img.path);
        if (!baked.empty()) {
            return std::vector<std::uint8_t>(baked.begin(), baked.end());
        }
        // un-baked Embed — fall through to the on-disk read below
        detail::warnEmbedNotBaked(isAsm ? "routine" : "asset", img.path);
    }
    const std::filesystem::path full = assetRoot() / std::filesystem::path(img.path);
    std::ifstream in{full, std::ios::binary};
    if (!in) {
        throw std::runtime_error("AudioSystem::host: cannot open driver image file: " + full.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string contents = ss.str();
    if (isAsm) {
        return vm.assemble(contents);  // assemble the source in this VM's ISA (the engine's own assembler)
    }
    return std::vector<std::uint8_t>(contents.begin(), contents.end());  // raw image bytes, read as-is
}

}  // namespace

struct AudioSystem::Impl {
    // ── A voice: one cued sound, currently sounding ──────────────────────────────────────────────────
    // A Chiptune voice owns its OWN VM (with its own APU) hosting its driver at the hardware clock — so
    // simultaneous voices each get the console's full complement of sound channels, and any channel
    // contention is a single driver's own doing inside its VM, never one voice silencing another. A Pcm
    // voice holds a share of the decoded frames and a read cursor. `lane` is the voice's FIFO of
    // produced-but-unmixed post-gain frames: the APU callback pushes, the mix pops. It is the SPSC
    // hand-off between the thread stepping this voice's machine and the audio thread that mixes, and it
    // is a FIFO (not a per-pass scratch) because machines run at their own pace — what a machine
    // produces ahead of the mix simply waits, so no voice's stream ever gains or loses a sample. Voices
    // live behind unique_ptr so the APU callback's captured pointer stays stable however the vector
    // grows. Everything here is the audio thread's except the four fields marked as the machine's.
    struct Voice {
        // A voice's lane holds what its machine has produced and the mix has not taken yet; a Pcm voice
        // has no machine and no lane.
        explicit Voice(std::size_t laneCapacity) : lane(laneCapacity) {}

        // The machine's thread reaches this voice's lane and its silence run, so it leaves before any
        // of that is torn down.
        ~Voice() { runner.reset(); }

        Voice(const Voice&) = delete;
        Voice& operator=(const Voice&) = delete;

        AudioId      id{};  // the catalog entry this voice sounds (Retrigger replaces by matching it)
        AudioType    type{};
        // Consecutive exact-zero RAW output frames (auto-close input) — counted by the machine's thread
        // in the sample callback, read by the audio thread at the close decision.
        std::atomic<std::size_t> silenceRun{0};

        // Release: a closing voice never truncates at amplitude — it rides a short linear fade
        // (rampFrame) over `rampRemaining` of `rampTotal` frames and is removed at zero. Entered by
        // stop(), by a Retrigger replacing this id, and by a PCM buffer running out (the tail then
        // decays from `lastFrame`, so a file whose final sample is off zero still lands silently).
        // The silence auto-close (a one-shot SFX already at exact zero) removes directly — no ramp.
        bool         releasing = false;
        std::size_t  rampRemaining = 0;
        std::size_t  rampTotal = 0;

        // The same fact, for the machine's own thread to read: the fade is applied to frames the machine
        // has yet to produce, so a finishing machine runs on whatever the output is holding. Written by
        // the audio thread as the fade is entered, read by the machine's thread as it paces itself.
        std::atomic<bool> finishing{false};

        // Chiptune half
        std::unique_ptr<vm::VmRunner>     runner;  // the voice's own machine + its stepping; null on Pcm
        std::optional<Routine<void()>>    driver;  // the placed driver, placed by the machine's thread
        audio::SpscRingBuffer<AudioFrame> lane;    // produced post-gain frames awaiting the mix

        // The mix's per-voice working set: the frames this pass took, how many of them there were, and
        // the starvation the substitution covered for. A voice is only counted once it has produced —
        // before that it is still building its machine, and the count measures starvation, not startup.
        std::vector<AudioFrame> taken;
        std::size_t             tookFrames    = 0;
        bool                    produced      = false;
        std::size_t             laneUnderflow = 0;

        // Pcm half
        std::shared_ptr<const std::vector<AudioFrame>> pcm;  // decoded frames (shared with the cache)
        std::size_t                                    cursor = 0;
        AudioFrame                                     lastFrame{};  // the frame a past-end tail decays from

        // Resident-driver half. A resident voice hosts a game's own sound driver (retropp/driver_binding.h)
        // and is stepped as a resident machine (apply queue → tick → idle), not as a started driver. It is
        // sustained (never auto-closed) and rides the VMDriver bus. Gestures ride the runner's mailbox and
        // are performed ONCE at the next tick. `snapshot` is the coherent read-slot block shared with the
        // game-thread handle.
        bool                            resident = false;
        DriverDefinition                residentDef;             // machine facts (images/tick/slots/verbs)
        Isa                             residentIsa = Isa::Sm83; // the ISA (verified at host)
        std::shared_ptr<DriverSnapshot> snapshot;                // published read-slots (shared w/ handle)

        // The audio unit, on a HostDriven system: the console's sound hardware alone, the game driving
        // it through the system's place verbs. Sustained like a resident driver — stop() leaves it
        // running, since its RAM holds the game's samples — and paced by its own clock, never by the mix.
        bool audioUnit = false;

        // The sampler: the resident voice hosting the engine's default SNES sound driver, which this
        // system hosts by itself the first time an audio file is cued through the chip. A resident voice
        // in every other respect; the system's sample loads and chip settings reach its machine through
        // `writes`, served on the machine's own thread.
        bool sampler = false;

        // What the production thread posts to this voice's machine, for the machine's thread to take at its
        // next look, in posting order: bytes for a place, a streamed file's delivery started on a voice of
        // the chip (or stopped there when it carries no stream), and a gesture for the driver. A gesture
        // travels in this one queue with the writes it follows, so a sample is in audio RAM before the
        // gesture that keys it is performed — the look hands it to the mailbox the step that follows the
        // look drains. Guarded by the Impl's writeMtx.
        struct PlaceWrite {
            std::uint32_t             at;
            std::vector<std::uint8_t> bytes;
        };
        struct StreamChange {
            std::uint8_t                             voice = 0;
            std::optional<audio::snesstream::Stream> stream;
        };
        struct Gesture {
            Instruction instruction;
        };
        std::vector<std::variant<PlaceWrite, StreamChange, Gesture>> writes;

        // The sampler's deliveries, one per voice of the chip that is streaming a file. The machine's
        // thread starts, feeds and stops them, and nothing else touches them.
        std::array<std::optional<audio::snesstream::Stream>, audio::snesdriver::kVoices> streams;
    };

    // One request the game thread makes of the audio unit — a read or a write of one place — carried
    // to the unit's thread and answered there. The requester owns it (it lives on that thread's stack)
    // and waits on `answered` until `done`; the unit's thread fills `result` or `error` and sets `done`
    // under the request mutex. A write carries its bytes by copy, so the requester's span may die.
    struct UnitRequest {
        MemoryRegion              where;
        std::uint32_t             index = 0;
        bool                      isRead = false;
        std::vector<std::uint8_t> bytes;   // a write's bytes in, a read's bytes out
        std::exception_ptr        error;   // what the unit's verb threw, rethrown to the requester
        bool                      done = false;
    };

    // `ownedSink` is null on the borrow path and holds the sink on the owning path; it is declared
    // FIRST so it is constructed before `sink` is bound to it, and destroyed LAST (after the ring +
    // voices are torn down — safe because the dtor stops the sink AND joins the production thread before
    // any of this runs). The active sink is always reached through `sink`, so the wiring below is
    // identical on both paths.
    std::unique_ptr<AudioSink>        ownedSink;
    AudioSink&                        sink;
    unsigned                          sampleRate;
    AudioKind                         kind_;          // the system's fixed kind — Chiptune, Pcm or HostDriven
    VMPlatform                        platform_;      // the console each chiptune voice's VM is built as
    detail::CoreFactory               core_;          // the core those VMs are built on
    TimingProfile                     timing_;        // the profile each voice's Vm is constructed with; a voice is stepped by its machine's clock
    std::optional<vm::MachineClock>   clock_;         // the clock of the machine this system's core builds; empty on a platform with no core
    std::size_t                       targetFrames;   // keep the buffer filled to ~this (latency buffer)
    std::size_t                       ringFloor;      // buffered frames above which the mix waits for a
                                                      // straggling machine instead of substituting silence
    std::size_t                       autoStopSilenceFrames;  // a one-shot SFX auto-closes after this many
                                                              // consecutive exact-zero output frames (~250ms)
    std::size_t                       releaseFrames;  // a closing voice's fade length (~8 ms — the de-click)
    std::uint64_t                     cyclesPerFrame;  // the frame quantum — one stepDriver() runs this many
    int                               maxStepsPerWake;  // safety cap on steps per produce pass
    std::size_t                       framesPerStep;   // audio frames one step of a machine produces
    audio::SpscRingBuffer<AudioFrame> ring;

    // The audio unit of a HostDriven system (null on every other kind). Both pointers are set once, on
    // the constructing thread, and read from the game thread for the system's life: the unit's voice is
    // never closed (stop() and the auto-close both leave it), so they stay valid without ever reaching
    // into the production thread's voice list. The request channel is declared BEFORE the voices so it
    // outlives the unit's thread, which is joined as its voice is destroyed and serves requests until then.
    Vm*                             audioUnit_  = nullptr;  // the unit's machine
    vm::VmRunner*                   unitRunner_ = nullptr;  // the thread that steps it
    std::optional<vm::RunGovernor>  unitGovernor_;          // its wall-clock pace (threaded systems)
    std::mutex                      unitRequestMtx;
    std::condition_variable         unitAnswered;
    std::vector<UnitRequest*>       unitRequests;           // posted by the game thread, taken by the unit's

    // The sampler's bookkeeping — one sampler per system, so it lives here beside the voice that is it.
    // Audio RAM as the default driver lays it out: whole samples in the cache, from kSamplesStart up to the
    // echo ring's base, each with a directory entry; a long file delivered through its voice's fixed
    // window; the echo ring at the top, growing down. `sampleCache` owns the placements and the directory
    // entries 8..255; the sampler owns the chip writes, the holds and the ring. Production-thread-only,
    // like the voices, unless marked otherwise.
    audio::snesdriver::SampleCache sampleCache;

    // Every file encoded for this system, kept on the host (indexed by AudioId) so a dropped whole sample
    // is placed again from it with no second read or encode, and a streamed file is delivered from it.
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> encodedCache;

    std::uint8_t  windowsWritten = 0;  // a bit per voice whose window's directory entry has been written
    std::uint8_t  echoVoices     = 0;  // EON — the voices feeding the echo path
    std::uint64_t cueCounter     = 0;  // ticks each cue, the cache's recency clock
    std::mutex    writeMtx;            // guards every voice's `writes`

    // What each voice holds, so the cache keeps a sample a voice is playing and the ring stays off it. A
    // whole sample a voice keyed on is held from that cue until a later cue there, a stop, or — for a once
    // pass — the driver's own key-off, which the machine's thread reports through `samplerModes`.
    std::array<std::optional<AudioId>, audio::snesdriver::kVoices> heldId{};
    std::array<PlayMode::Kind,         audio::snesdriver::kVoices> heldMode{};
    std::array<std::uint64_t,          audio::snesdriver::kVoices> holdSince{};
    std::array<bool,                   audio::snesdriver::kVoices> oncePlaying{};  // a once hold's key-on has landed

    // The machine's thread publishes, after each step, the eight voice mode bytes (relaxed) then its step
    // count (release); the production thread reads the step count (acquire) then the modes, so a once pass
    // the driver has ended releases its hold.
    std::atomic<std::uint64_t> samplerModes{0};
    std::atomic<std::uint64_t> samplerSteps{0};

    // A file is read and encoded at the call, on the game thread; the encoded bytes cross to the production
    // thread through this inbox (a control-path lock, never the audio data path) into `encodedCache`.
    std::mutex encodedInboxMtx;
    std::vector<std::pair<AudioId, std::shared_ptr<const std::vector<std::uint8_t>>>> encodedInbox;
    std::vector<bool> encodedAtCall;  // game-thread-owned: ids already encoded at a call

    std::size_t streamThreshold = audio::snesstream::kStreamWindowBytes;

    // The echo path's state machine. The path is set once for the system; its ring grows down into the
    // cache and shrinks back up. A grow waits on a sample a voice is playing in the ring's way (growPending);
    // a shrink writes the new size now and moves the base once the old ring's length has elapsed
    // (shrinkPending). `base` is the chip ring's current base, `level` the configured echo volume, `muted`
    // whether stop() silenced it.
    struct EchoState {
        bool          set   = false;
        std::uint8_t  edl   = 0;
        std::uint16_t base  = audio::snesdriver::kRingEnd;
        std::uint8_t  level = 0;
        bool          muted = false;
        bool          growPending   = false;
        std::uint16_t pendingBase   = 0;
        std::uint8_t  pendingEdl    = 0;
        bool          shrinkPending = false;
        std::uint16_t shrinkBase    = 0;
        std::uint64_t shrinkEsaAt   = 0;
    };
    EchoState echo_;

    // The active voices — every cued sound still sounding, mixed together each pass. Audio-thread-owned
    // (a voice is created when its cue is applied there, mixed there, and closed there); each voice's
    // machine runs beside them on its own thread.
    std::vector<std::unique_ptr<Voice>> voices;

    // Voices on their way out. A closing voice's machine may be mid-step, and waiting for it inline
    // would hand a stalled machine the power to stall the audio it is leaving — so the voice is asked
    // to stop, set aside here, and destroyed by a later pass once its machine has left.
    std::vector<std::unique_ptr<Voice>> closing;

    // Per-id caches shared across this system's voices. `asmCache` holds LoadFromPath-assembled driver
    // bytes so replaying a sound assembles once; `pcmCache` holds decoded PCM frames, shared into each
    // voice (shared_ptr keeps a voice's frames alive and stable however the cache vector grows).
    // Production-thread-only, like the voices.
    std::vector<std::optional<std::vector<std::uint8_t>>>       asmCache;
    std::vector<std::shared_ptr<const std::vector<AudioFrame>>> pcmCache;

    // ── Hosted resident driver ────────────────────────────────────────────────────────────────────────
    // host() runs on the GAME thread but a resident voice owns a VM (production-thread-only), so a host()
    // creates the voice shell + its shared snapshot on the game thread and hands the shell across the
    // `hostInbox` (mutex-guarded — a rare control-path lock, never the audio data path). The production
    // thread drains the inbox each pass, building each voice's VM. `driverSnapshots` is a game-thread-owned
    // side table (indexed by AudioId) the handle reads its slots from; production writes only INTO the
    // pointed-to snapshot objects, never the vector, so no cross-thread access to the vector itself.
    std::mutex                                   hostInboxMtx;
    std::vector<std::unique_ptr<Voice>>          hostInbox;         // game thread → production
    std::vector<std::shared_ptr<DriverSnapshot>> driverSnapshots;   // game-thread-owned; indexed by AudioId

    std::atomic<std::size_t>          framesDropped{0};
    std::atomic<std::size_t>          underflowFrames{0};

    // Frames of silence the mix has substituted across every voice — the audio thread's running total
    // of the per-voice counters, published atomically so it can be read from another thread while the
    // machines run. Distinct from `underflowFrames`, which is the ring coming up short at the device.
    std::atomic<std::size_t>          laneUnderflowTotal{0};

    // The public diagnostic snapshot, gathered in one place. Four independent relaxed loads: a production
    // pass can land between two of them, which is all a reader of these numbers needs. Nothing relates
    // them to each other, so there is nothing here for a seqlock to make coherent.
    AudioStats stats() const {
        return AudioStats{
            .framesBuffered  = ring.sizeApprox(),
            .framesDropped   = framesDropped.load(std::memory_order_relaxed),
            .outputUnderflow = underflowFrames.load(std::memory_order_relaxed),
            .laneUnderflow   = laneUnderflowTotal.load(std::memory_order_relaxed),
        };
    }

    // Scratch for the mixdown (reused every pass, no per-pass allocation once warm).
    std::vector<AudioFrame> mixScratch;

    // ── production thread + cross-thread cueing ──────────────────────────────────────────────────────
    // The cue channel (main→production) and the wait/wake the production thread parks on. The cue queue
    // is lock-free SPSC (main pushes in play()/stop(), production pops in drainCues()); the mutex + cv
    // govern ONLY the thread's park/wake, never the cue data or the PCM ring. `running` gates the loop;
    // `threaded` records whether a production thread was started (false in the test's manual mode, where
    // play()/stop() apply their cue inline on the calling thread). `playing` is the cross-thread status
    // flag — maintained by the production thread as voices come and go, read by isPlaying().
    audio::CueQueue          cueQueue{256};
    std::mutex               mtx;
    std::condition_variable  cv;
    std::atomic<bool>        running{false};
    bool                     threaded = false;
    std::atomic<bool>        playing{false};
    std::thread              productionThread;

    // BORROW: `ownedSink` stays null; `sink` binds the external reference (non-owning).
    Impl(detail::CoreFactory core, AudioKind kind, AudioSink& s, VMPlatform platform, TimingProfile timing,
         unsigned rate)
        : ownedSink(nullptr),
          sink(s),
          sampleRate(rate),
          kind_(kind),
          platform_(platform),
          core_(core),
          timing_(timing),
          clock_(clockFor(core, platform, kind)),
          targetFrames(rate / 20),
          ringFloor(rate / 40),
          autoStopSilenceFrames(rate / 4),
          releaseFrames(rate * 8 / 1000),
          cyclesPerFrame(cyclesPerFrameFor(clock_)),
          maxStepsPerWake(maxStepsPerWakeFor(clock_, rate)),
          framesPerStep(framesPerStepFor(clock_, rate)),
          ring(rate / 4) {
        wire();
    }

    // OWN: move the sink into `ownedSink`; `sink` binds to it. `ownedSink` is initialised before `sink`
    // (declaration order), so `*ownedSink` is live when the reference binds.
    Impl(detail::CoreFactory core, AudioKind kind, std::unique_ptr<AudioSink> s, VMPlatform platform,
         TimingProfile timing, unsigned rate)
        : ownedSink(std::move(s)),
          sink(*ownedSink),
          sampleRate(rate),
          kind_(kind),
          platform_(platform),
          core_(core),
          timing_(timing),
          clock_(clockFor(core, platform, kind)),
          targetFrames(rate / 20),
          ringFloor(rate / 40),
          autoStopSilenceFrames(rate / 4),
          releaseFrames(rate * 8 / 1000),
          cyclesPerFrame(cyclesPerFrameFor(clock_)),
          maxStepsPerWake(maxStepsPerWakeFor(clock_, rate)),
          framesPerStep(framesPerStepFor(clock_, rate)),
          ring(rate / 4) {
        wire();
    }

    // The clock every machine of this system runs on: the console's, or — for a HostDriven system — the
    // audio unit's own, since that is the machine it hosts. Nothing on a platform with no core.
    static std::optional<vm::MachineClock> clockFor(detail::CoreFactory core, VMPlatform platform,
                                                    AudioKind kind) {
        if (core == nullptr) {
            return std::nullopt;
        }
        return kind == AudioKind::HostDriven ? vm::VmCoreAccess::audioUnitClockOf(core, platform)
                                             : vm::VmCoreAccess::clockOf(core, platform);
    }

    // The frame quantum: one frame of the voice machine's own clock, in its own cycles. Zero on a
    // platform with no core — no machine is ever built there, so nothing steps by it.
    static std::uint64_t cyclesPerFrameFor(const std::optional<vm::MachineClock>& clock) {
        return clock ? clock->cyclesPerFrame : 0u;
    }

    // The audio frames one step of a machine produces: the frame quantum's share of a second, at
    // this system's rate. At least one, so a very short frame still makes progress.
    static std::size_t framesPerStepFor(const std::optional<vm::MachineClock>& clock, unsigned rate) {
        if (!clock || clock->hertzNumerator == 0) {
            return 1;
        }
        return static_cast<std::size_t>(std::max<std::uint64_t>(
            static_cast<std::uint64_t>(clock->cyclesPerFrame) * rate * clock->hertzDivisor /
                clock->hertzNumerator,
            1));
    }

    // Steps needed to fill the latency buffer from empty (ceil(target / framesPerStep)) plus slack.
    // The device drains the ring on its own clock, so a wake usually needs far fewer; this only
    // bounds a fill-from-empty pass (and any runaway).
    static int maxStepsPerWakeFor(const std::optional<vm::MachineClock>& clock, unsigned rate) {
        const std::uint64_t perStep = framesPerStepFor(clock, rate);
        const std::uint64_t target  = rate / 20;
        return static_cast<int>((target + perStep - 1) / perStep) + 2;  // ceil + slack
    }

    // Wire the sink consumer to the ring — identical on both construction paths, since the active sink
    // is always reached through `sink`. (The producer side is per-voice: each chiptune voice's APU
    // callback is wired when the voice is created in applyPlay.) Begin draining immediately: the sink
    // pulls (its audio thread) from the ring; empty ring => silence until something plays. The pull is
    // the consumer side of the one SPSC PCM hand-off.
    void wire() {
        sink.start(sampleRate, kAudioChannels, [this](std::span<AudioFrame> out) -> std::size_t {
            const std::size_t got = ring.pop(out);
            if (got < out.size()) {
                underflowFrames.fetch_add(out.size() - got, std::memory_order_relaxed);
            }
            return got;
        });
        // Register the sampler's driver on the library now, on the constructing thread, so a cue never
        // mints it on the production thread while the game thread is reading the library at a call.
        if (platform_ == VMPlatform::Snes && kind_ == AudioKind::Chiptune) {
            (void)audio::snesdriver::defaultDriver();
        }
    }

    // A HostDriven system hosts its unit once it knows whether it has threads: on a threaded system the
    // unit gets a thread of its own, on a manual one it is stepped by the test. Called by the threaded
    // ctors from startProductionThread (before the production thread exists, so the voice list is
    // still the constructing thread's to touch) and by the manual ctor directly.
    void hostAudioUnitIfHostDriven() {
        if (kind_ == AudioKind::HostDriven) {
            hostAudioUnit();
        }
    }

    // A HostDriven system is its console's audio unit, alone: one machine with no console CPU in it,
    // built here and running from the system's first moment, its frames the system's output. The game
    // drives it through the system's read / write. It rides the music bus.
    //
    // The unit is paced by ITS OWN CLOCK, not by the mix: a cued voice runs ahead until the pipeline
    // holds the latency target and parks until the output drains it, but the unit is a chip that is
    // always running — it steps when the wall clock owes it a frame, a fixed cushion ahead so the device
    // never runs dry, and parks only until the next frame is due — the same governor a running cartridge
    // paces by. A request from the game wakes the unit's thread, and the look hook serves it there — a
    // parked unit at once, a stepping one as its step ends — so a write lands and a read is answered a
    // thread wake after the call, never a frame after it.
    void hostAudioUnit() {
        auto voice       = std::make_unique<Voice>(laneCapacity());
        voice->type      = AudioType::Music;
        voice->audioUnit = true;
        voice->runner    = std::make_unique<vm::VmRunner>(
            vm::VmCoreAccess::makeAudioUnit(core_, platform_, timing_), vm::VmRunner::StepKind::Started,
            cyclesPerFrame, runnerMode());
        Voice* vp      = voice.get();
        Vm*    machine = &voice->runner->machine();
        audioUnit_     = machine;
        unitRunner_    = voice->runner.get();  // the voice lives as long as the system: never closed
        voice->runner->beforeFirstStep([this, vp, machine] {
            machine->audio(true, {.rate = sampleRate, .onSample = [this, vp](std::int16_t left, std::int16_t right) {
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(vp->type);
                // The unit does not wait for the mix, so a lane the mix has not drained can fill; a
                // frame it has no room for is dropped and counted, the way a full ring drops a mixed one.
                if (!vp->lane.push(AudioFrame{applyGain(left, gain), applyGain(right, gain)})) {
                    framesDropped.fetch_add(1, std::memory_order_relaxed);
                }
            }});
        });
        voice->runner->onEachLook([this, machine] { serveUnitRequests(*machine); });
        if (threaded) {
            // The cushion: the latency target, in the unit's own cycles — how far ahead of the wall
            // clock the unit runs, so the frames the device asks for are already made.
            const std::uint64_t cushion =
                static_cast<std::uint64_t>(targetFrames) * clock_->hertzNumerator /
                (static_cast<std::uint64_t>(clock_->hertzDivisor) * sampleRate);
            unitGovernor_.emplace(clock_->hertzNumerator, clock_->hertzDivisor);
            unitGovernor_->restart(std::chrono::steady_clock::now());
            vm::VmRunner*    raw = voice->runner.get();
            vm::RunGovernor* g   = &*unitGovernor_;
            voice->runner->start(
                [g, raw, cushion] {
                    // Step while the cycles run lag what the wall clock owes plus the cushion; both
                    // sides are read on the stepping thread, so the closure is race-free by construction.
                    const std::uint64_t owed = g->owedThrough(std::chrono::steady_clock::now()) + cushion;
                    const std::uint64_t ran  = raw->cyclesRun();
                    return static_cast<std::size_t>(ran > owed ? ran - owed : 0);
                },
                /*highWater=*/1,
                [g, raw, cushion] {
                    // How long until the wall clock owes everything run so far, less the cushion —
                    // when the next frame is due. Measured this way the step lands when it is owed.
                    const std::uint64_t ran = raw->cyclesRun();
                    return g->timeUntilOwed(ran > cushion ? ran - cushion : 0);
                });
        }
        voices.push_back(std::move(voice));
        playing.store(true, std::memory_order_relaxed);
    }

    // ── The audio unit's place verbs ───────────────────────────────────────────────────────────────
    // The game thread's read / write of a place on the unit. The unit is reached only from its own
    // thread, so the request is posted, the unit woken, and the call waits until the unit's look hook
    // has served it (serveUnitRequests). On a manual system the calling thread IS the unit's thread, so
    // the request is served inline. What the unit's verb threw is rethrown here, at the call.
    void requireAudioUnit(const char* verb) const {
        if (audioUnit_ == nullptr) {
            throw std::logic_error(std::string("AudioSystem::") + verb +
                                   ": only a HostDriven system hosts its console's audio unit — this "
                                   "system has no unit to reach");
        }
    }

    // Carry one request to the unit and wait for its answer.
    void askUnit(UnitRequest& request) {
        if (!threaded) {
            serveOne(*audioUnit_, request);  // the calling thread steps the unit on a manual system
        } else {
            {
                std::lock_guard<std::mutex> lock(unitRequestMtx);
                unitRequests.push_back(&request);
            }
            unitRunner_->wake();
            std::unique_lock<std::mutex> lock(unitRequestMtx);
            unitAnswered.wait(lock, [&request] { return request.done; });
        }
        if (request.error) {
            std::rethrow_exception(request.error);
        }
    }

    // The unit's thread: take every request posted so far and answer each, in posting order.
    void serveUnitRequests(Vm& unit) {
        std::vector<UnitRequest*> taken;
        {
            std::lock_guard<std::mutex> lock(unitRequestMtx);
            if (unitRequests.empty()) {
                return;
            }
            taken.swap(unitRequests);
        }
        for (UnitRequest* request : taken) {
            serveOne(unit, *request);
        }
    }

    void serveOne(Vm& unit, UnitRequest& request) {
        try {
            if (request.isRead) {
                request.bytes = unit.read(request.where, request.index);
            } else {
                unit.write(request.where, std::span<const std::uint8_t>(request.bytes), request.index);
            }
        } catch (...) {
            request.error = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lock(unitRequestMtx);
            request.done = true;
        }
        unitAnswered.notify_all();
    }

    // ── Cue application (production thread, or inline in manual mode) ─────────────────────────────────
    // Drain every queued cue and apply it. Host-inbox first (so a host()-then-play() sequence finds its
    // voice already built), then the SPSC command queue. SPSC: only the production thread (or, in manual
    // mode, the single calling thread) pops here.
    void drainCues() {
        drainHostInbox();
        drainEncodedInbox();  // the bytes a call encoded, before the cue that plays them
        std::array<audio::AudioCommand, 1> one;
        while (cueQueue.pop(std::span<audio::AudioCommand>(one)) == 1) {
            applyCommand(one[0]);
        }
        releaseEndedHolds();  // a once pass the driver ended frees its hold
        serviceEcho();        // a pending grow commits once its way is clear; a shrink moves its base
    }

    // Move any host()-created resident voices from the inbox into the live set, building each one's VM
    // (place images, run .init, enable audio) on this thread. Uncontended in the steady state (host() is
    // rare); the mutex is a control-path lock, never touched on the audio data path.
    void drainHostInbox() {
        std::vector<std::unique_ptr<Voice>> incoming;
        {
            std::lock_guard<std::mutex> lock(hostInboxMtx);
            incoming.swap(hostInbox);
        }
        for (std::unique_ptr<Voice>& v : incoming) {
            initResidentVoice(*v);
            voices.push_back(std::move(v));
        }
    }

    void applyCommand(const audio::AudioCommand& cmd) {
        switch (cmd.op) {
            case audio::AudioCommand::Op::Play:
                applyPlay(cmd.id, cmd.mode);
                break;
            case audio::AudioCommand::Op::Stop:
                // Silence the SYSTEM: every CUED voice enters its release fade (a truncation at amplitude
                // would click) and is removed at zero within milliseconds; the ring then drains and the
                // sink silence-fills. A HOSTED RESIDENT DRIVER is excluded — it is always-running and
                // closes only through its handle (close) or system destruction, so stop() never destroys
                // its mid-game driver RAM. THE AUDIO UNIT of a HostDriven system is excluded for the same
                // reason — its RAM holds the game's samples, and the chip is the system. `playing` clears
                // when the last tail finishes, in the produce pass. Registered audio stays registered — a
                // later Play cues it afresh.
                for (const std::unique_ptr<Voice>& v : voices) {
                    if (!v->resident && !v->audioUnit) {
                        beginRelease(*v);
                    }
                }
                // The sampler stays, as a resident driver does; every voice it keyed on is keyed off,
                // and the echo path's output is muted — its buffer circulates a residue of the chip's
                // integer arithmetic that would otherwise never reach zero — until a cue engages it again.
                if (Voice* s = findSamplerVoice()) {
                    postGesture(*s, *s->residentDef.verbs.stop);
                    if (echo_.set) {
                        writeDsp(*s, kEvolL, 0);
                        writeDsp(*s, kEvolR, 0);
                        echo_.muted = true;
                    }
                    for (std::uint8_t voice = 0; voice < audio::snesdriver::kVoices; ++voice) {
                        stopStream(*s, voice);
                        releaseHold(voice);
                    }
                }
                break;
            case audio::AudioCommand::Op::PlayCue:
                applyPlayCue(cmd.id, cmd.cue);
                break;
            case audio::AudioCommand::Op::EffectVoice:
                applyEffect(cmd.cue);
                break;
            case audio::AudioCommand::Op::SetEchoPath:
                setEchoPath(cmd.path);
                break;
            case audio::AudioCommand::Op::StopVoice:
                if (Voice* s = findSamplerVoice()) {
                    const auto voice = static_cast<std::uint8_t>(cmd.value);
                    postGesture(*s, Instruction::write(Location::memory(audio::snesdriver::voiceBlock(voice)),
                                                       1, audio::snesdriver::kKeyOffBlock));
                    stopStream(*s, voice);
                    releaseHold(voice);
                }
                break;
            case audio::AudioCommand::Op::DriverPlay:
                applyDriverPlay(cmd.id, cmd.lane, cmd.value);
                break;
            case audio::AudioCommand::Op::DriverStop:
                applyDriverStop(cmd.id);
                break;
            case audio::AudioCommand::Op::DriverSlot:
                applyDriverSlot(cmd.id, cmd.slotIndex, cmd.value);
                break;
            case audio::AudioCommand::Op::DriverRestart:
                applyDriverRestart(cmd.id);
                break;
            case audio::AudioCommand::Op::DriverClose:
                if (Voice* v = residentVoice(cmd.id)) {
                    beginRelease(*v);  // release-fade and close (removed at ramp end in the produce pass)
                }
                break;
        }
    }

    // Find the live resident voice hosting `driver` (production-thread-only). Null if none — a driver op
    // that arrives after close(), or for an id never hosted here, is harmlessly ignored. A voice already
    // riding its release fade is skipped: it is on its way out, and skipping it is what lets an id hosted
    // again be driven while the machine it replaces finishes fading.
    //
    // A machine that has been hosted is always found, wherever it currently sits. host() and the verbs
    // that follow it are one ordered sequence on the game thread, but they arrive here through two
    // channels, and a machine handed over while this pass was working through the other one is still in
    // the inbox rather than among the voices. Taking delivery of the inbox before giving up is what
    // makes the two channels behave as the one sequence they were issued as.
    [[nodiscard]] Voice* residentVoice(AudioId driver) {
        if (Voice* found = findResidentVoice(driver)) {
            return found;
        }
        drainHostInbox();
        return findResidentVoice(driver);
    }

    [[nodiscard]] Voice* findResidentVoice(AudioId driver) {
        for (const std::unique_ptr<Voice>& v : voices) {
            if (v->resident && v->id == driver && !v->releasing) {
                return v.get();
            }
        }
        return nullptr;
    }

    // A play verb: pick the named lane's declared realization, bake the played id into it, and queue it.
    // The handle validated the lane exists before enqueuing, so a missing lane here is defensive.
    void applyDriverPlay(AudioId driver, AudioType lane, std::uint64_t value) {
        Voice* v = residentVoice(driver);
        if (v == nullptr) {
            return;
        }
        const PlayVerbs& play = v->residentDef.verbs.play;
        const std::optional<Instruction>* realization = &play.music;
        if (lane == AudioType::Sfx) {
            realization = &play.sfx;
        } else if (lane == AudioType::Vocals) {
            realization = &play.vocals;
        }
        if (realization->has_value()) {
            v->runner->enqueue(bakeInstructionValue(**realization, value));
        }
    }

    // The stop verb: queue the driver's declared stop realization as-is (its own fixed value applies).
    void applyDriverStop(AudioId driver) {
        Voice* v = residentVoice(driver);
        if (v == nullptr || !v->residentDef.verbs.stop.has_value()) {
            return;
        }
        v->runner->enqueue(*v->residentDef.verbs.stop);
    }

    // The restart verb: queue the driver's declared .init again — the same gesture placement ran
    // once. It rides the mailbox like every other verb, so it is performed at the next tick
    // boundary, in submission order, inside the frame's cycle budget.
    void applyDriverRestart(AudioId driver) {
        Voice* v = residentVoice(driver);
        if (v == nullptr || !v->residentDef.init.has_value()) {
            return;
        }
        v->runner->enqueue(*v->residentDef.init);
    }

    // A slot write: queue a mailbox write of `value` to slot `slotIndex`'s declared address/width.
    void applyDriverSlot(AudioId driver, std::uint32_t slotIndex, std::uint64_t value) {
        Voice* v = residentVoice(driver);
        if (v == nullptr || slotIndex >= v->residentDef.slots.size()) {
            return;
        }
        const SlotSpec& s = v->residentDef.slots[slotIndex];
        v->runner->enqueue(Instruction::write(Location::memory(s.address), s.width, value));
    }

    // ── The sampler: audio files through the console's sound chip ────────────────────────────────────
    // On an SNES system of the Chiptune kind a registered audio file is a sample the S-DSP plays. The
    // system hosts the engine's default sound driver for it by itself the first time one is cued, converts
    // each file to the chip's own format once — loading it whole into audio RAM, or, when it is longer than
    // a streamed file's window, delivering it through that window as the voice plays — and keys the cue's
    // voice on through the driver's mailbox — a resident-driver gesture like any other, performed at the
    // machine's next tick.

    void requireChiptune(const char* verb) const {
        if (kind_ != AudioKind::Chiptune) {
            throw std::logic_error(std::string("AudioSystem::") + verb +
                                   ": a cue plays through the engine's sound driver, which runs on an SNES "
                                   "system of the Chiptune kind");
        }
    }

    [[nodiscard]] Voice* findSamplerVoice() {
        for (const std::unique_ptr<Voice>& v : voices) {
            if (v->sampler && !v->releasing) {
                return v.get();
            }
        }
        return nullptr;
    }

    // The sampler's voice, hosted on the first cue that needs it: the default driver's registration as a
    // resident voice, built the way host() builds one, with no handle — the game never names the driver.
    Voice& samplerVoice() {
        if (Voice* s = findSamplerVoice()) {
            return *s;
        }
        const DriverId<audio::snesdriver::Slots> id = audio::snesdriver::defaultDriver();
        const AudioLibrary::Entry& entry = AudioLibrary::instance().entry(id.id());
        auto voice           = std::make_unique<Voice>(laneCapacity());
        voice->resident      = true;
        voice->sampler       = true;
        voice->id            = id.id();
        voice->type          = AudioType::VMDriver;
        voice->residentDef   = *entry.driver;
        voice->residentIsa   = entry.isa;
        voice->snapshot      = std::make_shared<DriverSnapshot>(entry.driver->slots.size());
        initResidentVoice(*voice);
        voices.push_back(std::move(voice));
        playing.store(true, std::memory_order_relaxed);
        return *voices.back();
    }

    // Bytes for a place on a voice's machine. A manual system's machine steps on this thread, so the
    // write lands now; a threaded one's lands at its machine's next look, in posting order, before the
    // step that follows it.
    void postWrite(Voice& v, std::uint32_t at, std::vector<std::uint8_t> bytes) {
        const MemoryRegion where{.at = at, .size = static_cast<std::uint32_t>(bytes.size())};
        if (!threaded) {
            v.runner->machine().write(where, std::span<const std::uint8_t>(bytes));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(writeMtx);
            v.writes.push_back(Voice::PlaceWrite{.at = at, .bytes = std::move(bytes)});
        }
        v.runner->wake();
    }

    // A streamed file's delivery started on a voice of the chip, or stopped there when `change` carries no
    // stream. Like a write: now on a manual system, at the machine's next look on a threaded one, in
    // posting order with the writes.
    void postStream(Voice& v, Voice::StreamChange change) {
        if (!threaded) {
            changeStream(v, v.runner->machine(), std::move(change));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(writeMtx);
            v.writes.emplace_back(std::move(change));
        }
        v.runner->wake();
    }

    // A gesture for the driver on a voice's machine, in posting order with the writes: performed by the
    // step after the machine's next look on a threaded system, by the next step on a manual one, where
    // every write before it has already landed.
    void postGesture(Voice& v, const Instruction& instruction) {
        if (!threaded) {
            v.runner->enqueue(instruction);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(writeMtx);
            v.writes.emplace_back(Voice::Gesture{.instruction = instruction});
        }
        v.runner->wake();
    }

    // The machine's thread, at each look: everything posted so far, in order. A gesture goes into the
    // runner's mailbox here, on the thread that drains it; one the mailbox has no room for waits, with
    // everything posted after it, for the next look.
    void serveWrites(Voice& v, Vm& machine) {
        using Posted = std::variant<Voice::PlaceWrite, Voice::StreamChange, Voice::Gesture>;
        std::vector<Posted> taken;
        {
            std::lock_guard<std::mutex> lock(writeMtx);
            if (v.writes.empty()) {
                return;
            }
            taken.swap(v.writes);
        }
        for (auto it = taken.begin(); it != taken.end(); ++it) {
            if (const Voice::PlaceWrite* w = std::get_if<Voice::PlaceWrite>(&*it)) {
                machine.write(MemoryRegion{.at = w->at, .size = static_cast<std::uint32_t>(w->bytes.size())},
                              std::span<const std::uint8_t>(w->bytes));
            } else if (const Voice::Gesture* g = std::get_if<Voice::Gesture>(&*it)) {
                if (!v.runner->enqueue(g->instruction)) {
                    std::lock_guard<std::mutex> lock(writeMtx);
                    v.writes.insert(v.writes.begin(), std::move_iterator(it), std::move_iterator(taken.end()));
                    return;
                }
            } else {
                changeStream(v, machine, std::move(std::get<Voice::StreamChange>(*it)));
            }
        }
    }

    // The machine's thread: a delivery starts by writing the window's opening and its whole loop, ahead of
    // the step that keys the voice on; a stop leaves the window as it stands.
    void changeStream(Voice& v, Vm& machine, Voice::StreamChange change) {
        std::optional<audio::snesstream::Stream>& stream = v.streams[change.voice];
        stream = std::move(change.stream);
        if (stream.has_value()) {
            writeBlocks(machine, stream->start());
        }
    }

    // The machine's thread, after each step: every delivery reads where its voice's decoder stands and
    // writes the blocks the voice has left behind with the file's next ones. Delivery is therefore in the
    // machine's own time — the machine never takes a step this has not run before.
    void feedStreams(Voice& v, Vm& machine) {
        for (std::uint8_t voice = 0; voice < audio::snesdriver::kVoices; ++voice) {
            if (v.streams[voice].has_value()) {
                writeBlocks(machine, v.streams[voice]->advance(vm::VmCoreAccess::sampleDecoderAddress(machine, voice)));
            }
        }
    }

    static void writeBlocks(Vm& machine, const std::vector<audio::snesstream::Write>& blocks) {
        for (const audio::snesstream::Write& w : blocks) {
            machine.write(MemoryRegion{.at   = snes::audioRam(w.address),
                                       .size = static_cast<std::uint32_t>(audio::brr::kBlockBytes)},
                          std::span<const std::uint8_t>(w.block));
        }
    }

    void writeDsp(Voice& s, std::uint8_t reg, std::uint8_t value) {
        postWrite(s, snes::dspRegister(reg), std::vector<std::uint8_t>{value});
    }

    // 0 to 1 as the chip's 0 to 127.
    [[nodiscard]] static std::uint8_t unit127(float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 127.0f));
    }

    // A voice's left and right volume from a cue's volume and pan: the middle is full on both sides, and
    // panning takes from the far side until, at the edge, that side is silent.
    [[nodiscard]] static std::pair<std::uint8_t, std::uint8_t> volumes(float volume, float pan) {
        const std::uint8_t full = unit127(volume);
        pan                     = std::clamp(pan, -1.0f, 1.0f);
        const auto left  = static_cast<std::uint8_t>(pan <= 0 ? full : std::lround(full * (1.0f - pan)));
        const auto right = static_cast<std::uint8_t>(pan >= 0 ? full : std::lround(full * (1.0f + pan)));
        return {left, right};
    }

    void applyPlayCue(AudioId id, const audio::VoiceCue& cue) {
        namespace sd = audio::snesdriver;
        const AudioLibrary& library = AudioLibrary::instance();
        if (static_cast<std::size_t>(id) >= library.size()) {
            return;  // unknown handle — nothing to cue
        }
        // The sampler first: hosting it the first time registers its driver on the library, and an entry
        // taken before that would not survive the library growing.
        Voice&              s      = samplerVoice();
        const std::uint64_t cueNow = ++cueCounter;
        const CuedSample    sample = cueSample(s, id, cue, cueNow);
        // The voice's hold: release what it held, then hold a whole sample it keys on. A streamed voice
        // holds nothing — its window is its own.
        releaseHold(cue.voice);
        if (!sample.streamed) {
            sampleCache.hold(id);
            heldId[cue.voice]    = id;
            heldMode[cue.voice]  = cue.mode.kind;
            holdSince[cue.voice] = samplerSteps.load(std::memory_order_acquire);
        }
        const AudioEffect& e     = cue.effect;
        const auto         pitch = static_cast<std::uint16_t>(
            std::clamp<long>(std::lround(e.pitch * kPitchAsRecorded), 1, kPitchMax));
        const auto [left, right] = volumes(e.volume, e.pan);
        // A repeat's period in the driver's frames — the console's own, at the rate its clock gives. A
        // streamed pass ends on the file's last block, which the chip mutes the voice at: once is left to
        // the chip, and a repeat strikes again with no key-off between.
        sd::Mode      mode   = sd::Mode::Continuous;
        std::uint16_t period = 0;
        if (cue.mode.kind == PlayMode::Kind::Once) {
            mode = sample.streamed ? sd::Mode::Continuous : sd::Mode::Once;
        } else if (cue.mode.kind == PlayMode::Kind::Repeat) {
            mode = sample.streamed ? sd::Mode::RepeatSelfEnding : sd::Mode::Repeat;
            const double framesPerSecond = static_cast<double>(clock_->hertzNumerator) /
                                           (static_cast<double>(clock_->hertzDivisor) * clock_->cyclesPerFrame);
            period = static_cast<std::uint16_t>(std::clamp<long>(
                std::lround(60.0 / cue.mode.perMinute * framesPerSecond), 1, 0xFFFF));
        }
        applySend(s, cue.voice, e.echo || e.reverb);
        if (cue.mode.kind == PlayMode::Kind::Repeat) {
            postGesture(s, Instruction::write(Location::memory(sd::voicePeriod(cue.voice)), 2, period));
            postGesture(s, Instruction::write(Location::memory(sd::voiceCountdown(cue.voice)), 2, period));
        }
        postGesture(s, Instruction::write(Location::memory(sd::voiceBlock(cue.voice)), sd::kKeyOnWidth,
                                          sd::keyOnBlock(sample.entry, pitch, left, right, mode)));
    }

    // The directory entry a cue keys its voice on with, and whether the voice streams to it.
    struct CuedSample {
        std::uint8_t entry    = 0;
        bool         streamed = false;
    };

    // The sample a cue plays, routed on the production thread. A file at or under the threshold is kept
    // whole in the cache when room can be made — placed lowest-first, the stalest unheld samples giving
    // way — and every later cue plays that sample, round or through to its end; a file over the threshold,
    // or one the cache cannot make room for, is delivered through the cue's voice's window, with the same
    // sound. The bytes were read and encoded at the call and wait in `encodedCache`.
    CuedSample cueSample(Voice& s, AudioId id, const audio::VoiceCue& cue, std::uint64_t cueNow) {
        const std::shared_ptr<const std::vector<std::uint8_t>>& blocks = encodedFor(id);
        if (blocks == nullptr) {
            return CuedSample{};  // not encoded (an unknown id reached here) — nothing to cue
        }
        if (blocks->size() <= streamThreshold) {
            const bool wasPlaced = sampleCache.placementOf(id).has_value();
            if (const auto placed =
                    sampleCache.place(id, static_cast<std::uint16_t>(blocks->size()), cueNow)) {
                if (!wasPlaced) {
                    writeWholeSample(s, placed->start, placed->entry, *blocks);
                }
                stopStream(s, cue.voice);  // a whole sample on this voice takes it off any delivery
                const std::uint8_t entry = cue.mode.kind == PlayMode::Kind::Continuous
                                               ? placed->entry
                                               : endingEntry(s, id, placed->start);
                return CuedSample{.entry = entry};
            }
            // the cache is full of samples other voices are playing — stream this one instead
        }
        return CuedSample{.entry = startStream(s, cue, blocks), .streamed = true};
    }

    // The directory entry that plays a whole sample through to its end: the sample's own start, continuing
    // into the silent sample the sound program carries at kSilenceBlock — so a pass ends on the sample's
    // last sample, and the voice has nothing to sound in the frame before the driver keys it off. The cache
    // hands the entry, taking one the first time a pass plays the sample through and the same entry after.
    std::uint8_t endingEntry(Voice& s, AudioId id, std::uint16_t start) {
        const std::uint8_t entry = sampleCache.endingEntry(id);
        writeDirectory(s, entry, start, audio::snesdriver::kSilenceBlock);
        return entry;
    }

    // An audio file in the chip's sample format: a `.brr` file as it is, any other decoded at the chip's
    // rate and encoded to loop round from its start, so a cue can hold it; a pass ends where the file does.
    std::vector<std::uint8_t> encodeSample(const AudioLibrary::Entry& entry) const {
        const std::vector<std::uint8_t> file = readEntryBytes(entry);
        if (detail::endsWith(entry.asmPath, ".brr")) {
            audio::brr::requireWellFormed(file);
            return file;
        }
        return audio::brr::encode(audio::brr::toMono(decodeFile(file, audio::brr::kSampleRate)), /*loop=*/true);
    }

    // A streamed file's delivery started on the cue's voice, in that voice's fixed window. The window's
    // directory entry — the voice's own number — is written the first time the voice streams; the delivery
    // feeds the blocks as the voice plays. It ends itself when the cue plays a pass — once or at a tempo —
    // or when the file is a `.brr` whose last block ends without looping.
    std::uint8_t startStream(Voice& s, const audio::VoiceCue& cue,
                             const std::shared_ptr<const std::vector<std::uint8_t>>& blocks) {
        namespace ss = audio::snesstream;
        const std::uint8_t  voice   = cue.voice;
        const std::uint16_t opening = windowOf(voice);
        const auto          bit     = static_cast<std::uint8_t>(1u << voice);
        if ((windowsWritten & bit) == 0) {
            const auto loop = static_cast<std::uint16_t>(opening +
                                                         ss::kStreamOpeningBlocks * audio::brr::kBlockBytes);
            // The directory entry: a key-on starts at the opening, and the loop's end block returns to the loop.
            writeDirectory(s, voice, opening, loop);
            windowsWritten = static_cast<std::uint8_t>(windowsWritten | bit);
        }
        const std::uint8_t lastFlags  = (*blocks)[blocks->size() - audio::brr::kBlockBytes] & 0x03;
        const bool         selfEnding = cue.mode.kind != PlayMode::Kind::Continuous || lastFlags == 0x01;
        postStream(s, Voice::StreamChange{.voice = voice, .stream = ss::Stream(blocks, opening, selfEnding)});
        return voice;
    }

    // The delivery to `voice` stopped, if the voice has ever streamed (its window written).
    void stopStream(Voice& s, std::uint8_t voice) {
        if ((windowsWritten & static_cast<std::uint8_t>(1u << voice)) != 0) {
            postStream(s, Voice::StreamChange{.voice = voice});
        }
    }

    // A change to a voice as it plays: whether it feeds the effect path, then its pitch and volumes
    // through the driver, with no key-on. Nothing to change on a system that has cued nothing.
    void applyEffect(const audio::VoiceCue& cue) {
        namespace sd = audio::snesdriver;
        Voice* s = findSamplerVoice();
        if (s == nullptr) {
            return;
        }
        applySend(*s, cue.voice, cue.effect.echo || cue.effect.reverb);
        const AudioEffect& e     = cue.effect;
        const auto         pitch = static_cast<std::uint16_t>(
            std::clamp<long>(std::lround(e.pitch * kPitchAsRecorded), 1, kPitchMax));
        const auto [left, right] = volumes(e.volume, e.pan);
        postGesture(*s, Instruction::write(Location::memory(sd::voiceBlock(cue.voice) + sd::kChangeFieldsOffset),
                                           sd::kChangeFieldsWidth, sd::changeFields(pitch, left, right)));
        postGesture(*s, Instruction::write(Location::memory(sd::voiceBlock(cue.voice)), 1, sd::kChangeCommand));
    }

    // A whole sample written to the cache's placement: its bytes, and the directory entry that plays it
    // round — its start as both the key-on address and the loop a lap returns to.
    void writeWholeSample(Voice& s, std::uint16_t start, std::uint8_t entry,
                          const std::vector<std::uint8_t>& blocks) {
        postWrite(s, snes::audioRam(start), std::vector<std::uint8_t>(blocks));
        writeDirectory(s, entry, start, start);
    }

    // A directory entry: the key-on address and the address a lap loops back to, four bytes at the entry.
    void writeDirectory(Voice& s, std::uint8_t entry, std::uint16_t keyOn, std::uint16_t loop) {
        postWrite(s, snes::audioRam(static_cast<std::uint16_t>(audio::snesdriver::kDirectory + 4 * entry)),
                  std::vector<std::uint8_t>{static_cast<std::uint8_t>(keyOn & 0xFF),
                                            static_cast<std::uint8_t>(keyOn >> 8),
                                            static_cast<std::uint8_t>(loop & 0xFF),
                                            static_cast<std::uint8_t>(loop >> 8)});
    }

    // A voice's fixed streaming window, by the voice's number.
    [[nodiscard]] static std::uint16_t windowOf(std::uint8_t voice) {
        return static_cast<std::uint16_t>(audio::snesdriver::kWindows +
                                          voice * audio::snesstream::kStreamWindowBytes);
    }

    // The encoded bytes of `id`, read and encoded at the call and delivered through the inbox; drains the
    // inbox first in case the bytes were pushed in the same pass as the command that needs them.
    const std::shared_ptr<const std::vector<std::uint8_t>>& encodedFor(AudioId id) {
        const auto index = static_cast<std::size_t>(id);
        if (index >= encodedCache.size() || encodedCache[index] == nullptr) {
            drainEncodedInbox();
        }
        static const std::shared_ptr<const std::vector<std::uint8_t>> none;
        return index < encodedCache.size() ? encodedCache[index] : none;
    }

    // Take delivery of every file encoded at a call since the last drain, into `encodedCache`.
    void drainEncodedInbox() {
        std::vector<std::pair<AudioId, std::shared_ptr<const std::vector<std::uint8_t>>>> taken;
        {
            std::lock_guard<std::mutex> lock(encodedInboxMtx);
            if (encodedInbox.empty()) {
                return;
            }
            taken.swap(encodedInbox);
        }
        for (auto& [id, bytes] : taken) {
            const auto index = static_cast<std::size_t>(id);
            if (index >= encodedCache.size()) {
                encodedCache.resize(index + 1);
            }
            encodedCache[index] = std::move(bytes);
        }
    }

    // Read and encode `id` on the game thread the first time this system cues it, and post the bytes to the
    // production thread. A file that cannot be read or decoded throws here, at the call; an unknown id is
    // quiet, as the cue is.
    void encodeAtCall(AudioId id) {
        const AudioLibrary& library = AudioLibrary::instance();
        const auto          index   = static_cast<std::size_t>(id);
        if (index >= library.size()) {
            return;
        }
        if (index < encodedAtCall.size() && encodedAtCall[index]) {
            return;
        }
        const AudioLibrary::Entry& entry = library.entry(id);
        if (entry.kind != AudioKind::Pcm) {
            throw std::runtime_error(
                "AudioSystem::play: a cue plays a registered audio file through the sound chip — this id "
                "is not an audio file");
        }
        auto bytes = std::make_shared<const std::vector<std::uint8_t>>(encodeSample(entry));
        {
            std::lock_guard<std::mutex> lock(encodedInboxMtx);
            encodedInbox.emplace_back(id, bytes);
        }
        if (index >= encodedAtCall.size()) {
            encodedAtCall.resize(index + 1, false);
        }
        encodedAtCall[index] = true;
    }

    // play(id) of an audio file on an SNES Chiptune system means the default cue, which finds the encoding
    // on the production thread — so the same at-the-call encode runs for it.
    void encodeAtCallIfSnesAudioFile(AudioId id) {
        if (platform_ != VMPlatform::Snes || kind_ != AudioKind::Chiptune) {
            return;
        }
        const AudioLibrary& library = AudioLibrary::instance();
        const auto          index   = static_cast<std::size_t>(id);
        if (index >= library.size() || library.entry(id).kind != AudioKind::Pcm) {
            return;
        }
        encodeAtCall(id);
    }

    // A delivery depends on the clock alone outlasting a window's opening; checked at the call. Neither
    // console's clock trips it.
    void requireStreamableClock() const { audio::snesstream::requireStreamable(*clock_); }

    // Release a voice's hold on its whole sample, if it holds one.
    void releaseHold(std::uint8_t voice) {
        if (heldId[voice].has_value()) {
            sampleCache.release(*heldId[voice]);
            heldId[voice].reset();
        }
        oncePlaying[voice] = false;
    }

    // Release the hold of any once voice the driver has keyed off. The driver's mode byte reads non-zero
    // once the key-on has landed and 0 once the pass ends; a hold releases only after the byte has been
    // seen non-zero (so a key-on still waiting in the runner's mailbox — a tick batches what it has — is
    // never mistaken for a finished pass). The machine's thread publishes the modes, then its step count.
    void releaseEndedHolds() {
        const std::uint64_t steps = samplerSteps.load(std::memory_order_acquire);
        const std::uint64_t modes = samplerModes.load(std::memory_order_relaxed);
        for (std::uint8_t v = 0; v < audio::snesdriver::kVoices; ++v) {
            if (!heldId[v].has_value() || heldMode[v] != PlayMode::Kind::Once || holdSince[v] >= steps) {
                continue;
            }
            if (static_cast<std::uint8_t>((modes >> (8 * v)) & 0xFF) != 0) {
                oncePlaying[v] = true;  // the key-on has landed and the pass is playing
            } else if (oncePlaying[v]) {
                releaseHold(v);  // the pass the driver was playing has ended
            }
        }
    }

    // The machine's thread, after each sampler step: the eight voice mode bytes (relaxed), then the step
    // count (release).
    void publishSamplerModes(Vm& machine) {
        std::uint64_t modes = 0;
        for (std::uint8_t v = 0; v < audio::snesdriver::kVoices; ++v) {
            const std::uint8_t b =
                machine.read(MemoryRegion{.at = audio::snesdriver::voiceBlock(v) + 6, .size = 1}).front();
            modes |= static_cast<std::uint64_t>(b) << (8 * v);
        }
        samplerModes.store(modes, std::memory_order_relaxed);
        samplerSteps.fetch_add(1, std::memory_order_release);
    }

    // A voice feeds the effect path or does not: set or clear its EON bit. Turning a voice onto a set path
    // after stop() muted it writes the configured echo volume back.
    void applySend(Voice& s, std::uint8_t voice, bool feeds) {
        const auto bit = static_cast<std::uint8_t>(1u << voice);
        if (feeds) {
            echoVoices = static_cast<std::uint8_t>(echoVoices | bit);
            if (echo_.set && echo_.muted) {
                writeDsp(s, kEvolL, echo_.level);
                writeDsp(s, kEvolR, echo_.level);
                echo_.muted = false;
            }
        } else {
            echoVoices = static_cast<std::uint8_t>(echoVoices & ~bit);
        }
        writeDsp(s, kEon, echoVoices);
    }

    // The chip registers a configuration lands in: the delay (EDL), the feedback, the echo volume, and the
    // eight-tap filter — straight for an echo, darkening for a reverb.
    struct EchoRegs {
        std::uint8_t                edl      = 0;
        std::uint8_t                feedback = 0;
        std::uint8_t                level    = 0;
        std::array<std::uint8_t, 8> taps{};
    };
    [[nodiscard]] EchoRegs echoRegsFor(const std::variant<Echo, Reverb>& config) const {
        EchoRegs r;
        if (const Echo* e = std::get_if<Echo>(&config)) {
            r.edl = static_cast<std::uint8_t>(
                std::clamp<long>(std::lround(e->delay / kEchoDelayStep), 1, kEchoDelayLengthMax));
            r.feedback = unit127(e->feedback);
            r.level    = unit127(e->level);
            r.taps     = kStraightTaps;
        } else {
            const Reverb& v = std::get<Reverb>(config);
            r.edl          = kReverbDelayLength;
            r.feedback     = static_cast<std::uint8_t>(
                std::lround(std::clamp(v.decay, 0.0f, 1.0f) * kReverbFeedbackAtFullDecay));
            r.level = unit127(v.level);
            r.taps  = kDarkeningTaps;
        }
        return r;
    }

    // Zero the echo ring [base, kRingEnd), so the chip's delay line starts silent rather than reading back
    // the sample bytes that were in that audio RAM — a grow places the ring into freed cache space.
    void clearRing(Voice& s, std::uint16_t base, std::uint8_t edl) {
        const std::size_t bytes = edl == 0 ? 4 : static_cast<std::size_t>(edl) * 2048;
        postWrite(s, snes::audioRam(base), std::vector<std::uint8_t>(bytes, 0));
    }

    // The chip's one echo path, set for the whole system — the most recent call wins. An echo is a buffer
    // the chip writes its output into and reads back a delay later, fed back through an eight-tap filter
    // passing the sound straight; a reverb is the same path short, fed back through a filter that darkens
    // each pass. The parts that do not depend on where the ring sits — feedback, echo volume, the taps,
    // echo writes on — are written now; the ring's base moves with the delay: a longer delay grows the
    // ring down into the cache (waiting on a sample a voice is playing in its way), a shorter one shrinks
    // it back up once the old ring's length has elapsed. A voice's send (EON) is left as it is — a voice
    // feeds the path the moment one is set.
    void setEchoPath(const std::variant<Echo, Reverb>& config) {
        Voice&         s       = samplerVoice();  // hosts the sampler if this is the first thing on the system
        const EchoRegs r       = echoRegsFor(config);
        const auto     baseNew = static_cast<std::uint16_t>(audio::snesdriver::kRingEnd - 2048 * r.edl);
        const auto     pageNew = static_cast<std::uint8_t>(baseNew >> 8);
        writeDsp(s, kEfb, r.feedback);
        writeDsp(s, kEvolL, r.level);
        writeDsp(s, kEvolR, r.level);
        for (std::size_t i = 0; i < r.taps.size(); ++i) {
            writeDsp(s, static_cast<std::uint8_t>(kFirBase + 0x10 * i), r.taps[i]);
        }
        writeDsp(s, kFlg, kFlgEchoOn);
        echo_.set         = true;
        echo_.level       = r.level;
        echo_.muted       = false;
        echo_.growPending = false;  // a later setting drops a pending grow for itself
        if (r.edl > echo_.edl) {
            echo_.shrinkPending = false;  // a grow cancels a deferred shrink's second half
            if (sampleCache.lowerLimit(baseNew)) {
                echo_.growPending = true;  // a sample a voice is playing is in the way — the chip waits
                echo_.pendingBase = baseNew;
                echo_.pendingEdl  = r.edl;
            } else {
                clearRing(s, baseNew, r.edl);
                writeDsp(s, kEsa, pageNew);
                writeDsp(s, kEdl, r.edl);
                echo_.edl  = r.edl;
                echo_.base = baseNew;
            }
        } else if (r.edl < echo_.edl) {
            const std::uint8_t oldEdl = echo_.edl;
            writeDsp(s, kEdl, r.edl);  // the new size now; the base once the old ring's length has elapsed
            echo_.edl           = r.edl;
            echo_.shrinkPending = true;
            echo_.shrinkBase    = baseNew;
            echo_.shrinkEsaAt   = samplerSteps.load(std::memory_order_acquire) + oldEdl + 1;
        }
        // r.edl == echo_.edl: the registers above are the whole of it.
    }

    // Each produce pass, after the queue: commit a grow once the sample in the ring's way has released, and
    // move a shrunk ring's base once the old ring's length has elapsed.
    void serviceEcho() {
        Voice* sp = findSamplerVoice();
        if (sp == nullptr) {
            return;
        }
        Voice& s = *sp;
        if (echo_.growPending && !sampleCache.anyHeldAbove(echo_.pendingBase)) {
            sampleCache.lowerLimit(echo_.pendingBase);  // drop what is now unheld in the way
            clearRing(s, echo_.pendingBase, echo_.pendingEdl);
            writeDsp(s, kEsa, static_cast<std::uint8_t>(echo_.pendingBase >> 8));
            writeDsp(s, kEdl, echo_.pendingEdl);
            echo_.edl         = echo_.pendingEdl;
            echo_.base        = echo_.pendingBase;
            echo_.growPending = false;
        }
        if (echo_.shrinkPending && samplerSteps.load(std::memory_order_acquire) >= echo_.shrinkEsaAt) {
            writeDsp(s, kEsa, static_cast<std::uint8_t>(echo_.shrinkBase >> 8));
            sampleCache.raiseLimit(echo_.shrinkBase);
            echo_.base          = echo_.shrinkBase;
            echo_.shrinkPending = false;
        }
    }

    // The S-DSP's echo path, by register.
    static constexpr std::uint8_t kEfb     = 0x0D;  // the feedback
    static constexpr std::uint8_t kFirBase = 0x0F;  // C0; C1..C7 follow at $1F..$7F
    static constexpr std::uint8_t kEvolL   = 0x2C;  // the echo's volume, left
    static constexpr std::uint8_t kEvolR   = 0x3C;  // and right
    static constexpr std::uint8_t kEon     = 0x4D;  // which voices feed the path
    static constexpr std::uint8_t kFlg     = 0x6C;  // the flags: mute, echo writes, the noise clock
    static constexpr std::uint8_t kEsa     = 0x6D;  // the buffer's page
    static constexpr std::uint8_t kEdl     = 0x7D;  // the delay, in steps of 2 KB and 16 ms
    static constexpr std::uint8_t kFlgEchoOn = 0x00;  // unmuted, echo writes on, noise clock 0
    static constexpr float        kEchoDelayStep = 0.016f;  // one EDL step at 32 kHz
    static constexpr long         kEchoDelayLengthMax = 15;
    // A reverb: passes 32 ms apart, each one darkened; at a decay of 1 each pass keeps a little under
    // four fifths of the last, a tail of seconds that still dies away.
    static constexpr std::uint8_t kReverbDelayLength         = 2;
    static constexpr float        kReverbFeedbackAtFullDecay = 100.0f;
    static constexpr std::array<std::uint8_t, 8> kStraightTaps{0x7F, 0, 0, 0, 0, 0, 0, 0};
    static constexpr std::array<std::uint8_t, 8> kDarkeningTaps{0x3A, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00};
    // The chip's pitch: $1000 plays a sample at the rate it was encoded at; the register holds 14 bits.
    static constexpr float kPitchAsRecorded = 4096.0f;
    static constexpr long  kPitchMax        = 0x3FFF;

    // Which thread steps a new voice's machine. A system with a production thread gives every machine a
    // thread of its own; a manual one has no threads at all, so its machines step from the calling
    // thread and a test stays deterministic.
    [[nodiscard]] vm::VmRunner::Mode runnerMode() const {
        return threaded ? vm::VmRunner::Mode::Threaded : vm::VmRunner::Mode::Inline;
    }

    // A lane holds the whole latency target plus two steps: a machine parks once the pipeline reaches the
    // target, and with an empty ring the entire target can be sitting in this one lane — leaving room for
    // the step it is in the middle of and a step of margin. Within that, a lane cannot overflow, which is
    // why the APU callback pushes without inspecting the result. A Pcm voice has no machine and no lane.
    [[nodiscard]] std::size_t laneCapacity() const {
        return kind_ == AudioKind::Chiptune || kind_ == AudioKind::HostDriven ? targetFrames + 2 * framesPerStep
                                                                             : 0;
    }

    // The demand a caller passes when it takes whatever has been produced, at whatever pace it likes —
    // the golden-gate seam, which advances a machine by a chosen number of cycles and captures exactly
    // what that produced.
    static constexpr std::size_t kMixEverything = SIZE_MAX;

    // How many frames the output is short of its latency buffer — the pace every produce pass mixes at,
    // whether it stepped the machines itself or they stepped themselves. A tail is finished rather than
    // paced: a voice riding its release fade is a few hundred frames from being gone, and holding those
    // back would leave it sounding until something drains the ring.
    [[nodiscard]] std::size_t framesTheOutputWants() const {
        if (anyReleasing()) {
            return kMixEverything;
        }
        const std::size_t buffered = ring.sizeApprox();
        return targetFrames > buffered ? targetFrames - buffered : 0;
    }

    // Give a voice's machine its thread, and the pacing it runs under: it produces while the frames
    // waiting downstream of it — its own lane plus the output buffer they are mixed into — come to less
    // than the latency target, and parks there. What a machine runs ahead by is therefore the whole
    // pipeline's inventory, not its lane's alone, and one step overshoots the target by at most a step:
    // the same standing inventory the machines keep when the produce pass steps them itself.
    //
    // A finishing machine drops the buffer from that sum and paces against its own lane, because the
    // frames its fade rides are frames it has yet to produce — the mix takes them the moment they land,
    // whatever the output is holding, which is what lets stop() reach silence with nothing draining.
    //
    // On a manual system this does nothing — those machines are stepped by hand.
    void startRunner(Voice& v) {
        if (!threaded) {
            return;
        }
        Voice* vp = &v;
        v.runner->start(
            [this, vp] {
                return vp->finishing.load(std::memory_order_relaxed)
                           ? vp->lane.sizeApprox()
                           : vp->lane.sizeApprox() + ring.sizeApprox();
            },
            targetFrames);
    }

    // Build a resident voice: give the voice its runner, resolve its images to bytes, and declare what
    // the machine's own thread does before its first step — place the images, run the declared .init
    // once (inside hostDriver), then switch the APU's frames into the voice's lane. audio(true, …) runs
    // AFTER hostDriver because hostDriver resets the machine — the rate and the function are set once
    // the reset is behind us. The voice rides the VMDriver bus (its `type`).
    void initResidentVoice(Voice& v) {
        v.runner = std::make_unique<vm::VmRunner>(vm::VmCoreAccess::make(core_, platform_, timing_),
                                                  vm::VmRunner::StepKind::Resident, cyclesPerFrame,
                                                  runnerMode());

        // Reading a registry or the disk is not machine mutation, so it happens here, on the audio
        // thread — which also keeps a missing or unassemblable image an error at the call that asked
        // for the driver. The bytes travel to the machine's thread, which is what places them.
        std::vector<std::vector<std::uint8_t>> owned;
        owned.reserve(v.residentDef.images.size());
        for (const StoredDriverImage& img : v.residentDef.images) {
            owned.push_back(resolveDriverImageBytes(img, v.runner->machine()));
        }

        Voice* vp      = &v;
        Vm*    machine = &v.runner->machine();
        v.runner->beforeFirstStep([this, vp, machine, owned = std::move(owned)] {
            std::vector<DriverImage> images;
            images.reserve(owned.size());
            for (std::size_t i = 0; i < owned.size(); ++i) {
                images.push_back(DriverImage{.bytes = std::span<const std::uint8_t>(owned[i]),
                                             .base  = vp->residentDef.images[i].base});
            }
            DriverBinding binding;
            binding.images    = std::move(images);
            binding.mapper    = vp->residentDef.mapper;
            binding.tickEntry = vp->residentDef.tickEntry;
            binding.stackTop  = vp->residentDef.stackTop;
            binding.slots     = vp->residentDef.slots;  // the Vm needs the specs so readSlot can publish them
            binding.init      = vp->residentDef.init;
            binding.isa       = vp->residentIsa;
            machine->hostDriver(binding);  // place + validate + run .init once (verifies the ISA vs the platform)

            machine->audio(true, {.rate = sampleRate, .onSample = [vp](std::int16_t left, std::int16_t right) {
                // Scale by the VMDriver bus (a straight amplifier over the driver's own internal
                // mixing). At the default unity gain this is the exact identity, so the laned frame is
                // the driver's own.
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(vp->type);
                vp->lane.push(AudioFrame{applyGain(left, gain), applyGain(right, gain)});
            }});
        });
        // Each step publishes what that step left in the declared slots, so the game-thread handle
        // always reads a coherent set from the most recent tick, and the sampler's step then feeds every
        // file it streams. The hook holds the MACHINE, not the
        // voice's pointer to its runner: closing a voice releases that pointer before it waits for the
        // thread, so a hook that went through it would find nothing there for exactly as long as the
        // wait lasts. A runner's machine keeps its address for the runner's whole life, and the runner
        // does not release it until its thread has left.
        v.runner->afterEachStep([this, vp, machine] {
            publishSnapshot(*vp, *machine);
            if (vp->sampler) {
                feedStreams(*vp, *machine);
                publishSamplerModes(*machine);
            }
        });
        // What the system itself writes into the machine — the sampler's samples, chip settings and the
        // deliveries it starts and stops — lands on the machine's thread at its next look, ahead of the
        // step that follows.
        v.runner->onEachLook([this, vp, machine] { serveWrites(*vp, *machine); });
        startRunner(v);
    }

    // Publish one frame's declared-slot values as a coherent snapshot for the game-thread handle. Read
    // every slot (write-only mailboxes read back harmlessly — the handle surfaces only readable ones).
    // Runs on the thread stepping `machine`, which is why the machine is passed rather than reached
    // through the voice.
    void publishSnapshot(Voice& v, Vm& machine) {
        if (!v.snapshot) {
            return;
        }
        std::vector<std::uint64_t> values(v.residentDef.slots.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = machine.readSlot(i);
        }
        v.snapshot->publish(values);
    }

    // host()'s game-thread half: validate the system kind and the one-instance rule, create the resident
    // voice shell + its shared snapshot, hand the shell to production through the inbox, and mark playing.
    void hostDriver(AudioId driver, std::size_t slotCount) {
        if (kind_ != AudioKind::Chiptune) {
            throw std::runtime_error(
                "AudioSystem::host: only a Chiptune audio system can host a resident sound driver");
        }
        const std::size_t idx = static_cast<std::size_t>(driver);
        if (idx >= driverSnapshots.size()) {
            driverSnapshots.resize(idx + 1);
        }
        if (driverSnapshots[idx] != nullptr) {
            throw std::runtime_error(
                "AudioSystem::host: this driver is already hosted on this system (one resident machine "
                "per registration per system)");
        }
        auto snapshot = std::make_shared<DriverSnapshot>(slotCount);
        driverSnapshots[idx] = snapshot;

        const AudioLibrary::Entry& entry = AudioLibrary::instance().entry(driver);
        auto voice          = std::make_unique<Voice>(laneCapacity());
        voice->resident     = true;
        voice->id           = driver;
        voice->type         = AudioType::VMDriver;
        voice->residentDef  = *entry.driver;  // copy so production never reads the (growable) library
        voice->residentIsa  = entry.isa;
        voice->snapshot     = std::move(snapshot);
        {
            std::lock_guard<std::mutex> lock(hostInboxMtx);
            hostInbox.push_back(std::move(voice));
        }
        playing.store(true, std::memory_order_relaxed);
    }

    // Release the id's snapshot slot, so the driver can be hosted again. This is close()'s game-thread
    // half — the voice itself closes from the queued cue, and it keeps the snapshot alive through its
    // own shared_ptr until it is destroyed, so a publish in flight is unaffected. The one-machine-per-
    // registration rule is about machines that are live, not about ids that have ever been used.
    void releaseDriverSnapshot(AudioId driver) {
        const std::size_t idx = static_cast<std::size_t>(driver);
        if (idx < driverSnapshots.size()) {
            driverSnapshots[idx].reset();
        }
    }

    // The game-thread handle's coherent slot read: the whole published block for `driver` (empty if the id
    // is not hosted here). Game-thread only — production never touches driverSnapshots (the vector).
    [[nodiscard]] std::vector<std::uint64_t> readDriverSnapshot(AudioId driver) const {
        const std::size_t idx = static_cast<std::size_t>(driver);
        if (idx >= driverSnapshots.size() || driverSnapshots[idx] == nullptr) {
            return {};
        }
        return driverSnapshots[idx]->read();
    }

    // The game-thread handle's starvation count: frames of silence the mix substituted for `driver`
    // (zero if the id is not hosted here). One relaxed word — nothing else observes with it.
    [[nodiscard]] std::size_t readDriverUnderflow(AudioId driver) const {
        const std::size_t idx = static_cast<std::size_t>(driver);
        if (idx >= driverSnapshots.size() || driverSnapshots[idx] == nullptr) {
            return 0;
        }
        return driverSnapshots[idx]->laneUnderflow.load(std::memory_order_relaxed);
    }

    // Apply a Play cue: bounds-check the id, verify its kind matches THIS system's kind, and start a NEW
    // voice for it. Under Layer (the fixed default) the voice starts beside the ones already sounding —
    // play() never touches a playing voice. Under Retrigger the voices already playing the SAME id are
    // closed first (the arcade re-fire restarts the sound); every other voice plays on untouched. A
    // Chiptune voice gets its own VM with the driver materialized into it; a Pcm voice gets a share of
    // the decoded frames. Backend-owning, so this runs ONLY on the production thread (or the manual
    // calling thread).
    void applyPlay(AudioId id, CueMode mode) {
        const AudioLibrary& library = AudioLibrary::instance();
        const auto index = static_cast<std::size_t>(id);
        if (index >= library.size()) {
            return;  // unknown handle — nothing to cue
        }
        const AudioLibrary::Entry& entry = library.entry(id);
        // An audio file on an SNES system of the Chiptune kind plays through the console's sound chip:
        // play(id) is the cue with every default — voice 0, as recorded, full, in the middle.
        if (entry.kind == AudioKind::Pcm && kind_ == AudioKind::Chiptune && platform_ == VMPlatform::Snes) {
            applyPlayCue(id, audio::VoiceCue{});
            return;
        }
        // One kind per system: an id of the other backend cannot play here (the ISA-mismatch precedent —
        // a cheap enum compare before any placement / decode, so a mismatch throws loudly).
        if (entry.kind != kind_) {
            throw std::runtime_error(
                "AudioSystem::play: this audio was registered for a different backend (chiptune vs PCM) "
                "than this audio system — it cannot play here");
        }
        if (mode == CueMode::Retrigger) {
            // Restart THIS sound: the voices already playing the same id enter their release fade (a
            // hard cut would click) while the fresh voice below takes over. Everything else keeps
            // sounding untouched.
            for (const std::unique_ptr<Voice>& v : voices) {
                if (v->id == id) {
                    beginRelease(*v);
                }
            }
        }
        auto voice  = std::make_unique<Voice>(laneCapacity());
        voice->id   = id;
        voice->type = entry.type;
        if (kind_ == AudioKind::Chiptune) {
            // The voice's own machine, behind its runner. The bytes are resolved here and placed by the
            // thread that steps the machine; the sample callback's raw-pointer capture is safe because
            // the voice lives behind unique_ptr (stable address) and its machine leaves before it does.
            voice->runner = std::make_unique<vm::VmRunner>(
                vm::VmCoreAccess::make(core_, platform_, timing_), vm::VmRunner::StepKind::Started,
                cyclesPerFrame, runnerMode());
            if (index >= asmCache.size()) {
                asmCache.resize(index + 1);
            }
            std::vector<std::uint8_t> bytes =
                resolveChiptuneBytes(voice->runner->machine(), entry, asmCache[index]);

            Voice* vp      = voice.get();
            Vm*    machine = &voice->runner->machine();
            voice->runner->beforeFirstStep([this, vp, machine, isa = entry.isa, bytes = std::move(bytes)] {
                machine->audio(true, {.rate = sampleRate, .onSample = [vp](std::int16_t left, std::int16_t right) {
                    // Auto-close bookkeeping: track the run of consecutive exact-zero output frames. A
                    // finished one-shot SFX's DAC-on tail settles to exact (0,0) (verified against the
                    // real drivers), while an active tone is high-pass-centred and oscillates through 0
                    // — so the run only reaches the threshold once the sound has actually stopped. Keyed
                    // off the RAW pre-gain sample: a low mixer level could round a still-playing quiet
                    // tone to zero, and that must not count as the sound stopping.
                    const std::size_t run = vp->silenceRun.load(std::memory_order_relaxed);
                    vp->silenceRun.store((left == 0 && right == 0) ? run + 1 : 0,
                                         std::memory_order_relaxed);
                    // Scale by the voice's bus level (one relaxed atomic load + one integer
                    // multiply-shift per channel). At the default unity gain this is the exact identity,
                    // so the laned frame is bit-for-bit the produced one.
                    const std::uint32_t gain = AudioMixer::instance().effectiveGain(vp->type);
                    vp->lane.push(AudioFrame{applyGain(left, gain), applyGain(right, gain)});
                }});
                // The chiptune binding carries the machine's own ISA: SM83 on a Game Boy (unchanged), the
                // 65816 on the SNES. Every other field is the shared chiptune binding's.
                RoutineBinding binding = kChiptuneBinding;
                binding.isa            = isa;
                vp->driver             = machine->uploadRoutine<void()>(bytes, binding);
                machine->startDriver(*vp->driver);
            });
            startRunner(*voice);
        } else {  // AudioKind::Pcm — no VM: decode the file once, then the voice plays the shared frames
            if (index >= pcmCache.size()) {
                pcmCache.resize(index + 1);
            }
            if (pcmCache[index] == nullptr) {
                pcmCache[index] =
                    std::make_shared<const std::vector<AudioFrame>>(decodePcmEntry(entry));
            }
            voice->pcm    = pcmCache[index];
            voice->cursor = 0;
        }
        voices.push_back(std::move(voice));
        playing.store(true, std::memory_order_relaxed);
    }

    // Decode a Pcm entry's audio file into stereo frames at this system's sample rate — a `.brr` sample
    // through the chip format's own decoder, any other file through the audio-file decoder.
    std::vector<AudioFrame> decodePcmEntry(const AudioLibrary::Entry& entry) {
        if (detail::endsWith(entry.asmPath, ".brr")) {
            return audio::brr::decodeToFrames(readEntryBytes(entry), sampleRate);
        }
        return decodeFile(readEntryBytes(entry), sampleRate);
    }

    // An audio file's bytes decoded to stereo frames at `rate`, through the hook the audio-file
    // registration installs — never by naming the decoder directly, which is what keeps the decoder out
    // of binaries that register no audio file. A Pcm entry only exists because that registration ran, so
    // the hook is always set here; the guard is defensive.
    static std::vector<AudioFrame> decodeFile(std::span<const std::uint8_t> file, unsigned rate) {
        if (detail::g_pcmDecode == nullptr) {
            throw std::runtime_error(
                "AudioSystem::play: PCM decoder is not linked — no audio file was registered");
        }
        return detail::g_pcmDecode(file, rate);
    }

    // A Pcm entry's file bytes. Embed → the build baked them into the asset registry (keyed by the
    // logical path); otherwise the file ships beside the binary and is read from assetRoot(). PCM's
    // per-type default is LoadFromPath.
    static std::vector<std::uint8_t> readEntryBytes(const AudioLibrary::Entry& entry) {
        if (resolveAssetPolicy(entry.policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
            if (const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(entry.asmPath);
                !baked.empty()) {
                return std::vector<std::uint8_t>(baked.begin(), baked.end());
            }
            detail::warnEmbedNotBaked("asset", entry.asmPath);
        }
        const std::filesystem::path full = assetRoot() / std::filesystem::path(entry.asmPath);
        std::ifstream in{full, std::ios::binary};
        if (!in) {
            throw std::runtime_error("AudioSystem::play: cannot open audio file: " + full.string());
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string contents = ss.str();
        return std::vector<std::uint8_t>(contents.begin(), contents.end());
    }

    // Enter the release fade: the voice's remaining output rides a short linear ramp to zero
    // (rampFrame) and the voice is removed when it lands. Idempotent — a voice already releasing keeps
    // its ramp position (a second stop()/Retrigger does not restart the fade).
    void beginRelease(Voice& v) const {
        if (v.releasing) {
            return;
        }
        v.releasing     = true;
        v.rampTotal     = releaseFrames;
        v.rampRemaining = releaseFrames;
        v.finishing.store(true, std::memory_order_relaxed);
    }

    // Whether any voice is riding its release fade — production continues past the latency target for
    // these (the tail is a few hundred frames; the ring's capacity, several times the target, absorbs
    // it), so stop() reaches silence even when nothing is draining the ring.
    [[nodiscard]] bool anyReleasing() const {
        for (const auto& v : voices) {
            if (v->releasing) {
                return true;
            }
        }
        return false;
    }

    // ── The mixdown ────────────────────────────────────────────────────────────────────────────────────
    // Advance by `wanted` frames — as many as the output is short — bounded by what the lanes hold, sum
    // position-by-position (saturating), and push the mixed frames to the ring.
    //
    // How far the lanes bound it depends on how much the output still has in hand. While the buffer holds
    // more than its floor there is time to wait, so the pass takes only what EVERY machine has ready and
    // comes back for the rest: a machine a few milliseconds late costs nobody anything, and the frames
    // its healthier neighbours have parked are inventory rather than starvation. A machine that has yet
    // to produce its first frame is still building, and a voice riding its release fade is finished
    // rather than paced, so neither one holds the pass back.
    //
    // Once the buffer is down to the floor the wait is over: the pass advances by what the MOST-advanced
    // lane holds, a voice with fewer frames contributes silence for the shortfall, and that shortfall is
    // counted against it. A machine stalled for longer than the buffer's cushion mutes itself and no one
    // else, and its late frames still play, after a gap, with nothing dropped from its stream. So the
    // counted figure is real starvation — a stall the output could not absorb — and never scheduling
    // jitter. A caller that owns the stepping itself passes an unbounded demand: it advances every
    // machine in lockstep, so there is no pace to keep and nothing to wait for.
    //
    // The output's own demand matters as much as the lane bound. Without it the mix would advance by
    // whatever the deepest lane happened to hold, and since machines run ahead by whole steps, a machine
    // merely one step less far ahead than its neighbour would read as starving — every voice but the
    // deepest padded with a step of silence, worse the more machines there are. Without the lane bound, a
    // pass where nothing has been produced yet would stuff silence in front of frames that are merely
    // late; instead it produces nothing and the ring's own underflow covers it.
    //
    // With one voice the sum is the identity and the shortfall is always zero, so a lone sound's bytes
    // reach the ring exactly as its APU produced them. A ring-full push drops that mixed frame and
    // counts it — the mix never blocks on a full ring.
    void mixLanes(std::size_t wanted) {
        if (voices.empty()) {
            return;
        }
        std::size_t n           = 0;
        bool        everyoneHas = false;
        if (wanted != kMixEverything && ring.sizeApprox() > ringFloor) {
            std::size_t least = SIZE_MAX;
            for (const auto& v : voices) {
                if (v->produced && !v->releasing) {
                    least = std::min(least, v->lane.sizeApprox());
                }
            }
            if (least != SIZE_MAX) {
                n           = least;
                everyoneHas = true;
            }
        }
        if (!everyoneHas) {
            for (const auto& v : voices) {
                n = std::max(n, v->lane.sizeApprox());
            }
        }
        n = std::min(n, wanted);
        if (n == 0) {
            return;
        }
        for (const auto& v : voices) {
            v->taken.resize(n);  // holds its capacity between passes, so a warm mix allocates nothing
            v->tookFrames = v->lane.pop(std::span<AudioFrame>(v->taken));
            if (v->tookFrames > 0) {
                v->produced = true;
            }
            if (v->produced) {
                v->laneUnderflow += n - v->tookFrames;
                laneUnderflowTotal.fetch_add(n - v->tookFrames, std::memory_order_relaxed);
                if (v->snapshot) {
                    // A resident machine publishes its own count to its handle, from here — where the
                    // substitution is made — so the game thread reads it through the object it already
                    // reads slots from.
                    v->snapshot->laneUnderflow.store(v->laneUnderflow, std::memory_order_relaxed);
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            mixScratch.clear();
            for (const auto& v : voices) {
                const AudioFrame f = i < v->tookFrames ? v->taken[i] : AudioFrame{};
                mixScratch.push_back(
                    v->releasing
                        ? detail::rampFrame(f, v->rampRemaining > i ? v->rampRemaining - i : 0,
                                            v->rampTotal)
                        : f);
            }
            if (!ring.push(detail::mixFrames(mixScratch))) {
                framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        for (const auto& v : voices) {
            if (v->releasing) {
                v->rampRemaining = v->rampRemaining > n ? v->rampRemaining - n : 0;
            }
        }
    }

    // One produce pass: top the ring back up to its latency target, then close the voices that finished
    // (a one-shot SFX gone silent; a PCM buffer exhausted). Where the machines run decides what the pass
    // does. On a threaded system they run beside it, so the pass takes what they produced and lets them
    // run on; holding off while the ring is already at its target is what paces them, since a machine
    // parks once its lane and the ring together hold the target and waits for this pass to draw them
    // down. On a manual system the pass steps every machine itself and mixes after each step.
    void produceOnce() {
        if (kind_ == AudioKind::Pcm) {
            producePcmMix();
            return;
        }
        if (threaded) {
            if (!voices.empty() && (ring.sizeApprox() < targetFrames || anyReleasing())) {
                mixLanes(framesTheOutputWants());
                for (const auto& v : voices) {
                    v->runner->wake();
                }
            }
        } else {
            for (int n = 0; n < maxStepsPerWake && !voices.empty() &&
                            (ring.sizeApprox() < targetFrames || anyReleasing());
                 ++n) {
                for (const auto& v : voices) {
                    // One frame of this voice's machine — a resident driver performs its queued
                    // gestures, calls its tick and idles the remainder; a started driver simply runs the
                    // budget. The APU lanes ~one frame's worth of samples either way, and a resident
                    // voice's step publishes its read-slot snapshot on the way out.
                    v->runner->stepOnce();
                }
                mixLanes(framesTheOutputWants());
            }
        }
        closeFinishedVoices();
    }

    // Close the voices that finished: a fade that landed at zero, and the auto-close of a one-shot SFX
    // whose output has been exact-zero past the threshold. Music/Vocals voices and a hosted resident
    // driver (VMDriver) are never auto-closed; the other voices play on untouched. A closing voice's
    // machine is asked to leave and the voice is set aside, then destroyed by a later pass once that
    // machine has left — an inline machine has already left, so a manual system closes in this pass.
    // `playing` tracks whether any voice remains.
    void closeFinishedVoices() {
        for (std::size_t i = 0; i < voices.size();) {
            Voice& v = *voices[i];
            const bool finished =
                (v.releasing && v.rampRemaining == 0) ||
                detail::shouldAutoStop(v.silenceRun.load(std::memory_order_relaxed),
                                       autoStopSilenceFrames, v.type);
            if (!finished) {
                ++i;
                continue;
            }
            if (v.runner) {
                v.runner->requestStop();
                v.runner->wake();
            }
            closing.push_back(std::move(voices[i]));
            voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
        }
        std::erase_if(closing, [](const std::unique_ptr<Voice>& v) {
            return v->runner == nullptr || v->runner->finished();
        });
        playing.store(!voices.empty(), std::memory_order_relaxed);
    }

    // Ask every machine to leave before any of them is waited on, so shutdown costs one step rather than
    // one step per machine. The waiting itself happens as each voice is destroyed.
    void stopAllRunners() {
        for (const std::vector<std::unique_ptr<Voice>>* set : {&voices, &closing}) {
            for (const std::unique_ptr<Voice>& v : *set) {
                if (v->runner) {
                    v->runner->requestStop();
                    v->runner->wake();
                }
            }
        }
    }

    // One PCM produce pass: stream every voice's decoded frames forward together, summing at each
    // position, up to the latency target. A voice whose buffer is exhausted contributes nothing and is
    // removed after the pass; the others play on. A ring-full push consumes nothing (all cursors hold),
    // so the next pass resumes exactly where this one stopped. At unity gain with a single voice the
    // streamed frames are the decoded bytes unchanged.
    void producePcmMix() {
        while (!voices.empty() && (ring.sizeApprox() < targetFrames || anyReleasing())) {
            // A voice at its buffer's end enters release: the tail decays from the final frame, so a
            // file whose last sample sits off zero still lands at silence instead of clicking.
            for (const auto& v : voices) {
                if (!v->releasing && v->cursor >= v->pcm->size()) {
                    v->lastFrame = v->pcm->empty() ? AudioFrame{} : v->pcm->back();
                    beginRelease(*v);
                }
            }
            mixScratch.clear();
            for (const auto& v : voices) {
                if (v->releasing && v->rampRemaining == 0) {
                    continue;  // tail finished — removed below
                }
                const AudioFrame src =
                    v->cursor < v->pcm->size() ? (*v->pcm)[v->cursor] : v->lastFrame;
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(v->type);
                AudioFrame f{applyGain(src.left, gain), applyGain(src.right, gain)};
                if (v->releasing) {
                    f = detail::rampFrame(f, v->rampRemaining, v->rampTotal);
                }
                mixScratch.push_back(f);
            }
            if (mixScratch.empty()) {
                break;  // every voice done — the erase below closes them
            }
            if (!ring.push(detail::mixFrames(mixScratch))) {
                break;  // ring full — nothing consumed; resume here next pass
            }
            for (const auto& v : voices) {
                if (v->cursor < v->pcm->size()) {
                    ++v->cursor;
                }
                if (v->releasing && v->rampRemaining > 0) {
                    --v->rampRemaining;
                }
            }
        }
        std::erase_if(voices, [](const std::unique_ptr<Voice>& v) {
            return v->releasing && v->rampRemaining == 0;
        });
        playing.store(!voices.empty(), std::memory_order_relaxed);
    }

    // ── Production thread ────────────────────────────────────────────────────────────────────────────
    // Start the self-pacing production thread (the threaded ctors call this after construction; the
    // manual test ctor does not, leaving `threaded` false). From here the voices are touched ONLY by this
    // thread — applyPlay/produceOnce/drainCues never run elsewhere.
    void startProductionThread() {
        threaded = true;
        hostAudioUnitIfHostDriven();  // its thread is its own; the production thread only mixes it
        running.store(true, std::memory_order_relaxed);
        productionThread = std::thread([this] { productionLoop(); });
    }

    // Signal the loop to exit, wake it, and join — a no-op in manual mode (no thread was started). MUST
    // run before the Impl members (voices/ring/cueQueue) destruct, since the thread is their only producer.
    void stopProductionThread() {
        if (!productionThread.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            running.store(false, std::memory_order_relaxed);
        }
        cv.notify_one();
        productionThread.join();
    }

    // The self-pacing loop: drain cues, produce while playing, then park — timed while playing (periodic
    // refill), untimed while idle (zero wakeups until a cue). Woken early by play()/stop()/shutdown.
    void productionLoop() {
        for (;;) {
            drainCues();
            if (playing.load(std::memory_order_relaxed)) {
                produceOnce();
            }

            std::unique_lock<std::mutex> lock(mtx);
            if (!running.load(std::memory_order_relaxed)) {
                break;
            }
            // A cue that landed during the produce pass above is visible here under the lock — handle it
            // without waiting (closes the lost-wakeup window: play()/stop() push THEN take this mutex to
            // notify, so a push is always observed either here or by the wait predicate below).
            if (cueQueue.sizeApprox() > 0) {
                continue;
            }
            if (playing.load(std::memory_order_relaxed)) {
                cv.wait_for(lock, kProductionWaitInterval);  // periodic device-paced refill
            } else {
                cv.wait(lock, [this] {
                    return !running.load(std::memory_order_relaxed) || cueQueue.sizeApprox() > 0;
                });  // park until a cue arrives or shutdown
            }
        }
    }

    // Enqueue is on the main thread; this routes the wake: a started production thread is notified, while
    // manual mode (no thread) applies the cue inline on the calling thread so a test sees it immediately.
    void wakeOrApply() {
        if (threaded) {
            {
                std::lock_guard<std::mutex> lock(mtx);  // order the push before the production thread's
            }                                            // predicate check / wait entry (no lost wakeup)
            cv.notify_one();
        } else {
            drainCues();
        }
    }
};

AudioSystem::AudioSystem(detail::CoreFactory core, AudioKind kind, AudioSink& sink, VMPlatform platform,
                         TimingProfile timing, unsigned sampleRate)
    : impl_(std::make_unique<Impl>(core, kind, sink, platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

AudioSystem::AudioSystem(detail::CoreFactory core, AudioKind kind, std::unique_ptr<AudioSink> sink,
                         VMPlatform platform, TimingProfile timing, unsigned sampleRate)
    : impl_(std::make_unique<Impl>(core, kind, std::move(sink), platform, timing, sampleRate)) {
    impl_->startProductionThread();
}

// The zero-boilerplate default: own an internally-constructed production sink. Delegates to the owning
// ctor with a fresh SdlAudioSink — adds only an include, no new library dependency (sdl_platform.cpp is
// already in this static lib). A non-SDL audio backend uses the injection seam (the two ctors above).
AudioSystem::AudioSystem(detail::CoreFactory core, AudioKind kind, VMPlatform platform,
                         TimingProfile timing, unsigned sampleRate)
    : AudioSystem(core, kind, std::make_unique<SdlAudioSink>(), platform, timing, sampleRate) {}

// Manual (thread-suppressed) construction for the internal test seam. Borrows `sink`; leaves `threaded`
// false so play()/stop() apply inline and the test drives production via AudioSystemTestAccess.
AudioSystem::AudioSystem(ManualTag, detail::CoreFactory core, AudioKind kind, AudioSink& sink,
                         VMPlatform platform, TimingProfile timing, unsigned sampleRate)
    : impl_(std::make_unique<Impl>(core, kind, sink, platform, timing, sampleRate)) {
    impl_->hostAudioUnitIfHostDriven();  // stepped by the test, through the seam
}

AudioSystem::~AudioSystem() {
    // Stop the sink first so its audio thread stops pulling the ring, THEN join the production thread so
    // it stops pushing the ring and touching the voices, THEN ask every machine to leave so they all
    // finish their step at once, THEN the Impl (cue queue + ring + voices + each voice's machine) is
    // destroyed — no produced or pulled frame touches a dead ring or machine. Waiting for a machine is
    // correct here: this is where the last one is torn down.
    impl_->sink.stop();
    impl_->stopProductionThread();
    impl_->stopAllRunners();
}

void AudioSystem::play(AudioId id, CueMode mode) {
    impl_->encodeAtCallIfSnesAudioFile(id);  // an SNES audio file is read and encoded here, at the call
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Play, id, mode});
    impl_->wakeOrApply();
}

void AudioSystem::stop() {
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Stop, AudioId{}});
    impl_->wakeOrApply();
}

// The SNES's voice surface and effect path. Each rides the same channel as every other cue, so it keeps
// its order with them; the file a cue plays is read and encoded here, at the call, and whether this is a
// Chiptune system is settled here too, before anything is queued.
void AudioSystem::SNES::play(AudioId id, const Cue& cue) {
    impl_->requireChiptune("play");
    impl_->requireStreamableClock();
    impl_->encodeAtCall(id);
    impl_->cueQueue.push(audio::AudioCommand{
        .op  = audio::AudioCommand::Op::PlayCue,
        .id  = id,
        .cue = audio::VoiceCue{.voice = cue.voice.index(), .mode = cue.mode, .effect = cue.effect}});
    impl_->wakeOrApply();
}

void AudioSystem::SNES::effect(Voice voice, const AudioEffect& effect) {
    impl_->requireChiptune("effect");
    impl_->cueQueue.push(audio::AudioCommand{
        .op  = audio::AudioCommand::Op::EffectVoice,
        .id  = AudioId{},
        .cue = audio::VoiceCue{.voice = voice.index(), .effect = effect}});
    impl_->wakeOrApply();
}

void AudioSystem::SNES::stop(Voice voice) {
    impl_->requireChiptune("stop");
    impl_->cueQueue.push(audio::AudioCommand{
        .op = audio::AudioCommand::Op::StopVoice, .id = AudioId{}, .value = voice.index()});
    impl_->wakeOrApply();
}

void AudioSystem::SNES::echo(const Echo& echo) {
    impl_->requireChiptune("echo");
    impl_->cueQueue.push(
        audio::AudioCommand{.op = audio::AudioCommand::Op::SetEchoPath, .id = AudioId{}, .path = echo});
    impl_->wakeOrApply();
}

void AudioSystem::SNES::echo(const Reverb& reverb) {
    impl_->requireChiptune("echo");
    impl_->cueQueue.push(
        audio::AudioCommand{.op = audio::AudioCommand::Op::SetEchoPath, .id = AudioId{}, .path = reverb});
    impl_->wakeOrApply();
}

std::vector<std::uint8_t> AudioSystem::read(const MemoryRegion& where, std::uint32_t index) {
    impl_->requireAudioUnit("read");
    Impl::UnitRequest request{.where = where, .index = index, .isRead = true};
    impl_->askUnit(request);
    return std::move(request.bytes);
}

void AudioSystem::write(const MemoryRegion& where, std::span<const std::uint8_t> bytes, std::uint32_t index) {
    impl_->requireAudioUnit("write");
    Impl::UnitRequest request{.where = where,
                              .index = index,
                              .isRead = false,
                              .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    impl_->askUnit(request);
}

bool       AudioSystem::isPlaying() const noexcept { return impl_->playing.load(std::memory_order_relaxed); }
AudioStats AudioSystem::audioStats() const noexcept { return impl_->stats(); }

// ── Hosted resident driver (the non-template core + the handle drive) ─────────────────────────────────
// host()'s template half (retropp/audio_system.h) validates the id is a driver and copies the slot layout
// into the handle; this is its non-template half — create the voice + snapshot and wake production.
void AudioSystem::hostResolvedDriver(AudioId driver, std::size_t slotCount) {
    impl_->hostDriver(driver, slotCount);
    impl_->wakeOrApply();
}

// The HostedDriver handle's verbs: marshal a driver op onto the same cue channel play()/stop() use (so
// order is preserved with cued audio) and wake production. All are game-thread, wait-free.
void AudioSystem::driverEnqueuePlay(AudioId driver, AudioType lane, std::uint64_t value) {
    impl_->cueQueue.push(audio::AudioCommand{
        .op = audio::AudioCommand::Op::DriverPlay, .id = driver, .lane = lane, .value = value});
    impl_->wakeOrApply();
}

void AudioSystem::driverEnqueueStop(AudioId driver) {
    impl_->cueQueue.push(audio::AudioCommand{.op = audio::AudioCommand::Op::DriverStop, .id = driver});
    impl_->wakeOrApply();
}

void AudioSystem::driverEnqueueRestart(AudioId driver) {
    impl_->cueQueue.push(
        audio::AudioCommand{.op = audio::AudioCommand::Op::DriverRestart, .id = driver});
    impl_->wakeOrApply();
}

void AudioSystem::driverEnqueueSlotWrite(AudioId driver, std::uint32_t slotIndex, std::uint64_t value) {
    impl_->cueQueue.push(audio::AudioCommand{.op = audio::AudioCommand::Op::DriverSlot,
                                             .id = driver, .value = value, .slotIndex = slotIndex});
    impl_->wakeOrApply();
}

void AudioSystem::driverClose(AudioId driver) {
    impl_->releaseDriverSnapshot(driver);
    impl_->cueQueue.push(audio::AudioCommand{.op = audio::AudioCommand::Op::DriverClose, .id = driver});
    impl_->wakeOrApply();
}

std::vector<std::uint64_t> AudioSystem::driverReadSnapshot(AudioId driver) const {
    return impl_->readDriverSnapshot(driver);
}

std::size_t AudioSystem::driverUnderflowFrames(AudioId driver) const {
    return impl_->readDriverUnderflow(driver);
}

// ── Internal test seam (src/audio/audio_system_testing.h) ────────────────────────────────────────────
// Defined here, where Impl is complete. A friend of AudioSystem, so it reaches the private manual ctor
// and impl_. Drives production synchronously on the calling thread — the deterministic path device-free
// tests use in place of the autonomous production thread.
namespace detail {

void AudioSystemTestAccess::step(AudioSystem& sys) {
    sys.impl_->drainCues();
    if (sys.impl_->playing.load(std::memory_order_relaxed)) {
        sys.impl_->produceOnce();
    }
}

std::size_t AudioSystemTestAccess::voiceCount(const AudioSystem& sys) {
    return sys.impl_->voices.size();
}

void AudioSystemTestAccess::stepVoice(AudioSystem& sys, std::size_t index) {
    sys.impl_->drainCues();
    if (index < sys.impl_->voices.size() && sys.impl_->voices[index]->runner != nullptr) {
        sys.impl_->voices[index]->runner->stepOnce();
    }
}

void AudioSystemTestAccess::mix(AudioSystem& sys, std::size_t wanted) {
    sys.impl_->mixLanes(wanted);
}

std::size_t AudioSystemTestAccess::laneUnderflowFrames(const AudioSystem& sys, std::size_t index) {
    return index < sys.impl_->voices.size() ? sys.impl_->voices[index]->laneUnderflow : 0;
}

std::size_t AudioSystemTestAccess::laneFrames(const AudioSystem& sys, std::size_t index) {
    return index < sys.impl_->voices.size() ? sys.impl_->voices[index]->lane.sizeApprox() : 0;
}

std::size_t AudioSystemTestAccess::laneCapacity(const AudioSystem& sys, std::size_t index) {
    return index < sys.impl_->voices.size() ? sys.impl_->voices[index]->lane.capacity() : 0;
}

std::size_t AudioSystemTestAccess::latencyTarget(const AudioSystem& sys) {
    return sys.impl_->targetFrames;
}

std::size_t AudioSystemTestAccess::waitingFloor(const AudioSystem& sys) {
    return sys.impl_->ringFloor;
}

std::size_t AudioSystemTestAccess::framesPerStep(const AudioSystem& sys) {
    return sys.impl_->framesPerStep;
}

std::uint64_t AudioSystemTestAccess::stepDriverRaw(AudioSystem& sys, std::uint64_t cycles) {
    sys.impl_->drainCues();
    // Chiptune-only seam (the golden gate drives a chiptune driver); a Pcm system has no VM to step.
    // Steps the FIRST voice's VM by exactly `cycles`, then mixes its laned samples into the ring — with
    // one voice (the golden gate's shape) the mix is the identity, so the captured PCM is the driver's
    // bytes unchanged.
    if (sys.impl_->playing.load(std::memory_order_relaxed) && !sys.impl_->voices.empty() &&
        sys.impl_->voices.front()->runner != nullptr) {
        const std::uint64_t ran = sys.impl_->voices.front()->runner->machine().stepDriver(cycles);
        sys.impl_->mixLanes(AudioSystem::Impl::kMixEverything);  // this seam produced the frames itself
        return ran;
    }
    return 0;
}

Vm& AudioSystemTestAccess::voiceMachine(AudioSystem& sys, std::size_t index) {
    sys.impl_->drainCues();
    if (index >= sys.impl_->voices.size() || sys.impl_->voices[index]->runner == nullptr) {
        throw std::out_of_range("AudioSystemTestAccess::voiceMachine: no voice with a machine at that index");
    }
    return sys.impl_->voices[index]->runner->machine();
}

void AudioSystemTestAccess::streamThreshold(AudioSystem& sys, std::size_t bytes) {
    sys.impl_->streamThreshold = bytes;
}

std::uint8_t AudioSystemTestAccess::streamedVoices(AudioSystem& sys) {
    sys.impl_->drainCues();
    const AudioSystem::Impl::Voice* s = sys.impl_->findSamplerVoice();
    if (s == nullptr) {
        return 0;
    }
    std::uint8_t bits = 0;
    for (std::size_t voice = 0; voice < s->streams.size(); ++voice) {
        if (s->streams[voice].has_value()) {
            bits = static_cast<std::uint8_t>(bits | (1u << voice));
        }
    }
    return bits;
}

std::uint16_t AudioSystemTestAccess::cacheLimit(AudioSystem& sys) {
    sys.impl_->drainCues();
    return sys.impl_->sampleCache.limit();
}

std::optional<std::uint16_t> AudioSystemTestAccess::placementOf(AudioSystem& sys, AudioId id) {
    sys.impl_->drainCues();
    return sys.impl_->sampleCache.placementOf(id);
}

std::uint8_t AudioSystemTestAccess::heldVoices(AudioSystem& sys) {
    sys.impl_->drainCues();
    std::uint8_t bits = 0;
    for (std::size_t voice = 0; voice < sys.impl_->heldId.size(); ++voice) {
        if (sys.impl_->heldId[voice].has_value()) {
            bits = static_cast<std::uint8_t>(bits | (1u << voice));
        }
    }
    return bits;
}

bool AudioSystemTestAccess::echoPending(AudioSystem& sys) {
    sys.impl_->drainCues();
    return sys.impl_->echo_.growPending;
}

}  // namespace detail

}  // namespace retropp
