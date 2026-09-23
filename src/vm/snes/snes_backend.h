// Internal SNES / 65816 VM backend — a VmBackend over Snaggletooth's Snes machine.
//
// This is the ONE place SNES machine idiom lives: the two-port controller word, the region read from a
// cartridge's own header, and the frame/save observers the machine reports through. The generic Vm
// (vm.cpp) drives it only through VmBackend, so it knows none of it. The machine's routine and register
// vocabulary — naming places, calling routines, escapes, watches, driver hosting, its APU — is not among
// this core's verbs: each such verb throws, in the seam's posture, so no capability is faked.
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
#include <vector>

#include "snaggletooth/snes/snes.h"
#include "src/vm/vm_backend.h"

namespace retropp::vm {

class SnesBackend final : public VmBackend,
                          private snaggletooth::FrameObserver,
                          private snaggletooth::SaveObserver {
public:
    SnesBackend() = default;

    void reset() override;
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
    std::uint32_t placeRoutine(std::span<const std::uint8_t> bytes) override;
    void loadRom(std::span<const std::uint8_t> rom) override;
    void bootHostedRom() override;
    [[nodiscard]] AssembledRoutine assemble(std::string_view source) const override;
    [[nodiscard]] int registerWidthBytes(std::uint16_t registerId) const override;
    [[nodiscard]] bool regionIsAddressable(const MemoryRegion& region) const override;
    void readRegion(const MemoryRegion& region, std::uint32_t index,
                    std::span<std::uint8_t> out) override;
    void writeRegion(const MemoryRegion& region, std::uint32_t index,
                     std::span<const std::uint8_t> bytes) override;

    void beginCall(std::uint32_t entry) override;
    void writeRegister(std::uint16_t registerId, std::uint64_t value, int width) override;
    void writeMemory(std::uint32_t address, std::uint64_t value, int width) override;
    void run() override;
    [[nodiscard]] std::uint64_t readRegister(std::uint16_t registerId) override;
    [[nodiscard]] std::uint64_t readMemory(std::uint32_t address, int width) override;

    void enableAudio(unsigned sampleRate, AudioSampleSink sink) override;
    void beginContinuous(std::uint32_t entry) override;
    std::uint64_t runForCycles(std::uint64_t cpuCycles) override;

    // Guest input: the SNES pad. The public two-port word (snes.h) packs into the seam's opaque uint64.
    [[nodiscard]] bool takesButtons() const override { return true; }
    void setButtons(std::uint64_t held) override;

    void configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                std::uint32_t stackTop) override;
    std::uint64_t callResident(std::uint32_t entry, std::span<const ResidentRegister> presets,
                               std::uint64_t maxCpuCycles) override;
    void callInContext(std::uint32_t entry, std::span<const ResidentRegister> presets,
                       CallStack stack, std::size_t maxInstructions,
                       const std::function<void()>& readOutputs) override;

    void setEscapeSink(EscapeSink sink) override;
    void armEscape(std::uint32_t address, bool replacesRoutine) override;
    void disarmEscape(std::uint32_t address) override;
    void writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int width) override;

    void setWatchSink(WatchSink sink) override;
    void armWatch(const MemoryRegion& where, bool onRead, bool onWrite) override;
    void disarmWatch(const MemoryRegion& where, bool onRead, bool onWrite) override;

    // Picture: forward each frame Snaggletooth finishes to the engine's sink, as Rgba8888.
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

    // FrameObserver / SaveObserver — the machine reports its picture and its save through these; the
    // backend is both, so it is its own observer and needs no back-pointer.
    void frame(const snaggletooth::VideoFrame& frame) override;
    void changed(std::span<const std::uint8_t> save) override;

    std::optional<snaggletooth::Snes> snes_;             // the live machine; empty until a ROM is hosted
    std::vector<std::uint8_t>         rom_;              // the hosted image, kept for save reads and rebuilds
    snaggletooth::Region              region_ = snaggletooth::Region::Ntsc;  // the cartridge's, from its header
    bool                              romHosted_    = false;  // loadRom has run
    bool                              videoEnabled_ = false;  // remembered so a rebuild re-attaches the frame observer
    bool                              saveChanged_  = false;  // the guest changed the save since it was last taken
    FrameSink                         frameSink_;     // where a finished frame is forwarded
    EscapeSink                        escapeSink_;    // stored; this core answers no escapes
    WatchSink                         watchSink_;     // stored; this core answers no watches
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_SNES_BACKEND_H
