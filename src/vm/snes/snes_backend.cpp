#include "src/vm/snes/snes_backend.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

#include "retropp/vm.h"                   // VMPlatform + detail::snesCore's declaration
#include "snaggletooth/snes/cartridge.h"  // parseCartridgeHeader — the region the machine is built at

namespace retropp::vm {

namespace {

// The console rate the cartridge's own country byte asks for; NTSC for an image whose header does not say,
// which is what the reference player does for an image that does not declare one.
snaggletooth::Region regionOf(std::span<const std::uint8_t> rom) {
    const std::optional<snaggletooth::CartridgeHeader> header = snaggletooth::parseCartridgeHeader(rom);
    return header && header->video == snaggletooth::VideoStandard::Pal ? snaggletooth::Region::Pal
                                                                       : snaggletooth::Region::Ntsc;
}

// The rate the sound chip produces at, whatever the region: one frame every 32 cycles of its own
// 1'024'000 Hz clock.
constexpr unsigned kDspRate = 32'000u;

// One port decoded from the seam's opaque word: the twelve buttons in the pad's shift order when the
// port's occupied bit is set, an empty socket when it is not. `shift` is 0 for port one, 16 for two —
// snes.h's packing.
std::optional<snaggletooth::Joypad> decodePad(std::uint64_t word, unsigned shift) {
    const std::uint64_t half = word >> shift;
    if ((half & (std::uint64_t{1} << 15)) == 0) {
        return std::nullopt;  // an empty socket, told apart from a pad holding nothing
    }
    snaggletooth::Joypad pad;
    pad.b      = (half >> 0) & 1u;
    pad.y      = (half >> 1) & 1u;
    pad.select = (half >> 2) & 1u;
    pad.start  = (half >> 3) & 1u;
    pad.up     = (half >> 4) & 1u;
    pad.down   = (half >> 5) & 1u;
    pad.left   = (half >> 6) & 1u;
    pad.right  = (half >> 7) & 1u;
    pad.a      = (half >> 8) & 1u;
    pad.x      = (half >> 9) & 1u;
    pad.l      = (half >> 10) & 1u;
    pad.r      = (half >> 11) & 1u;
    return pad;
}

}  // namespace

void SnesBackend::emplaceMachine() {
    snes_.emplace(snaggletooth::SnesConfig{.rom = rom_, .region = region_});
    snes_->setSaveObserver(this);
    snes_->setFrameObserver(videoEnabled_ ? this : nullptr);
    saveChanged_ = false;  // a fresh machine has changed nothing since it was last taken
    if (resampler_) {
        resampler_->reset();  // a fresh machine's sound starts from silence, as its picture does
    }
}

void SnesBackend::restoreSave(std::vector<std::uint8_t> save) {
    snaggletooth::SnesState state = snes_->state();  // the once-per-load round trip, never per step
    state.sram = std::move(save);
    snes_->restore(state);
    saveChanged_ = false;  // a restore reports a change; a loaded save is not one
}

void SnesBackend::reset() {
    if (!romHosted_) {
        throw std::logic_error("reset: no cartridge is hosted on this machine (host the image first)");
    }
    std::vector<std::uint8_t> save = snes_->state().sram;  // the save survives a reset, as a battery does
    emplaceMachine();
    if (!save.empty()) {
        restoreSave(std::move(save));
    }
}

void SnesBackend::loadRom(std::span<const std::uint8_t> rom) {
    if (rom.empty()) {
        throw std::invalid_argument("the cartridge image has no bytes");
    }
    rom_.assign(rom.begin(), rom.end());  // kept for save reads and machine rebuilds
    region_ = regionOf(rom_);             // the cartridge's own region, from its header
    emplaceMachine();                     // construction is power-on: work RAM cleared, PC at the reset vector
    romHosted_ = true;
}

void SnesBackend::bootHostedRom() {
    if (!romHosted_) {
        throw std::logic_error(
            "bootHostedRom: no game cartridge is hosted on this machine (host the image first)");
    }
    // Construction leaves the machine where the contract asks — firmware-exit state, PC at the cartridge's
    // entry, not stepped — so a fresh machine IS the boot. The save the host loaded before the first run
    // survives it, as a cartridge's battery does through a power cycle.
    std::vector<std::uint8_t> save = snes_->state().sram;
    emplaceMachine();
    if (!save.empty()) {
        restoreSave(std::move(save));
    }
}

std::uint64_t SnesBackend::runForCycles(std::uint64_t cpuCycles) {
    // A quarter of a frame at a time, the DSP drained after each slice: the frames a step produces
    // reach the sink in four batches rather than one, and the machine's queue never holds more than a
    // quarter frame. Snaggletooth carries the overshoot inside the machine and run(a) then run(b) is
    // run(a + b), so the slicing changes nothing the program can observe.
    const std::uint64_t slice = clock()->cyclesPerFrame / 4;
    for (std::uint64_t remaining = cpuCycles; remaining != 0;) {
        const std::uint64_t step = std::min(remaining, slice);
        snes_->run(step);
        drainAudio();
        remaining -= step;
    }
    return cpuCycles;
}

void SnesBackend::drainAudio() {
    const std::vector<snaggletooth::StereoFrame> frames = snes_->takeFrames();
    if (!audioSink_) {
        return;  // nobody listens: the frames are dropped, as an unwatched picture is never drawn
    }
    for (const snaggletooth::StereoFrame& frame : frames) {
        resampler_->push(frame.left, frame.right, audioSink_);
    }
}

void SnesBackend::enableAudio(unsigned sampleRate, AudioSampleSink sink) {
    if (sampleRate == 0) {
        throw std::invalid_argument("enableAudio: the sink's rate is zero");
    }
    resampler_.emplace(kDspRate, sampleRate);
    audioSink_ = std::move(sink);
}

void SnesBackend::setButtons(std::uint64_t held) {
    if (!snes_) {
        return;  // nothing hosted; a running machine always has one
    }
    snes_->setJoypad(snaggletooth::JoypadPort::One, decodePad(held, 0));
    snes_->setJoypad(snaggletooth::JoypadPort::Two, decodePad(held, 16));
}

void SnesBackend::frame(const snaggletooth::VideoFrame& frame) {
    if (frameSink_) {
        frameSink_(frame.pixels, static_cast<int>(frame.width), static_cast<int>(frame.height),
                   GuestPixelFormat::Rgba8888);
    }
}

void SnesBackend::setFrameSink(FrameSink sink) { frameSink_ = std::move(sink); }

void SnesBackend::setVideoEnabled(bool enabled) {
    videoEnabled_ = enabled;
    if (snes_) {
        snes_->setFrameObserver(enabled ? this : nullptr);
    }
}

void SnesBackend::changed(std::span<const std::uint8_t>) { saveChanged_ = true; }

std::size_t SnesBackend::saveDataSize() const {
    return romHosted_ ? snes_->state().sram.size() : 0;
}

std::vector<std::uint8_t> SnesBackend::readSaveData() {
    return romHosted_ ? snes_->state().sram : std::vector<std::uint8_t>{};
}

void SnesBackend::writeSaveData(std::span<const std::uint8_t> bytes) {
    if (!romHosted_) {
        throw std::invalid_argument(
            "writeSaveData: this machine hosts no cartridge of its own, so it keeps nothing");
    }
    if (bytes.size() != snes_->state().sram.size()) {
        throw std::invalid_argument("writeSaveData: this save is not the size this cartridge keeps");
    }
    restoreSave(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

bool SnesBackend::takeSaveDataChanged() {
    if (!saveChanged_) {
        return false;
    }
    saveChanged_ = false;
    return true;
}

// ── The verbs this core does not realize. Each throws std::logic_error naming what the core does now.
//    setEscapeSink / setWatchSink store the sink; installing one is not arming.
void SnesBackend::advanceClock(std::uint64_t) {
    throw std::logic_error("advanceClock: the SNES core advances only by running its cartridge");
}
std::uint32_t SnesBackend::placeRoutine(std::span<const std::uint8_t>) {
    throw std::logic_error("placeRoutine: the SNES core hosts a cartridge and keeps no routine arena");
}
AssembledRoutine SnesBackend::assemble(std::string_view) const {
    throw std::logic_error("assemble: the SNES core assembles no routine source");
}
int SnesBackend::registerWidthBytes(std::uint16_t) const { return 0; }
bool SnesBackend::regionIsAddressable(const MemoryRegion&) const { return false; }
void SnesBackend::readRegion(const MemoryRegion&, std::uint32_t, std::span<std::uint8_t>) {
    throw std::logic_error("readRegion: the SNES core names no places in guest memory");
}
void SnesBackend::writeRegion(const MemoryRegion&, std::uint32_t, std::span<const std::uint8_t>) {
    throw std::logic_error("writeRegion: the SNES core names no places in guest memory");
}
void SnesBackend::beginCall(std::uint32_t) {
    throw std::logic_error("beginCall: the SNES core makes no routine calls");
}
void SnesBackend::writeRegister(std::uint16_t, std::uint64_t, int) {
    throw std::logic_error("writeRegister: the SNES core makes no routine calls");
}
void SnesBackend::writeMemory(std::uint32_t, std::uint64_t, int) {
    throw std::logic_error("writeMemory: the SNES core names no places in guest memory");
}
void SnesBackend::run() { throw std::logic_error("run: the SNES core makes no routine calls"); }
std::uint64_t SnesBackend::readRegister(std::uint16_t) {
    throw std::logic_error("readRegister: the SNES core makes no routine calls");
}
std::uint64_t SnesBackend::readMemory(std::uint32_t, int) {
    throw std::logic_error("readMemory: the SNES core names no places in guest memory");
}
void SnesBackend::beginContinuous(std::uint32_t) {
    throw std::logic_error("beginContinuous: the SNES core hosts no driver");
}
void SnesBackend::configureResidentImage(std::span<const DriverImage>, Mapper, std::uint32_t) {
    throw std::logic_error("configureResidentImage: the SNES core hosts no resident driver");
}
std::uint64_t SnesBackend::callResident(std::uint32_t, std::span<const ResidentRegister>, std::uint64_t) {
    throw std::logic_error("callResident: the SNES core hosts no resident driver");
}
void SnesBackend::callInContext(std::uint32_t, std::span<const ResidentRegister>, CallStack, std::size_t,
                                const std::function<void()>&) {
    throw std::logic_error("callInContext: the SNES core makes no routine calls");
}
void SnesBackend::setEscapeSink(EscapeSink sink) { escapeSink_ = std::move(sink); }
void SnesBackend::armEscape(std::uint32_t, bool) {
    throw std::logic_error("armEscape: the SNES core answers no escapes");
}
void SnesBackend::disarmEscape(std::uint32_t) {
    throw std::logic_error("disarmEscape: the SNES core answers no escapes");
}
void SnesBackend::writeLiveRegister(std::uint16_t, std::uint64_t, int) {
    throw std::logic_error("writeLiveRegister: the SNES core answers no escapes");
}
void SnesBackend::setWatchSink(WatchSink sink) { watchSink_ = std::move(sink); }
void SnesBackend::armWatch(const MemoryRegion&, bool, bool) {
    throw std::logic_error("armWatch: the SNES core answers no watches");
}
void SnesBackend::disarmWatch(const MemoryRegion&, bool, bool) {
    throw std::logic_error("disarmWatch: the SNES core answers no watches");
}

}  // namespace retropp::vm

namespace retropp::detail {

// The SNES core; the region follows the cartridge, so the platform argument is not read.
std::unique_ptr<vm::VmBackend> snesCore(VMPlatform /*platform*/) {
    return std::make_unique<vm::SnesBackend>();
}

}  // namespace retropp::detail
