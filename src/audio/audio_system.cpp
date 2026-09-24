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
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // resolveAssetPolicy
#include "retropp/asset_registry.h"    // assetRoot — the single project-relative resource root
#include "retropp/audio_library.h"     // the single catalog play() reads entries from
#include "retropp/audio_mixer.h"       // AudioMixer::instance().effectiveGain — the per-voice output scale
#include "retropp/routine_registry.h"  // detail::findEmbeddedRoutine
#include "retropp/sdl_platform.h"      // SdlAudioSink — the auto-owned production sink (ctor 3)
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous test seam
#include "src/audio/auto_close.h"      // detail::shouldAutoStop — the one-shot-SFX lifecycle decision
#include "src/audio/cue_queue.h"       // audio::AudioCommand / CueQueue — the main→production channel
#include "src/audio/pcm_decode.h"      // detail::g_pcmDecode — the Pcm decode hook (installed by the no-ISA registerAudio)
#include "src/audio/produce_step.h"    // detail::mixFrames / rampFrame — the pure mixdown + release fade
#include "src/audio/ring_buffer.h"
#include "src/vm/vm_core_access.h"   // vm::VmCoreAccess — builds a voice's machine on the core the game named
#include "src/vm/vm_runner.h"        // vm::VmRunner — the machine a voice steps through

namespace retropp {

namespace {
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
    };

    // `ownedSink` is null on the borrow path and holds the sink on the owning path; it is declared
    // FIRST so it is constructed before `sink` is bound to it, and destroyed LAST (after the ring +
    // voices are torn down — safe because the dtor stops the sink AND joins the production thread before
    // any of this runs). The active sink is always reached through `sink`, so the wiring below is
    // identical on both paths.
    std::unique_ptr<AudioSink>        ownedSink;
    AudioSink&                        sink;
    unsigned                          sampleRate;
    AudioKind                         kind_;          // the system's fixed backend — Chiptune or Pcm
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
          clock_(core != nullptr ? vm::VmCoreAccess::clockOf(core, platform) : std::nullopt),
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
          clock_(core != nullptr ? vm::VmCoreAccess::clockOf(core, platform) : std::nullopt),
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
    }

    // ── Cue application (production thread, or inline in manual mode) ─────────────────────────────────
    // Drain every queued cue and apply it. Host-inbox first (so a host()-then-play() sequence finds its
    // voice already built), then the SPSC command queue. SPSC: only the production thread (or, in manual
    // mode, the single calling thread) pops here.
    void drainCues() {
        drainHostInbox();
        std::array<audio::AudioCommand, 1> one;
        while (cueQueue.pop(std::span<audio::AudioCommand>(one)) == 1) {
            applyCommand(one[0]);
        }
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
                // its mid-game driver RAM. `playing` clears when the last tail finishes, in the produce
                // pass. Registered audio stays registered — a later Play cues it afresh.
                for (const std::unique_ptr<Voice>& v : voices) {
                    if (!v->resident) {
                        beginRelease(*v);
                    }
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
        return kind_ == AudioKind::Chiptune ? targetFrames + 2 * framesPerStep : 0;
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
    // once (inside hostDriver), then enable the APU into the voice's lane. enableAudio runs AFTER
    // hostDriver because hostDriver resets the machine — the sink + rate are set once the reset is
    // behind us. The voice rides the VMDriver bus (its `type`).
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

            machine->enableAudio(sampleRate, [vp](std::int16_t left, std::int16_t right) {
                // Scale by the VMDriver bus (a straight amplifier over the driver's own internal
                // mixing). At the default unity gain this is the exact identity, so the laned frame is
                // the driver's own.
                const std::uint32_t gain = AudioMixer::instance().effectiveGain(vp->type);
                vp->lane.push(AudioFrame{applyGain(left, gain), applyGain(right, gain)});
            });
        });
        // Each step publishes what that step left in the declared slots, so the game-thread handle
        // always reads a coherent set from the most recent tick. The hook holds the MACHINE, not the
        // voice's pointer to its runner: closing a voice releases that pointer before it waits for the
        // thread, so a hook that went through it would find nothing there for exactly as long as the
        // wait lasts. A runner's machine keeps its address for the runner's whole life, and the runner
        // does not release it until its thread has left.
        v.runner->afterEachStep([this, vp, machine] { publishSnapshot(*vp, *machine); });
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
            voice->runner->beforeFirstStep([this, vp, machine, bytes = std::move(bytes)] {
                machine->enableAudio(sampleRate, [vp](std::int16_t left, std::int16_t right) {
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
                });
                vp->driver = machine->uploadRoutine<void()>(bytes, kChiptuneBinding);
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

    // Decode a Pcm entry's audio file into stereo frames at this system's sample rate. Embed → the build
    // baked the container bytes into the asset registry (keyed by the logical path); otherwise the file
    // ships beside the binary and is read from assetRoot(). PCM's per-type default is LoadFromPath.
    std::vector<AudioFrame> decodePcmEntry(const AudioLibrary::Entry& entry) {
        // Decode through the hook the audio-file registration installs, never by naming the decoder
        // directly — that is what keeps the decoder out of binaries that register no audio file. A Pcm entry
        // only exists because that registration ran, so the hook is always set here; the guard is defensive.
        if (detail::g_pcmDecode == nullptr) {
            throw std::runtime_error(
                "AudioSystem::play: PCM decoder is not linked — no audio file was registered");
        }
        if (resolveAssetPolicy(entry.policy, AssetPolicy::LoadFromPath) == AssetPolicy::Embed) {
            if (const std::span<const std::uint8_t> baked = detail::findEmbeddedAsset(entry.asmPath);
                !baked.empty()) {
                return detail::g_pcmDecode(baked, sampleRate);
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
        const std::vector<std::uint8_t> bytes(contents.begin(), contents.end());
        return detail::g_pcmDecode(bytes, sampleRate);
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
    : impl_(std::make_unique<Impl>(core, kind, sink, platform, timing, sampleRate)) {}

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
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Play, id, mode});
    impl_->wakeOrApply();
}

void AudioSystem::stop() {
    impl_->cueQueue.push(audio::AudioCommand{audio::AudioCommand::Op::Stop, AudioId{}});
    impl_->wakeOrApply();
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

}  // namespace detail

}  // namespace retropp
