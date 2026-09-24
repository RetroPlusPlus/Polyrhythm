// Internal VM backend seam: the abstract interface every per-system VM backend implements.
//
// This is the boundary that makes the VM host multi-system. The generic Vm (vm.cpp) owns one
// VmBackend chosen by VMPlatform and drives it through this interface; it knows nothing about SM83,
// the Game Boy memory map, or SameBoy. Each system supplies a concrete backend (SameBoyBackend for the
// Game Boy family, SnesBackend for the SNES; a NES / Genesis backend is a drop-in implementation of this
// same interface). All machine idiom for a system lives behind its backend.
//
// This header is INTERNAL — under src/vm/, never include/retropp/. It pulls no backend-library type.
#ifndef RETROPP_SRC_VM_VM_BACKEND_H
#define RETROPP_SRC_VM_VM_BACKEND_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "retropp/driver_binding.h"  // DriverImage / Mapper — the resident-driver image configuration
#include "retropp/raster_content.h"  // RasterPixelFormat — the layout a completed frame arrives in
#include "retropp/guest_watch.h"     // AccessVerdict — what a watched access is answered with
#include "retropp/memory_region.h"   // MemoryRegion — a declared place in the guest's address space
#include "retropp/timing.h"          // CycleDraw — a draw of cycles and the fraction carried
#include "src/vm/assembler.h"        // AssembledRoutine — the assemble() return shape (bytes + labels)
#include "src/vm/wide_math.h"        // mulAddDiv — a clock that is a ratio passes 64 bits

namespace retropp::vm {

// One register preset the generic host applies live before a resident entry call (the folded argument
// plus any fixed presets). Backend-neutral: a register id (the Location register id) and its value.
struct ResidentRegister {
    std::uint16_t registerId;
    std::uint64_t value;
};

// Whose stack a call in the guest's context pushes its return frame on. The generic host answers it
// from who owns the machine's state, never from the console: a guest with a context to preserve keeps
// its own stack, and a machine whose guest has not started yet has none to keep.
enum class CallStack {
    Guest,    // the machine's live stack, exactly where the guest's own call would push
    Scratch,  // the backend's own scratch stack — the machine has no guest context yet
};

// A machine's own clock: its rate as the exact ratio it is, and the cycles one of its frames takes.
// The generic host paces a machine from this and from nothing else.
struct MachineClock {
    std::uint32_t hertzNumerator;  // the clock is hertzNumerator / hertzDivisor cycles a second
    std::uint32_t hertzDivisor;
    std::uint32_t cyclesPerFrame;  // one frame of the machine's own, in those cycles
    [[nodiscard]] constexpr bool operator==(const MachineClock&) const noexcept = default;

    // One frame of the machine's own in whole nanoseconds, the fraction dropped: 16'742'706 for the
    // Game Boy family. Zero for a clock with a zero in it, which is no clock.
    [[nodiscard]] constexpr std::chrono::nanoseconds framePeriod() const noexcept {
        if (hertzNumerator == 0 || hertzDivisor == 0) {
            return std::chrono::nanoseconds::zero();
        }
        return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
            mulAddDiv(static_cast<std::uint64_t>(cyclesPerFrame) * hertzDivisor, 1'000'000'000u, 0,
                      hertzNumerator)
                .quotient)};
    }

    // The cycles a span of wall time is worth to this machine, with the fraction of a cycle left
    // over carried into the next draw so the running total never drifts. Hand back the carry the
    // last draw returned; zero is the right start.
    //
    // A span of exactly one of the machine's own frames is worth exactly cyclesPerFrame, and the
    // carry passes through untouched. The frame count is the hardware fact and the whole-nanosecond
    // period is the rounded one, so deriving the count back out of the period comes up a cycle short
    // every frame. A zero or negative span, or a clock with a zero in it, draws nothing.
    [[nodiscard]] constexpr CycleDraw cyclesFor(std::chrono::nanoseconds span,
                                                std::uint64_t carry) const noexcept {
        if (span.count() <= 0 || hertzNumerator == 0 || hertzDivisor == 0) {
            return CycleDraw{.cycles = 0, .carryNs = carry};
        }
        if (span == framePeriod()) {
            return CycleDraw{.cycles = cyclesPerFrame, .carryNs = carry};
        }
        const WideQuotient draw =
            mulAddDiv(hertzNumerator, static_cast<std::uint64_t>(span.count()), carry,
                      static_cast<std::uint64_t>(hertzDivisor) * 1'000'000'000u);
        return CycleDraw{.cycles = draw.quotient, .carryNs = draw.remainder};
    }
};

// The lifecycle the generic host drives per system. A call is: beginCall(entry) → writeRegister /
// writeMemory (marshal inputs) → run() → readRegister / readMemory (read the output). Registers are
// named by the backend-defined register id carried in a Location (the platform header — gb.h — is
// the id authority for the Game Boy family).
class VmBackend {
public:
    virtual ~VmBackend() = default;

    // Reset the machine to its post-reset state, clearing persistent routine state (e.g. RNG seeds).
    // Placed routines stay placed (their bytes live in the code space, untouched by reset).
    virtual void reset() = 0;

    // Advance the machine's free-running clock by `cycles` CPU cycles without executing a routine, so
    // time-based hardware registers (e.g. the Game Boy's rDIV divider) keep ticking between calls as
    // on always-running hardware. Must not disturb routine-visible state (registers/RAM the next call
    // marshals); only the timing/divider state advances.
    virtual void advanceClock(std::uint64_t cycles) = 0;

    // The machine's own clock, as the machine stands. This is the ONE answer to how fast a machine
    // runs and how long its frame is: the generic host builds its pacing from it and reads a rate
    // from nowhere else. A core whose rate depends on what it hosts answers for what it hosts now.
    //
    // Asked from the game's thread while the machine runs on its own, so a backend answers from
    // what it fixed when the cartridge was hosted and never from the running machine.
    //
    // A core that keeps no time of its own answers nothing, and the generic host refuses to run it.
    [[nodiscard]] virtual std::optional<MachineClock> clock() const { return std::nullopt; }

    // Inject a routine's extracted bytes into the code space and return the absolute entry address of
    // its first byte. Throws (std::runtime_error) if the backend's code arena cannot hold it, or
    // (std::logic_error) if this machine hosts a game's own cartridge — that image has no arena.
    virtual std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) = 0;

    // Load a whole cartridge image the game supplies and reset the machine, so the image's bytes are
    // addressable. The backend parses the image's own header; the engine never reads a ROM byte and
    // exposes no cartridge metadata. This makes the image READABLE, not running — there is no boot,
    // no entry point, no execution.
    //
    // Hosting a game's cartridge and hosting an engine-built one are EXCLUSIVE. The resident-driver
    // path synthesizes a cartridge — it writes its own header and places engine content into the
    // gaps — so the engine owns that image; here the game owns it. Each refuses the other rather
    // than silently overwriting it. Throws std::logic_error if the other mode is configured, and
    // std::invalid_argument for an empty image.
    virtual void loadRom(std::span<const std::uint8_t> rom) = 0;

    // Bring the hosted image to the state the platform's own boot firmware leaves: the boot overlay
    // unmapped so the cartridge answers everywhere it maps, the machine in the mode the image's own
    // header selects, registers and hardware state seeded to the documented firmware-exit values,
    // and the program counter at the cartridge's entry point. The machine is ready to execute the
    // image's own code and is NOT stepped here — running is the caller's act. Each backend seeds its
    // own console's state; no firmware binary is shipped or loaded.
    //
    // Throws std::logic_error unless a game's cartridge is hosted (loadRom first).
    virtual void bootHostedRom() = 0;

    // Assemble routine source written in this backend's assembly language into machine-code bytes
    // (+ exported label offsets), using the backend's own ISA assembler and platform symbol defaults
    // (e.g. the Game Boy backend assembles SM83 and predefines rDIV). The generic host calls this for
    // the source form of registerRoutine, then injects the bytes via placeRoutine exactly as for
    // pre-assembled bytes — so a future console's backend brings its own assembler, no shared change.
    // Throws (std::runtime_error, with line context) on a source error.
    [[nodiscard]] virtual AssembledRoutine assemble(std::string_view source) const = 0;

    // The byte-width of a register id (1, 2, or 4), or 0 if the id is not a register on this system.
    // The generic host uses this to validate a value's width against its bound register.
    [[nodiscard]] virtual int registerWidthBytes(std::uint16_t registerId) const = 0;

    // Whether every byte a region spans is reachable on this machine as it stands. The backend
    // decodes the region's (possibly bank-qualified) base into its own address space and checks the
    // whole extent fits the memory it starts in — a long array crossing bank boundaries is resolved
    // in decoded space, never by arithmetic on the encoded base. The generic host calls this once
    // per entry when a region batch is registered; it never learns the encoding.
    //
    // This is the ONE answer to what memory a machine has. Everything that asks — a region batch, a
    // routine's memory binding, a word read — asks this, so no two callers can be told different
    // things about the same address.
    [[nodiscard]] virtual bool regionIsAddressable(const MemoryRegion& region) const = 0;

    // Whether a single byte at `address` is reachable: the one-byte case of the question above, and
    // implemented as exactly that. The generic host uses it to validate a memory binding.
    [[nodiscard]] bool addressIsAccessible(std::uint32_t address) const {
        return regionIsAddressable(MemoryRegion{.at = address, .size = 1});
    }

    // Copy entry `index` of `region` into `out` (exactly region.size bytes), and the reverse. The
    // backend decodes the region's base into its own address space and strides to the entry THERE —
    // an entry past the first boundary of a banked memory is not at base + index * size, because the
    // encoded base stops describing the run once it leaves the window it names. No caller above the
    // backend does stride arithmetic on an encoded address.
    //
    // Writing a cartridge is allowed: the image is a buffer this process owns, not read-only silicon,
    // and patching a hosted image is a thing a game extending one legitimately does. The write lands
    // in memory only — nothing touches the file the bytes came from, and re-hosting replaces it.
    //
    // Throws std::out_of_range for an index the region does not declare or a region that does not
    // resolve, and std::invalid_argument if `bytes` is not exactly one entry wide.
    virtual void readRegion(const MemoryRegion& region, std::uint32_t index,
                            std::span<std::uint8_t> out) = 0;
    virtual void writeRegion(const MemoryRegion& region, std::uint32_t index,
                             std::span<const std::uint8_t> bytes) = 0;

    // Begin a fresh call frame: set the program counter to `entry`, set up a scratch stack, and
    // arrange for run() to terminate when the routine returns.
    virtual void beginCall(std::uint32_t entry) = 0;

    // Marshal one input into the pending frame (register) or live memory (address). Width in bytes.
    virtual void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) = 0;
    virtual void writeMemory(std::uint32_t address, std::uint64_t value, int width) = 0;

    // Run the current call to its return.
    virtual void run() = 0;

    // Read an output back after run(). Width in bytes (for memory).
    [[nodiscard]] virtual std::uint64_t readRegister(std::uint16_t registerId) = 0;
    [[nodiscard]] virtual std::uint64_t readMemory(std::uint32_t address, int width) = 0;

    // ── Audio chain (the hardware-speed driver path) ──────────────────────────────────────────────
    // The producer-side sink the backend's sound chip forwards each produced PCM frame to. Fires on the
    // thread that steps the machine: the AudioSystem's production thread for a driver it hosts, the
    // machine's own thread or the ticking thread for a hosted cartridge. A backend with no audio model
    // leaves it unused.
    using AudioSampleSink = std::function<void(std::int16_t left, std::int16_t right)>;

    // Enable the backend's APU audio at `sampleRate` Hz and route produced frames to `sink`. The
    // generic host calls this once when a consumer wires up the audio chain. A backend with no audio
    // synthesis throws (the Game Boy and SNES backends realize it). Idempotent.
    virtual void enableAudio(unsigned sampleRate, AudioSampleSink sink) = 0;

    // Position the machine at a continuously-running driver routine's entry (set PC + a scratch stack),
    // with NO return sentinel — unlike beginCall, the driver is not run to a return; it is stepped a
    // cycle budget at a time by runForCycles while its APU writes produce audio.
    virtual void beginContinuous(std::uint32_t entry) = 0;

    // Run the positioned driver for `cpuCycles` CPU cycles (the machine's own cycles — the unit
    // clock() counts in); the APU produces ~rate/frameRate frames into the enabled sink during the
    // run. Returns the CPU cycles actually run (≥ cpuCycles; a partial last instruction overshoots —
    // the caller carries the remainder for drift-free pacing).
    virtual std::uint64_t runForCycles(std::uint64_t cpuCycles) = 0;

    // ── Guest input ───────────────────────────────────────────────────────────────────────────────
    // Whether this core takes button state at all. A core with no input path answers false, and the
    // public verb refuses on the calling thread rather than failing later on the machine's own.
    [[nodiscard]] virtual bool takesButtons() const = 0;

    // Hold exactly these buttons, packed in the machine's own order — the same word the platform's
    // own button vocabulary produced, which the generic host passes through without reading. A level,
    // not an event: the whole set is replaced, so a bit that is clear is released. Called at a step
    // boundary, on the thread that steps the machine.
    virtual void setButtons(std::uint64_t held) = 0;

    // ── Resident driver (the hosted-machine path) ─────────────────────────────────────────────────
    // Configure the machine as a resident-driver host: build a cartridge image sized to hold the
    // highest placed bank, install `mapper` (the backend decodes its opaque id), place each image's
    // bytes at its (possibly bank-qualified) base, then load + reset. `stackTop` relocates the scratch
    // stack (0 = the backend's default scratch top). Preserves any already-placed routine arena. Throws
    // (std::invalid_argument / std::runtime_error) on a banked placement with the none mapper,
    // overlapping placed ranges, placement into the boot-ROM window / engine-reserved header gap, a
    // stack top outside work RAM, or a cartridge the backend cannot address.
    virtual void configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                        std::uint32_t stackTop) = 0;

    // Perform one resident entry call: apply `presets` to the register file live, set PC = entry and SP
    // = the configured resident stack top, plant the return sentinel, and run to the routine's return
    // counting CPU cycles (capped at `maxCpuCycles` — a runaway guard for a driver entry that never
    // returns). Returns the CPU cycles consumed. configureResidentImage must have been called first.
    virtual std::uint64_t callResident(std::uint32_t entry,
                                       std::span<const ResidentRegister> presets,
                                       std::uint64_t maxCpuCycles) = 0;

    // Call a routine the machine already holds WITHOUT disturbing what the guest was doing. The
    // sibling of callResident and beginCall/run: those establish a frame of the engine's own — the
    // whole register file replaced, the program counter and stack seated at engine-chosen values —
    // and so cannot run while a guest frame is live. This one preserves the frame it found.
    //
    //   entry            where the routine begins, in the machine's own vocabulary (bank-qualified
    //                    where the console needs it). A bank-qualified entry is callable only while
    //                    its own bank is the live one; the backend throws std::logic_error naming
    //                    both banks otherwise, because switching the mapper on the guest's behalf is
    //                    the guest's own act, not the engine's.
    //   presets          the register inputs, applied over the guest's live register file. Memory
    //                    inputs are written before the call through writeMemory, as for any call.
    //   stack            whose stack the return frame is pushed on (CallStack above). The push goes
    //                    through the machine's own bus, so every mapping the guest selected holds,
    //                    and it costs the guest no cycles.
    //   maxInstructions  the runaway guard, in instructions — a routine that never returns must
    //                    still end the call. On overrun the routine is abandoned where it is and the
    //                    register file is restored; the memory it changed stays changed.
    //   readOutputs      run after the routine returns and BEFORE the register file is restored, so
    //                    the host layer reads the bound output through readRegister / readMemory
    //                    exactly as it does after run().
    //
    // Returns nothing. The cycles the routine spent are the guest's own, and they are counted where
    // the machine was already being run from.
    //
    // The whole register file is restored afterwards, so the interrupted instruction stream resumes
    // as if nothing had happened; what the routine changed in memory stands, because that is the
    // routine's own work. A call made from inside a call is the same call, at any depth.
    virtual void callInContext(std::uint32_t entry, std::span<const ResidentRegister> presets,
                               CallStack stack, std::size_t maxInstructions,
                               const std::function<void()>& readOutputs) = 0;

    // ── Escapes (guest code hands control to native code) ─────────────────────────────────────────
    // The backend's whole part is DETECT AND REPORT: it watches the addresses the host layer arms and
    // calls the sink when one is about to execute. The address→handler table, its keys and its
    // validation live in the generic host — a backend never learns what a game does at an escape.

    // The sink the backend calls when an armed address is about to execute, passing the address as it
    // was armed (the encoded form the host layer handed down, so the host matches it back to its
    // declaration without decoding anything). Installed once by the generic host, in the same posture
    // as AudioSampleSink: it fires on the thread that steps the machine.
    using EscapeSink = std::function<void(std::uint32_t firedAt)>;

    // Install the sink. Idempotent; an empty sink detaches.
    virtual void setEscapeSink(EscapeSink sink) = 0;

    // Begin and stop watching one address. `address` is in the machine's own vocabulary, bank-qualified
    // where the console needs it — the backend decodes it, and an address in a banked window fires only
    // while that bank is live. The generic host arms only addresses that already answered
    // regionIsAddressable, so these do not re-validate; arming an address twice is idempotent and
    // disarming one that is not armed does nothing (the host layer refuses both before reaching here).
    //
    // `replacesRoutine` declares the answering kind: the backend puts its OWN ISA's return instruction
    // at the address while it is armed — keeping what was there — and restores it on disarm, so the
    // one instruction that executes after the sink returns is the return itself and the routine's body
    // never runs. Which instruction that is, and how wide, is the backend's business alone.
    //
    // Whether a per-instruction hook exists at all is the backend's business too: a backend with none
    // throws from armEscape rather than accepting an escape that could never fire.
    virtual void armEscape(std::uint32_t address, bool replacesRoutine) = 0;
    virtual void disarmEscape(std::uint32_t address) = 0;

    // Write one register of the PARKED machine's live register file, for the escape marshalling path.
    // Distinct from writeRegister, which marshals into the pending frame a beginCall applies: an
    // escape fires mid-run with no pending frame, and what it establishes must be in the register the
    // guest's own next instruction reads. Only meaningful on the thread stepping the machine, while it
    // is parked at an escape.
    virtual void writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int width) = 0;

    // ── Watches (the guest's own accesses, decided by native code) ────────────────────────────────
    // The backend's part is DETECT AND ASK: it watches the places the host layer arms and asks the
    // sink what each access does, then realizes the answer in whatever way its own core allows. The
    // place→handler table, its keys and its validation live in the generic host — a backend never
    // learns what a game does at a watch.

    // Which direction a watched access went. A backend arms each direction independently, because a
    // game that only wants writes must not make the machine pay for reads.
    enum class AccessKind : std::uint8_t { Read, Write };

    // The sink the backend asks when a watched byte is accessed, and whose answer it realizes.
    //
    //   armedBase  the encoded base the host layer armed, handed back verbatim so the host matches
    //              the access to its declaration without decoding anything.
    //   at         the encoded address of the byte actually accessed — the base for a one-byte
    //              place, and the byte's own address within a span.
    //   kind       which direction the access went.
    //   value      the byte the access carries: what the machine would have answered on a read, and
    //              what the guest is storing on a write.
    //
    // Returns what the access does. On a read the backend answers the guest with the substituted
    // byte for instead(v) and the machine's own byte otherwise (a read cannot be prevented). On a
    // write it prevents the store for veto(), and for instead(v) prevents the guest's store and
    // performs its own — which is the engine's own mechanism store and must not be watched, or the
    // handler re-enters itself without end.
    //
    // Installed once by the generic host, in the same posture as EscapeSink: it fires on the thread
    // that steps the machine, synchronously, with the machine parked at the access.
    using WatchSink = std::function<AccessVerdict(std::uint32_t armedBase, std::uint32_t at,
                                                  AccessKind kind, std::uint8_t value)>;

    // Install the sink. Idempotent; an empty sink detaches.
    virtual void setWatchSink(WatchSink sink) = 0;

    // Begin and stop watching every byte a place spans, in the directions named. `where` is in the
    // machine's own vocabulary, bank-qualified where the console needs it — the backend decodes it,
    // and a place in a banked window is watched only while that bank is live. The generic host arms
    // only places that already answered regionIsAddressable, so these do not re-validate the extent;
    // arming a place twice is idempotent, and disarming one that is not armed does nothing.
    //
    // Whether a per-access hook exists at all is the backend's business: a backend with none throws
    // from armWatch rather than accepting a watch that could never fire. A backend whose hook sees a
    // narrower address than the place spans (a CPU-width hook over a banked window) throws for a
    // place its hook cannot cover, naming what it could not reach.
    virtual void armWatch(const MemoryRegion& where, bool onRead, bool onWrite) = 0;
    virtual void disarmWatch(const MemoryRegion& where, bool onRead, bool onWrite) = 0;

    // ── Picture (the frames the machine finishes drawing) ─────────────────────────────────────────
    // The backend's part is SIGNAL AND HAND OVER: it calls the sink when the machine FINISHES a frame,
    // handing over that frame's pixels. Nothing polls a machine for its picture — a cycle budget can
    // end part-way through a raster, and a half-drawn frame reads exactly like a finished one.
    //
    // What a backend owes here is a completion, a buffer, dimensions and a pixel layout. It is never
    // asked how often it draws or which frame this is: a core that draws 256×224, or that cannot hold
    // a steady rate at all, is implementable behind this seam precisely because neither question is
    // asked of it.

    // The sink the backend calls when a frame completes. `pixels` is row-major and
    // width * height * bytesPerPixel(format) bytes long, valid for the duration of the call. Installed
    // once by the generic host, in the same posture as AudioSampleSink: it fires on the thread that
    // steps the machine.
    using FrameSink = std::function<void(std::span<const std::uint8_t> pixels, int width, int height,
                                         RasterPixelFormat format)>;

    // Install the sink. Idempotent; an empty sink detaches.
    virtual void setFrameSink(FrameSink sink) = 0;

    // Turn pixel output on and off. A machine hosts WITHOUT drawing by default — a raster costs cycles
    // that a co-execution path which never displays should not pay. A backend whose core produces no
    // picture throws rather than accepting one that could never arrive, the same posture armEscape and
    // armWatch take. Idempotent in both directions.
    virtual void setVideoEnabled(bool enabled) = 0;

    // ── Save data (what the machine keeps when the power goes off) ─────────────────────────────────
    // The backend's part is HOLD AND HAND OVER: it says how much persistent data the machine has right
    // now, copies it out, takes a stored copy back, and reports whether the guest has changed it. What
    // the bytes MEAN is the backend's alone — the generic host never reads one, so the console's whole
    // vocabulary for its own persistence (which memory, which chip, whether a clock rides along) stays
    // below this line.
    //
    // Two questions, not one. Whether the CORE has a persistence model at all is fixed for a backend;
    // how many bytes THIS image keeps depends on the image and is answered per machine. A core with no
    // model answers false and nothing else is ever asked of it — the posture takesButtons() takes.

    // Whether this core keeps anything across a power cycle. A backend with no persistence model
    // answers false, and the public surface refuses a keyed machine on the calling thread rather than
    // failing later on the machine's own.
    [[nodiscard]] virtual bool keepsSaveData() const = 0;

    // How many bytes the hosted image keeps, as the machine stands. Zero is an ordinary answer — an
    // image that keeps nothing (no battery, no save memory) is the common case for a core that has the
    // model — and it means there is nothing to write and nothing to read back.
    [[nodiscard]] virtual std::size_t saveDataSize() const = 0;

    // Copy out what the machine keeps: exactly saveDataSize() bytes, or empty when that is zero. Called
    // on the thread stepping the machine, like every other verb here.
    [[nodiscard]] virtual std::vector<std::uint8_t> readSaveData() = 0;

    // Take a stored copy back into the machine. A buffer that is not the size this image keeps is
    // refused (std::invalid_argument) rather than truncated or padded: a save that does not fit the
    // cartridge it was handed to is a save for a different cartridge, and guessing which bytes to drop
    // would corrupt the player's data silently.
    virtual void writeSaveData(std::span<const std::uint8_t> bytes) = 0;

    // What a file of this core's persistent data is called, without the dot ("sav" for the Game Boy
    // family). The bytes are the console's own format, so what a file of them is NAMED is the console's
    // answer too — a player's save is then the file every other program that knows that console already
    // reads, and the generic host never invents a name for a format it cannot read. Empty for a core
    // that keeps nothing, whose files never exist.
    [[nodiscard]] virtual std::string_view saveDataExtension() const = 0;

    // Whether the guest has changed what it keeps since this was last asked. ASKING CLEARS IT, so the
    // signal cannot be read twice and cannot be left uncleared — the host polls it once per step and a
    // separate clear verb would be one more thing to forget. A backend whose core cannot tell answers
    // true and lets the host's own pacing decide how often that costs a write.
    [[nodiscard]] virtual bool takeSaveDataChanged() = 0;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_VM_BACKEND_H
