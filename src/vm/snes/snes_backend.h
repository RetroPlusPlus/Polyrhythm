// Internal SNES / 65816 VM backend — a VmBackend over Snaggletooth's Snes machine.
//
// This is the ONE place SNES machine idiom lives: the two-port controller word, the region read from a
// cartridge's own header, the frame/save observers the machine reports through, the sound chip's
// 32'000 Hz frames on their way to a sink at the sink's rate, the 65816 register file behind the
// register ids snes.h fixes, and the image the core writes for the routines it places. The generic Vm
// (vm.cpp) drives it only through VmBackend, so it knows none of it. It names places: a place is a byte
// of work RAM, the cartridge image or its save named by any bus address that reaches it, a register
// read as it stands, or a byte of one of the four memories the bus cannot name, named through the space
// snes.h folds into the top byte (snes_address.h decodes both); every region verb reads and writes
// wherever a place resolves, and a register takes no write. It runs
// routines: bytes placed into an image of its own (snes_image.h), each at the address its source named
// or wherever the arena has room, called in a frame of the engine's own; and a routine the cartridge
// already holds, called in the guest's own context on the guest's own stack. It answers escapes and
// watches through the machine's own instruction and access watchers: a place is the byte an address
// reaches, so an escape or a watch armed through one alias fires through every other, and a replaced
// routine is answered by a return the machine stands in for its first fetch, the image left as it is. It
// hosts a resident driver: the driver's images placed in that same image of its own, under the LoROM or
// HiROM map the driver's mapper names, its entries called in the engine's frame on the driver's stack, and
// the machine idling on the image's own loop between them while the sound chip plays.
//
// INTERNAL — under src/vm/, never include/retropp/. It pulls Snaggletooth's public snes.h (the Snes
// machine and the SnesState the tests observe); no snaggletooth:: type reaches include/retropp/.
#ifndef RETROPP_SRC_VM_SNES_SNES_BACKEND_H
#define RETROPP_SRC_VM_SNES_SNES_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "snaggletooth/snes/snes.h"
#include "src/vm/vm_backend.h"
#include "src/vm/snes/resampler.h"
#include "src/vm/snes/snes_image.h"

namespace retropp::vm {

class SnesBackend final : public VmBackend,
                          private snaggletooth::FrameObserver,
                          private snaggletooth::SaveObserver,
                          private snaggletooth::InstructionWatcher,
                          private snaggletooth::AccessWatcher {
public:
    SnesBackend() = default;

    // A hosted cartridge returns to power-on with its save kept; a routine machine is rebuilt on the
    // image it wrote, every placed routine intact.
    void reset() override;
    // On a machine holding the image this core writes — for its routines or a driver — park the CPU on
    // the image's idle loop, run `cycles`, and put the register file back, so the machine's own time
    // passes between calls, the sound chip plays through it, and nothing a call marshals moves. A
    // machine hosting a game's cartridge throws — its image has no spot the engine owns, and running it
    // would run the game.
    void advanceClock(std::uint64_t cycles) override;
    // The machine's own clock, from the region of the cartridge it hosts (NTSC before it hosts one), and
    // nothing else — asked from the game's thread while the machine runs, so it never touches the running
    // machine. 236'250'000 / 11 Hz and 357'366 master cycles a frame for a 60 Hz cartridge; 21'281'370 Hz
    // and 425'568 for a 50 Hz one.
    [[nodiscard]] std::optional<MachineClock> clock() const override {
        const snaggletooth::ConsoleClock c = snaggletooth::consoleClock(region_);
        return MachineClock{.hertzNumerator = static_cast<std::uint32_t>(c.hertzNumerator),
                            .hertzDivisor   = static_cast<std::uint32_t>(c.hertzDenominator),
                            .cyclesPerFrame = static_cast<std::uint32_t>(c.masterCyclesPerFrame)};
    }
    // Place a routine into the image this core writes — at `origin` when the source said one, else at
    // the first gap from $00:8000 up that holds it — and answer the bus address of its first byte. The
    // image is built at the first placement and grows to hold a routine in a later bank. Refuses an origin
    // no image address is, one overlapping a placed routine, the idle loop or the header, and any
    // placement on a machine hosting a game's cartridge.
    std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes,
                               std::optional<std::uint32_t> origin) override;
    void loadRom(std::span<const std::uint8_t> rom) override;
    void bootHostedRom() override;
    // 65816 source through Snaggletooth's assembler, which is absolute: a source that says its address
    // with ORG is assembled there, and one that does not is assembled at the first gap of the arena that
    // holds it, so its labels resolve for where it lands. The bytes are laid from the first range's start
    // to the last range's end, `origin` is that start, and a label is its offset within the bytes. A
    // source error throws std::runtime_error naming the line.
    [[nodiscard]] AssembledRoutine assemble(std::string_view source) const override;
    // 1 for the five 8-bit registers, 2 for the six 16-bit ones, in snes::Reg's order; 0 past PC.
    [[nodiscard]] int registerWidthBytes(std::uint16_t registerId) const override;
    [[nodiscard]] bool regionIsAddressable(const MemoryRegion& region) const override;
    void readRegion(const MemoryRegion& region, std::uint32_t index,
                    std::span<std::uint8_t> out) override;
    void writeRegion(const MemoryRegion& region, std::uint32_t index,
                     std::span<const std::uint8_t> bytes) override;

    // A call in a frame of the engine's own: native mode, 8-bit accumulator and index registers, direct
    // page $0000, data bank $00, the stack at $1FFF, every other register zero, then the marshaled inputs
    // over it. run() seats the frame, calls the entry on that stack with the idle loop as the landing,
    // and ends when the routine's RTS reaches it; the register file it left is what readRegister answers.
    void beginCall(std::uint32_t entry) override;
    void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) override;
    void writeMemory(std::uint32_t address, std::uint64_t value, int width) override;
    void run() override;
    [[nodiscard]] std::uint64_t readRegister(std::uint16_t registerId) override;
    [[nodiscard]] std::uint64_t readMemory(std::uint32_t address, int width) override;

    // Sound: the cartridge's own, from the machine's DSP at 32'000 Hz, converted to `sampleRate` on its
    // way to `sink` (resampler.h). Each frame reaches the sink from inside runForCycles, on the thread
    // that steps the machine. A second call replaces both; a zero rate throws std::invalid_argument.
    void enableAudio(unsigned sampleRate, AudioSampleSink sink) override;
    // The engine's frame with the program counter at `entry` and nothing pushed — on the driver's stack
    // when one is hosted — for runForCycles to run from.
    void beginContinuous(std::uint32_t entry) override;
    // Runs `cpuCycles` master cycles a quarter of a frame at a time, draining the DSP after each slice:
    // its frames reach the sink four times a frame, and a machine with no sink drops them as it goes.
    std::uint64_t runForCycles(std::uint64_t cpuCycles) override;

    // Guest input: the SNES pad. The public two-port word (snes.h) packs into the seam's opaque uint64.
    [[nodiscard]] bool takesButtons() const override { return true; }
    void setButtons(std::uint64_t held) override;

    // Host a driver: each image at its bus address under the map `mapper` names — snes::LoRom or
    // snes::HiRom, the none mapper one LoROM bank — in the image this core writes, beside every routine
    // already placed, and a fresh machine on it. Refuses an image no byte of the map is at, one crossing
    // the idle loop, the header or another image or routine, a stack top outside $0000-$1FFF, and any
    // placement on a machine hosting a game's cartridge. `stackTop` 0 is $1FFF.
    void configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                std::uint32_t stackTop) override;
    // One entry of the hosted driver, called in the engine's frame on the driver's stack with `presets`
    // over it, to its return; answers the master cycles it spent, exactly. `maxCpuCycles` bounds a routine
    // that never returns: the machine's guard is set to a twelfth of it in instructions, the fewest master
    // cycles an instruction takes.
    std::uint64_t callResident(std::uint32_t entry, std::span<const ResidentRegister> presets,
                               std::uint64_t maxCpuCycles) override;
    // A call into code the machine already holds, on the guest's own stack or the engine's scratch top,
    // the file put back afterwards. A machine parked by a cycle budget is usually part-way through an
    // instruction; that instruction is finished first, before the presets go over the file, so the
    // instruction completes under the guest's own registers and the call lands at an instruction
    // boundary. The entry's top bit (snes::rtl) picks the landing a JSL pushes over the one a JSR pushes.
    // From inside an escape the call is the same call; from inside a watch it throws std::logic_error —
    // the machine is part-way through the access the watch is deciding.
    void callInContext(std::uint32_t entry, std::span<const ResidentRegister> presets,
                       CallStack stack, std::size_t maxInstructions,
                       const std::function<void()>& readOutputs) override;

    // Escapes: an instruction watch on the byte the address reaches, in work RAM, the cartridge image or
    // its save. A replacing escape stands a return in for the fetch that begins the routine — an RTS, or
    // an RTL for an address snes::rtl wraps — so the routine's body never runs and nothing is written.
    // Every armed escape whose byte the fetch reaches is reported, in the order they were armed; two
    // escapes on one byte that would stand different returns there are refused. A register, open bus or
    // a memory the bus cannot name throws std::invalid_argument.
    void setEscapeSink(EscapeSink sink) override;
    void armEscape(std::uint32_t address, bool replacesRoutine) override;
    void disarmEscape(std::uint32_t address) override;
    // The live register file, inside an escape: what the guest's next instruction runs under.
    void writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int width) override;

    // Watches: an access watch on every byte the place spans — work RAM, the cartridge image, its save,
    // or a register — armed byte by byte through an address that reaches it, per direction. Every source
    // fires one: the CPU, both transfer engines and the work-RAM port. The sink is told the declared base
    // and the 24-bit address the access drove, and the machine realizes the answer itself. A memory the
    // bus cannot name, or open bus, throws std::invalid_argument.
    void setWatchSink(WatchSink sink) override;
    void armWatch(const MemoryRegion& where, bool onRead, bool onWrite) override;
    void disarmWatch(const MemoryRegion& where, bool onRead, bool onWrite) override;

    // Picture: forward each frame Snaggletooth finishes to the engine's sink, as Rgba8888 — a whole frame,
    // or one field of an interlaced picture while the program has the chip interlace.
    void setFrameSink(FrameSink sink) override;
    void setVideoEnabled(bool enabled) override;

    // Save data: the cartridge's battery-backed save RAM. The core has the model, so a machine can be
    // keyed; how many bytes an image keeps is the image's own answer.
    [[nodiscard]] bool keepsSaveData() const override { return true; }
    // The name every other program that reads a SNES cartridge's save already expects.
    [[nodiscard]] std::string_view saveDataExtension() const override { return "srm"; }
    [[nodiscard]] std::size_t saveDataSize() const override;
    [[nodiscard]] std::vector<std::uint8_t> readSaveData() override;
    void writeSaveData(std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] bool takeSaveDataChanged() override;

private:
    friend struct SnesBackendTestAccess;  // src/vm/snes/snes_backend_testing.h — reads state() for tests

    // Build a fresh machine from the held image and region (power-on) and re-attach the observers the new
    // machine does not carry. A fresh machine has changed nothing, so the save-changed flag clears here.
    void emplaceMachine();
    // Put a save back into the live machine and clear the changed flag a restore would otherwise raise.
    void restoreSave(std::vector<std::uint8_t> save);
    // Hand the frames the DSP produced since the last drain to the sink at its rate, or drop them when
    // no sink listens — either way the machine's queue is empty when this returns.
    void drainAudio();
    // Write the image that holds every placed routine and put it in the machine: patched in place when
    // the image keeps its size, a fresh machine when it grew.
    void rebuildRoutineImage();
    // Every run of bytes the engine's image carries: the placed routines, then the driver's images.
    [[nodiscard]] std::vector<snes_image::Placement> everything() const;
    // Seat the program counter of `file` on the image's idle loop, where a call lands and the machine
    // parks between calls.
    void seatOnIdleLoop(snaggletooth::Cpu65816State& file) const;
    // The machine at an instruction boundary: an instruction a cycle budget stopped part-way through, or
    // an interrupt sequence in flight, is finished first — under the guest's own registers, before a
    // call's presets go over them.
    void finishInstruction();

    // The memory a place is in, the offset of its first byte there, and that memory's size. Nothing when
    // no cartridge is hosted, or when the address names no memory: open bus, a space this console does
    // not have, or an offset past the end of the memory it names. A register is a memory here — its
    // value read as it stands, at its offset in the system banks' low half — that takes no write.
    enum class Memory : std::uint8_t {
        WorkRam, Cartridge, Save, Register, VideoRam, Palette, Sprites, AudioRam, AudioPort, DspRegister
    };
    struct Resolved {
        Memory      memory;
        std::size_t base;
        std::size_t size;
    };
    [[nodiscard]] std::optional<Resolved> resolve(std::uint32_t address) const;
    // Where entry `index` of `region` starts, checked to lie whole inside the memory `region` resolves to.
    // Throws std::out_of_range for an index the region does not declare, a region that resolves nowhere,
    // or an entry that runs past its memory's end.
    [[nodiscard]] std::pair<Memory, std::size_t> entryAt(const MemoryRegion& region,
                                                         std::uint32_t index) const;
    [[nodiscard]] std::uint8_t readByte(Memory memory, std::size_t offset) const;
    void writeByte(Memory memory, std::size_t offset, std::uint8_t value);

    // FrameObserver / SaveObserver — the machine reports its picture and its save through these; the
    // backend is both, so it is its own observer and needs no back-pointer.
    void frame(const snaggletooth::VideoFrame& frame) override;
    void changed(std::span<const std::uint8_t> save) override;

    // InstructionWatcher / AccessWatcher — the machine tells an armed instruction and an armed access
    // through these, and the backend asks the host layer's sinks.
    void reached(std::uint32_t address) override;
    snaggletooth::AccessAnswer read(std::uint32_t address, std::uint8_t value,
                                    snaggletooth::AccessSource source, snaggletooth::CycleKind kind,
                                    std::uint8_t cycle) override;
    snaggletooth::AccessAnswer write(std::uint32_t address, std::uint8_t value,
                                     snaggletooth::AccessSource source, snaggletooth::CycleKind kind,
                                     std::uint8_t cycle) override;
    // Ask the watch sink about one access and answer the machine with its verdict.
    snaggletooth::AccessAnswer askWatch(std::uint32_t address, AccessKind kind, std::uint8_t value);

    // An armed escape: the address as the host layer armed it, the byte it reaches, and what stands there.
    struct ArmedEscape {
        std::uint32_t                encoded;
        snaggletooth::Snes::Physical place;
        snaggletooth::Standin        standin;
    };
    // An armed watch: the declared base, the memory its first byte is in and how many bytes it spans, and
    // the directions it asks about.
    struct ArmedWatch {
        std::uint32_t                encoded;
        std::uint64_t                span;
        snaggletooth::Snes::Physical first;
        bool                         onRead;
        bool                         onWrite;
        [[nodiscard]] bool covers(snaggletooth::Snes::Physical p) const noexcept {
            return p.space == first.space && p.index >= first.index && p.index - first.index < span;
        }
    };
    // Put every armed escape and watch into the live machine — the watches Snaggletooth keeps are not part
    // of its state, so a fresh machine carries none — and install each watcher only while there is both
    // something armed and a sink to ask.
    void rearm();
    void installWatchers();
    // Arm every byte `w` covers, in the directions it asks about.
    void armWatchBytes(const ArmedWatch& w);
    // Release the bytes `gone` covered, in the directions named, that no remaining watch still covers.
    void releaseWatchBytes(const ArmedWatch& gone, bool onRead, bool onWrite);

    std::optional<snaggletooth::Snes> snes_;             // the live machine; empty until an image is held
    std::vector<std::uint8_t>         rom_;              // the held image, kept for save reads and rebuilds
    snaggletooth::Region              region_ = snaggletooth::Region::Ntsc;  // the cartridge's, from its header
    snaggletooth::CartridgeMap        map_ = snaggletooth::CartridgeMap::LoRom;  // the map the machine reads the image by
    bool                              romHosted_    = false;  // loadRom has run: the image is the game's
    bool                              imageBuilt_   = false;  // the image is the engine's own (snes_image.h)
    std::vector<snes_image::Placement> placed_;             // every routine in the engine's image
    std::vector<snes_image::Placement> resident_;           // the hosted driver's images in it
    bool                              residentHosted_   = false;   // configureResidentImage has run
    std::uint16_t                     residentStackTop_ = 0x1FFF;  // where the driver's entries push
    snaggletooth::Cpu65816State       pending_{};     // the frame beginCall stages for run()
    std::uint32_t                     pendingEntry_ = 0;  // the entry run() calls
    bool                              videoEnabled_ = false;  // remembered so a rebuild re-attaches the frame observer
    bool                              saveChanged_  = false;  // the guest changed the save since it was last taken
    FrameSink                         frameSink_;     // where a finished frame is forwarded
    EscapeSink                        escapeSink_;    // told each armed instruction the machine reaches
    WatchSink                         watchSink_;     // asked about each armed access
    std::vector<ArmedEscape>          armedEscapes_;  // in the order they were armed
    std::vector<ArmedWatch>           armedWatches_;  // in the order they were armed
    int                               insideWatch_ = 0;  // how deep the access watcher's calls are nested
    AudioSampleSink                   audioSink_;     // where each converted frame goes; empty until enableAudio
    std::optional<RationalResampler>  resampler_;     // 32'000 Hz to the sink's rate; built by enableAudio
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_SNES_BACKEND_H
