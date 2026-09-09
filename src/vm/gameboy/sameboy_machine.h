// Internal VM backend: a thin wrapper over SameBoy's surgical toolkit.
//
// This header is INTERNAL — it lives under src/vm/, never include/retropp/, and is
// not part of the engine's public surface. The public VM-host API (VMPlatform,
// registerRoutine → typed callable with developer-declared I/O bindings) sits on
// top of this backend; nothing here is exposed to a game.
//
// The wrapper is pimpl'd so this header pulls no SameBoy (GB_*) type: only
// sameboy_machine.cpp includes gb.h. That keeps GB_* out of any surface a consumer
// compiles against, even this internal header, and lets the test include it without
// SameBoy on its include path.
//
// The operation surface is backend-neutral IN SHAPE so the generic host can lift it
// behind the VMPlatform abstraction: construct-with-model, load a ROM image,
// reset, mutable register and memory access, and run-to-return (step the CPU
// until PC reaches a declared return address).
#ifndef RETROPP_SRC_VM_SAMEBOY_MACHINE_H
#define RETROPP_SRC_VM_SAMEBOY_MACHINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace retropp::vm {

// The console the backend instantiates. SameBoy is the GameBoy / GameBoyColor
// backend (the only one in v1); the wider VMPlatform selector lives in vm.h.
enum class ConsoleModel {
    GameBoy,       // → GB_MODEL_DMG_B
    GameBoyColor,  // → GB_MODEL_CGB_E
};

// A backend-neutral mirror of the SM83 register file. 16-bit pairs match
// SameBoy's GB_registers_t; the high byte of each pair is the first-named 8-bit
// register (e.g. A is af >> 8). The VM host's I/O bindings name registers here.
struct Registers {
    std::uint16_t af = 0;
    std::uint16_t bc = 0;
    std::uint16_t de = 0;
    std::uint16_t hl = 0;
    std::uint16_t sp = 0;
    std::uint16_t pc = 0;
};

// Which directly-accessible hardware memory to hand back — a selector, not an address range. A subset
// of SameBoy's GB_direct_access_t, covering the memories a routine reads and writes for I/O.
//
// Console-qualified on purpose: this names the Game Boy family's memories, so a second console's
// backend brings its own selector and the two never collide. It is also deliberately NOT called
// MemoryRegion — that name belongs to the public address-range value type.
enum class GbHardwareMemory {
    Rom,
    VRam,
    WorkRam,
    Oam,
    Hram,
    Io,
};

class SameBoyMachine {
public:
    explicit SameBoyMachine(ConsoleModel model);
    ~SameBoyMachine();

    // Non-copyable (owns a SameBoy instance); movable.
    SameBoyMachine(const SameBoyMachine&) = delete;
    SameBoyMachine& operator=(const SameBoyMachine&) = delete;
    SameBoyMachine(SameBoyMachine&&) noexcept;
    SameBoyMachine& operator=(SameBoyMachine&&) noexcept;

    ConsoleModel model() const;

    // Load a ROM image in situ (the routine runs at its address in this image).
    // SameBoy pads short buffers internally; a synthetic blob is fine for tests.
    void loadRom(std::span<const std::uint8_t> rom);

    // Reset to the model's post-reset state.
    void reset();

    Registers registers() const;
    void setRegisters(const Registers& regs);

    // A mutable view of a hardware memory region (live SameBoy storage — writes
    // land in the running machine). Empty if the region is unavailable.
    std::span<std::uint8_t> memory(GbHardwareMemory region);

    // Write one byte through the machine's own bus — the CPU's view, with every mapping and
    // register side effect the hardware gives a store. Distinct from memory(): a direct-access
    // write lands in raw storage, while a bus write to a control register (the boot-overlay bank
    // latch, the CGB mode latch) performs what the register does. This is how boot seeding
    // reproduces the writes the boot firmware itself performs.
    void busWrite(std::uint16_t address, std::uint8_t value);

    // Step the CPU from its current PC until PC reaches returnAddress, or until
    // maxInstructions have executed (a runaway guard — a routine that never
    // returns must still terminate the call). Returns the number of instructions
    // executed. The instruction at returnAddress is not relied upon to do
    // anything — the call stops once control reaches it.
    std::size_t runToReturn(std::uint16_t returnAddress,
                            std::size_t maxInstructions = 1'000'000);

    // Step the CPU from its current PC until control reaches `landing`, WITHOUT disturbing the run
    // this one is nested inside. Where runToReturn drives the machine a whole step at a time and
    // stops once its flag is set, this one drives the CPU itself and leaves from inside the
    // per-instruction hook the moment the landing is fetched — so `landing` is an address rather
    // than code, the byte there stays the guest's, and the interrupted instruction finds its own
    // fetch exactly as it left it. The cycles spent land in whatever run encloses this one, which is
    // where a guest's own time belongs; this call reports none.
    //
    // Returns the instructions executed, which equals `maxInstructions` when the routine ran away and
    // was abandoned. Restoring the register file is the caller's act, not this one's.
    std::size_t runInContext(std::uint16_t landing, std::size_t maxInstructions = 1'000'000);

    // Like runToReturn, but capped and reported in CYCLES rather than instructions: step the CPU from
    // its current PC until PC reaches returnAddress, or until `maxTicks8MHz` SameBoy cycles (8 MHz
    // units) have elapsed (a runaway guard). Returns the 8 MHz ticks actually run. Used for a resident
    // driver's per-frame entry calls, which must be accounted in cycles to pad the frame to the
    // hardware budget. Composes the execution callback (for the return address) with GB_run's tick
    // returns (for the cycle count).
    std::uint64_t runToReturnCycles(std::uint16_t returnAddress, std::uint64_t maxTicks8MHz);

    // ── Audio ─────────────────────────────────────────────────────────────────
    // A produced stereo PCM sample, neutral of the backend's sample type (the GB_*
    // type stays in the .cpp). One per APU output sample once audio is enabled.
    using SampleSink = std::function<void(std::int16_t left, std::int16_t right)>;

    // Enable APU audio output: set the APU's sample rate to `sampleRate` Hz (so it
    // resamples to the sink rate internally), select the hardware-accurate highpass
    // filter, and install the per-sample callback. Idempotent. After this, running
    // the CPU (runForCycles) produces samples into the installed sink.
    void enableAudio(unsigned sampleRate);

    // Install the sink the APU sample callback forwards each produced frame to. The
    // callback fires on the thread that runs the CPU (for the audio chain, the
    // AudioSystem's production thread) — so the sink is the producer side of the
    // audio ring buffer. Pass an empty sink to detach.
    void setSampleSink(SampleSink sink);

    // Run the CPU continuously for ~`ticks8MHz` SameBoy cycles (8 MHz units — twice
    // the 4 MHz T-cycles, since GB_run reports in 8 MHz ticks), WITHOUT a return
    // sentinel — the hosted routine is a continuously-running driver, not a call.
    // The APU produces samples throughout; any installed SampleSink receives them.
    // Returns the actual 8 MHz ticks run (≥ ticks8MHz; a partial last instruction
    // overshoots slightly — the caller carries the remainder for drift-free pacing).
    std::uint64_t runForCycles(std::uint64_t ticks8MHz);

    // ── Escapes ───────────────────────────────────────────────────────────────
    // One per-instruction hook serves both the run-to-return watch and the watched
    // address set. It is installed whenever either needs it and absent otherwise,
    // so a machine with nothing watched runs with no per-instruction cost at all.

    // Called when a watched address is about to execute, passing that address. The
    // instruction executes once the sink returns, whatever the sink did. Fires on
    // the thread running the CPU. Pass an empty sink to detach.
    using EscapeSink = std::function<void(std::uint16_t address)>;
    void setEscapeSink(EscapeSink sink);

    // Begin and stop watching one address. Both are idempotent: watching an address
    // already watched, or stopping one that is not, changes nothing.
    void armEscape(std::uint16_t address);
    void disarmEscape(std::uint16_t address);

    // How many addresses are watched — what decides whether the hook is installed.
    [[nodiscard]] std::size_t armedEscapeCount() const;

    // Whether the per-instruction hook is installed right now. A machine watching nothing, and not
    // running to a return, carries no hook at all — this is what makes that observable rather than
    // asserted.
    [[nodiscard]] bool hookInstalled() const;

    // ── Watches ───────────────────────────────────────────────────────────────
    // Two per-access hooks, one per direction, each installed only while at least
    // one address is watched in that direction — so a machine watching nothing
    // pays nothing at all, and a machine watching writes pays nothing on reads.
    //
    // The watched set is a BITMAP over the whole 16-bit space (8 KiB per
    // direction, allocated on first use). Once one address is watched the hook
    // runs on every access the CPU makes, which is the hottest path this machine
    // has, so the is-this-watched test is one load and a mask rather than a
    // search.
    //
    // The address is the CPU's own 16-bit view. Whether a watched address in the
    // switchable window means the bank the watcher meant is decided above this
    // layer, against mappedRomBank().

    // The byte the guest sees, answering a watched read. Fires on the thread
    // running the CPU, with the machine parked mid-instruction. A read cannot be
    // prevented — the instruction has committed to reading something — so the
    // sink answers with a byte and nothing else.
    using ReadAccessSink = std::function<std::uint8_t(std::uint16_t address, std::uint8_t data)>;

    // What a watched write does, in this machine's own terms. SameBoy's write
    // hook can let a store happen or prevent it and CANNOT change its value, so
    // substituting is composed here: prevent the guest's store, then perform our
    // own through the same bus so every mapping and register side effect holds.
    // That store is the engine's own and is not itself watched, or the sink would
    // re-enter without end.
    enum class WriteAction : std::uint8_t {
        Allow,       // the guest's store lands as it was
        Prevent,     // the store never happens
        Substitute,  // the store never happens; `substitute` is stored in its place
    };
    using WriteAccessSink = std::function<WriteAction(std::uint16_t address, std::uint8_t data,
                                                      std::uint8_t& substitute)>;

    // Install the sinks. Either may be empty, which detaches that direction.
    void setAccessSinks(ReadAccessSink onRead, WriteAccessSink onWrite);

    // Begin and stop watching one address in one direction. All four are
    // idempotent. The bitmap is a SET: an address covered by two declarations is
    // watched once, so the layer above decides when the last of them is gone.
    void armReadWatch(std::uint16_t address);
    void disarmReadWatch(std::uint16_t address);
    void armWriteWatch(std::uint16_t address);
    void disarmWriteWatch(std::uint16_t address);

    // Whether each per-access hook is installed right now. A machine watching
    // nothing carries neither — this is what makes that observable rather than
    // asserted.
    [[nodiscard]] bool readWatchInstalled() const;
    [[nodiscard]] bool writeWatchInstalled() const;

    // The cartridge bank currently mapped into the switchable window. An address in
    // that window names a different byte per bank, so a watcher that cares which
    // bank it meant compares against this when the address is reached.
    [[nodiscard]] std::uint16_t mappedRomBank() const;

    // ── Picture ───────────────────────────────────────────────────────────────
    // The machine draws nothing until video output is enabled: pixel output is off
    // from construction, because running the CPU for extended periods with a PPU that
    // draws costs cycles no headless path should pay.

    // Called when the PPU FINISHES a frame, with that frame's pixels: row-major RGBA
    // bytes, width * height * 4 long, valid for the duration of the call. Fires on the
    // thread running the CPU. Pass an empty sink to detach.
    using FrameSink = std::function<void(std::span<const std::uint8_t> pixels,
                                         int width, int height)>;
    void setFrameSink(FrameSink sink);

    // Turn drawing on and off. Enabling sizes the machine's own pixel buffer to the
    // screen, points the PPU at it, fixes the colour encoding to RGBA bytes, and reports
    // each finished frame to the installed sink; disabling suppresses pixel output again.
    // Idempotent in both directions. A frame the hardware would not have redrawn is not
    // reported — the screen keeps what it is already showing.
    void setVideoEnabled(bool enabled);

    // Whether video output is on. What decides it is one call to videoEnabled, so
    // this is what makes a machine's headlessness observable rather than asserted.
    [[nodiscard]] bool videoEnabled() const;

    // Defined in sameboy_machine.cpp. Public only so the backend's TU-local
    // execution callback can name it (it receives the instance via SameBoy's
    // user-data pointer); an incomplete type here, it exposes nothing usable.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SAMEBOY_MACHINE_H
