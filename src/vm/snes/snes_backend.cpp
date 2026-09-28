#include "src/vm/snes/snes_backend.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "cpu65816_asm.h"                 // assembleCpu65816 — the 65816 source a routine is written in
#include "retropp/snes.h"                 // snes::Space, snes::Reg — the top byte of a place's address, the register ids
#include "retropp/vm.h"                   // VMPlatform + detail::snesCore's declaration
#include "snaggletooth/snes/cartridge.h"  // parseCartridgeHeader — the region the machine is built at
#include "src/vm/snes/snes_address.h"     // decode, busAddressOf — a place resolved to its byte
#include "src/vm/snes/snes_image.h"       // the image a routine machine writes for what it places

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
    // The map is named outright: a game's cartridge is read by the map its header names, the engine's
    // own image by the map it was written for, and neither depends on the machine reading the header.
    snes_.emplace(snaggletooth::SnesConfig{.rom = rom_, .region = region_, .map = map_});
    snes_->setSaveObserver(this);
    snes_->setFrameObserver(videoEnabled_ ? this : nullptr);
    rearm();               // escapes and watches are the host's, not the machine's state
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
    if (imageBuilt_) {
        emplaceMachine();  // the image holds every placed routine; a fresh machine on it is the reset
        return;
    }
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
    if (imageBuilt_) {
        throw std::logic_error(
            "this VM already holds an image the engine wrote for its routines; a game's cartridge "
            "cannot share it");
    }
    rom_.assign(rom.begin(), rom.end());  // kept for save reads and machine rebuilds
    region_ = regionOf(rom_);             // the cartridge's own region, from its header
    map_    = snaggletooth::detectCartridgeMap(rom_);  // the map the machine reads it by, found the same way
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
    if (!frameSink_) {
        return;
    }
    // A frame drawn while the program asks the chip to interlace is one field of the picture, at the
    // parity the machine reports; otherwise it is the whole picture, whatever parity the machine counts.
    // The register is read as the frame completes, so the frame the program switches it in is reported
    // by the value the switch left.
    const FrameField field = !snes_->state().ppu.interlace() ? FrameField::Whole
                             : (frame.field & 1u) != 0u    ? FrameField::Odd
                                                            : FrameField::Even;
    frameSink_(frame.pixels, static_cast<int>(frame.width), static_cast<int>(frame.height),
               RasterPixelFormat::Rgba8888, field);
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

// ── Places ──────────────────────────────────────────────────────────────────────────────────────
// A place on the bus resolves through the machine's own classification, so every alias of a byte lands
// on that byte and a run strides through the memory it starts in, never through the addresses after
// its base. A place in a memory the bus cannot name is an offset into that memory. Reads come straight
// from the memory — a register's from the value a read would answer, with nothing moved; writes go
// through the verbs that write it by name, so no register is driven and no cycle is spent.

std::optional<SnesBackend::Resolved> SnesBackend::resolve(std::uint32_t address) const {
    if (!snes_) {
        return std::nullopt;  // nothing hosted: this machine has no memory yet
    }
    const std::optional<snes_address::Decoded> decoded = snes_address::decode(address);
    if (!decoded) {
        return std::nullopt;
    }
    const std::size_t at = decoded->at24;
    const auto within = [at](Memory memory, std::size_t size) -> std::optional<Resolved> {
        if (at >= size) {
            return std::nullopt;
        }
        return Resolved{.memory = memory, .base = at, .size = size};
    };
    switch (decoded->space) {
        case snes::Space::VideoRam: return within(Memory::VideoRam, snes_->vram().size());
        case snes::Space::Palette:  return within(Memory::Palette, snes_->cgram().size());
        case snes::Space::Sprites:  return within(Memory::Sprites, snes_->oam().size());
        case snes::Space::AudioRam: return within(Memory::AudioRam, 0x10000u);
        case snes::Space::Bus:      break;
    }
    const snaggletooth::Snes::Physical place = snes_->physical(decoded->at24);
    switch (place.space) {
        case snaggletooth::Snes::Space::WorkRam:
            return Resolved{.memory = Memory::WorkRam, .base = place.index, .size = snes_->state().wram.size()};
        case snaggletooth::Snes::Space::CartridgeRom:
            return Resolved{.memory = Memory::Cartridge, .base = place.index, .size = rom_.size()};
        case snaggletooth::Snes::Space::SaveRam:
            return Resolved{.memory = Memory::Save, .base = place.index, .size = snes_->state().sram.size()};
        case snaggletooth::Snes::Space::Register:
            // The registers' offsets in the system banks' low half; whether each byte of a run is a
            // register is answered byte by byte (regionIsAddressable), since the windows have gaps.
            return Resolved{.memory = Memory::Register, .base = place.index, .size = 0x10000};
        case snaggletooth::Snes::Space::OpenBus:  // no memory answers here
            return std::nullopt;
    }
    return std::nullopt;
}

bool SnesBackend::regionIsAddressable(const MemoryRegion& region) const {
    const std::uint64_t total = region.totalBytes();
    if (total == 0) {
        return false;  // a place spanning no bytes names nothing
    }
    const std::optional<Resolved> resolved = resolve(region.at);
    if (!resolved || resolved->base + total > resolved->size) {
        return false;
    }
    if (resolved->memory == Memory::Register) {
        // Every byte of the run is a register: the windows have open bus between them.
        for (std::uint64_t i = 0; i < total; ++i) {
            if (!snes_->peekRegister(static_cast<std::uint32_t>(resolved->base + i))) {
                return false;
            }
        }
    }
    return true;
}

std::pair<SnesBackend::Memory, std::size_t> SnesBackend::entryAt(const MemoryRegion& region,
                                                                 std::uint32_t index) const {
    if (!region.contains(index)) {
        throw std::out_of_range("entry " + std::to_string(index) + " is past the " +
                                std::to_string(region.count) + " this place declares");
    }
    const std::optional<Resolved> resolved = resolve(region.at);
    if (!resolved) {
        throw std::out_of_range("SNES address " + std::to_string(region.at) +
                                " names no memory on this machine");
    }
    // The stride is applied in the memory, not to the address: past the end of a bank's window the
    // addresses after the base reach something else, and the memory's next byte is what the run means.
    const std::size_t offset = resolved->base + static_cast<std::size_t>(region.size) * index;
    if (offset + region.size > resolved->size) {
        throw std::out_of_range("this place runs past the end of the memory it starts in");
    }
    return {resolved->memory, offset};
}

std::uint8_t SnesBackend::readByte(Memory memory, std::size_t offset) const {
    switch (memory) {
        case Memory::WorkRam:   return snes_->state().wram[offset];
        case Memory::Cartridge: return rom_[offset];
        case Memory::Save:      return snes_->state().sram[offset];
        case Memory::Register: {
            // The byte a read would answer, with nothing moved: no flag cleared, no port clocked, no
            // address stepped. Bank $00 is one of the system banks every register answers in.
            const std::optional<std::uint8_t> value = snes_->peekRegister(static_cast<std::uint32_t>(offset));
            if (!value) {
                throw std::out_of_range("no register is at offset " + std::to_string(offset));
            }
            return *value;
        }
        case Memory::VideoRam:  return snes_->vram()[offset];
        case Memory::Palette:   return snes_->cgram()[offset];
        case Memory::Sprites:   return snes_->oam()[offset];
        case Memory::AudioRam:  return snes_->peekApu(static_cast<std::uint16_t>(offset));
    }
    return 0;
}

void SnesBackend::writeByte(Memory memory, std::size_t offset, std::uint8_t value) {
    const auto pokeAt = [&](snaggletooth::Snes::Space space) {
        const std::optional<std::uint32_t> address = snes_address::busAddressOf(
            map_, snaggletooth::Snes::Physical{.space = space, .index = static_cast<std::uint32_t>(offset)});
        if (!address || !snes_->poke(*address, value)) {
            throw std::out_of_range("no bus address reaches byte " + std::to_string(offset) +
                                    " of this memory");
        }
    };
    switch (memory) {
        case Memory::WorkRam:
            pokeAt(snaggletooth::Snes::Space::WorkRam);
            return;
        case Memory::Cartridge:
            // The machine's copy and the image a rebuild starts from are written together, so a patch
            // survives reset() and a fresh boot as the image itself would.
            pokeAt(snaggletooth::Snes::Space::CartridgeRom);
            rom_[offset] = value;
            return;
        case Memory::Save:
            pokeAt(snaggletooth::Snes::Space::SaveRam);
            return;
        case Memory::Register:
            // A register's value is read as it stands; writing one is a program's own store, with the
            // effects a store has, and goes through a routine.
            throw std::logic_error("a register takes no write through a place; a routine writes it");
        case Memory::VideoRam:
            snes_->writeVram(static_cast<std::uint16_t>(offset), value);
            return;
        case Memory::Palette:
            snes_->writeCgram(static_cast<std::uint16_t>(offset), value);
            return;
        case Memory::Sprites:
            snes_->writeOam(static_cast<std::uint16_t>(offset), value);
            return;
        case Memory::AudioRam:
            snes_->writeApuRam(static_cast<std::uint16_t>(offset), value);
            return;
    }
}

void SnesBackend::readRegion(const MemoryRegion& region, std::uint32_t index, std::span<std::uint8_t> out) {
    if (out.size() != region.size) {
        throw std::invalid_argument("a region read takes exactly one entry's worth of bytes");
    }
    const auto [memory, offset] = entryAt(region, index);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = readByte(memory, offset + i);
    }
}

void SnesBackend::writeRegion(const MemoryRegion& region, std::uint32_t index,
                              std::span<const std::uint8_t> bytes) {
    if (bytes.size() != region.size) {
        throw std::invalid_argument("a region write takes exactly one entry's worth of bytes");
    }
    const auto [memory, offset] = entryAt(region, index);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        writeByte(memory, offset + i, bytes[i]);
    }
}

void SnesBackend::writeMemory(std::uint32_t address, std::uint64_t value, int width) {
    // Through the same resolver a region write uses, so a word and a range cannot disagree about where an
    // address is. Little-endian, as the 65816 stores a word.
    const auto [memory, offset] =
        entryAt(MemoryRegion{.at = address, .size = static_cast<std::uint32_t>(width)}, 0);
    for (int i = 0; i < width; ++i) {
        writeByte(memory, offset + static_cast<std::size_t>(i),
                  static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

std::uint64_t SnesBackend::readMemory(std::uint32_t address, int width) {
    // Through the same resolver a region read uses — see writeMemory.
    const auto [memory, offset] =
        entryAt(MemoryRegion{.at = address, .size = static_cast<std::uint32_t>(width)}, 0);
    std::uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(readByte(memory, offset + static_cast<std::size_t>(i))) << (8 * i);
    }
    return value;
}

// ── Routines ────────────────────────────────────────────────────────────────────────────────────
// A routine machine holds an image the engine wrote (snes_image.h). A placed routine is called in a
// frame of the engine's own, on a stack at $1FFF, with the idle loop as the landing; a routine the
// cartridge holds is called in the guest's own context. The assembler is absolute, so a source names
// its own address and its bytes land there.

namespace {

// The guard on a call, in instructions — the size the Game Boy's run-to-return carries.
constexpr std::size_t kMaxCallInstructions = 1'000'000;

// The frame a placed routine begins in: native mode, 8-bit accumulator and index registers with
// interrupts off (M, X and I set), direct page $0000, data bank $00, the stack at $1FFF, everything
// else zero. The program bank is the entry's; the program counter is seated by the call.
snaggletooth::Cpu65816State engineFrame() {
    snaggletooth::Cpu65816State frame{};
    frame.p = 0x34;
    frame.s = 0x1FFF;
    frame.e = false;
    return frame;
}

// One register of the file, by the id snes::Reg fixes.
void writeRegisterField(snaggletooth::Cpu65816State& file, snes::Reg reg, std::uint64_t value) {
    const auto low  = static_cast<std::uint8_t>(value & 0xFF);
    const auto wide = static_cast<std::uint16_t>(value & 0xFFFF);
    switch (reg) {
        case snes::Reg::A:  file.a = static_cast<std::uint16_t>((file.a & 0xFF00) | low); return;
        case snes::Reg::B:  file.a = static_cast<std::uint16_t>((file.a & 0x00FF) | (low << 8)); return;
        case snes::Reg::P:  file.p = low; return;
        case snes::Reg::DB: file.dbr = low; return;
        case snes::Reg::PB: file.pbr = low; return;
        case snes::Reg::C:  file.a = wide; return;
        case snes::Reg::X:  file.x = wide; return;
        case snes::Reg::Y:  file.y = wide; return;
        case snes::Reg::D:  file.d = wide; return;
        case snes::Reg::S:  file.s = wide; return;
        case snes::Reg::PC: file.pc = wide; return;
    }
}

std::uint64_t readRegisterField(const snaggletooth::Cpu65816State& file, snes::Reg reg) {
    switch (reg) {
        case snes::Reg::A:  return file.a & 0xFF;
        case snes::Reg::B:  return (file.a >> 8) & 0xFF;
        case snes::Reg::P:  return file.p;
        case snes::Reg::DB: return file.dbr;
        case snes::Reg::PB: return file.pbr;
        case snes::Reg::C:  return file.a;
        case snes::Reg::X:  return file.x;
        case snes::Reg::Y:  return file.y;
        case snes::Reg::D:  return file.d;
        case snes::Reg::S:  return file.s;
        case snes::Reg::PC: return file.pc;
    }
    return 0;
}

// A 24-bit bus address as the console writes one: $BB:AAAA.
std::string busAddress(std::uint32_t address) {
    char text[16];
    std::snprintf(text, sizeof text, "$%02X:%04X", (address >> 16) & 0xFF, address & 0xFFFF);
    return text;
}

}  // namespace

std::uint32_t SnesBackend::placeRoutine(std::span<const std::uint8_t> bytes,
                                       std::optional<std::uint32_t> origin) {
    if (romHosted_) {
        throw std::logic_error(
            "this VM hosts a game's own cartridge, which has no arena to place a routine into; call "
            "the hosted image's existing entries instead of injecting new code");
    }
    const std::uint32_t at = origin.value_or(snes_image::nextFreeAddress(map_, everything(), bytes.size()));
    const std::optional<std::size_t> offset = snes_image::imageOffset(map_, at);
    if (!offset) {
        throw std::invalid_argument("a routine cannot be placed at " + busAddress(at) +
                                    ": no byte of the image is at that address");
    }
    if (snes_image::overlapsReserved(map_, *offset, bytes.size())) {
        throw std::invalid_argument("a routine at " + busAddress(at) +
                                    " would overlap the image's idle loop or its header");
    }
    for (const snes_image::Placement& p : placed_) {
        const std::size_t theirs = *snes_image::imageOffset(map_, p.origin);
        if (*offset < theirs + p.bytes.size() && theirs < *offset + bytes.size()) {
            throw std::invalid_argument("a routine at " + busAddress(at) +
                                        " would overlap the routine placed at " + busAddress(p.origin));
        }
    }
    for (const snes_image::Placement& p : resident_) {
        const std::size_t theirs = *snes_image::imageOffset(map_, p.origin);
        if (*offset < theirs + p.bytes.size() && theirs < *offset + bytes.size()) {
            throw std::invalid_argument("a routine at " + busAddress(at) +
                                        " would overlap the driver image at " + busAddress(p.origin));
        }
    }
    placed_.push_back(snes_image::Placement{.origin = at,
                                            .bytes  = std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
    rebuildRoutineImage();
    return at;
}

std::vector<snes_image::Placement> SnesBackend::everything() const {
    std::vector<snes_image::Placement> all = placed_;
    all.insert(all.end(), resident_.begin(), resident_.end());
    return all;
}

void SnesBackend::rebuildRoutineImage() {
    std::vector<std::uint8_t> image = snes_image::buildImage(map_, everything());
    if (snes_ && image.size() == rom_.size()) {
        // The same chip: every byte that changed is written into the live machine, and the machine's
        // own state — its registers, its work RAM, its clock — stands.
        for (std::size_t i = 0; i < image.size(); ++i) {
            if (image[i] != rom_[i]) {
                snes_->poke(*snaggletooth::romAddress(map_, i), image[i]);
            }
        }
        rom_ = std::move(image);
        return;
    }
    rom_        = std::move(image);
    imageBuilt_ = true;
    emplaceMachine();
}

AssembledRoutine SnesBackend::assemble(std::string_view source) const {
    snaggletooth::assembler::Assembly assembly =
        snaggletooth::assembler::assembleCpu65816(source, "routine.asm");
    if (assembly.ok() && !assembly.ranges.empty() && assembly.ranges.front().start == 0) {
        // No ORG: the source was assembled at $000000, which is work RAM. Assemble it again at the first
        // gap of the arena that holds it, so every label inside it resolves for where it will land.
        const std::uint32_t at = snes_image::nextFreeAddress(map_, everything(), assembly.ranges.back().start +
                                                                                assembly.ranges.back().bytes.size());
        assembly = snaggletooth::assembler::assembleCpu65816(
            "        ORG " + busAddress(at) + "\n" + std::string(source), "routine.asm");
    }
    if (!assembly.ok()) {
        std::string what = "65816 assembly failed:";
        for (const snaggletooth::assembler::Diagnostic& d : assembly.errors) {
            what += "\n  line " + std::to_string(d.line) + ": " + d.message;
        }
        throw std::runtime_error(what);
    }
    if (assembly.ranges.empty()) {
        throw std::runtime_error("65816 assembly failed: the source emits no bytes");
    }
    // The bytes from the first range's start to the last range's end, gaps zero — the layout the source
    // named; the origin is where the first byte goes.
    const std::uint32_t first = assembly.ranges.front().start;
    const std::uint32_t end =
        assembly.ranges.back().start + static_cast<std::uint32_t>(assembly.ranges.back().bytes.size());
    AssembledRoutine out;
    out.bytes  = *snaggletooth::assembler::image(assembly, first, end - first);
    out.origin = first;
    for (const auto& [name, value] : assembly.symbols) {
        if (value >= first && value < end) {
            out.labels.set(name, value - first);
        }
    }
    return out;
}

int SnesBackend::registerWidthBytes(std::uint16_t registerId) const {
    if (registerId > static_cast<std::uint16_t>(snes::Reg::PC)) {
        return 0;  // not a 65816 register
    }
    return registerId >= static_cast<std::uint16_t>(snes::Reg::C) ? 2 : 1;
}

void SnesBackend::beginCall(std::uint32_t entry) {
    pending_      = engineFrame();
    pendingEntry_ = entry;
}

void SnesBackend::writeRegister(std::uint16_t registerId, std::uint64_t value, int /*width*/) {
    writeRegisterField(pending_, static_cast<snes::Reg>(registerId), value);
}

void SnesBackend::run() {
    if (!snes_) {
        throw std::logic_error("run: this machine holds no code to call (place a routine first)");
    }
    const std::optional<snes_address::Decoded> entry = snes_address::decode(pendingEntry_);
    if (!entry || entry->space != snes::Space::Bus) {
        throw std::invalid_argument("run: the entry " + std::to_string(pendingEntry_) +
                                    " is not a bus address");
    }
    // The landing is the program counter as it stands: the idle loop, where the call ends. The machine
    // is at an instruction boundary — every call ends at one and advanceClock puts the file back — so
    // the call is never refused for being mid-instruction; a refusal here is the entry's own bank not
    // mapping it.
    seatOnIdleLoop(pending_);
    snes_->setCpuState(pending_);
    const snaggletooth::Standin returns =
        entry->rtl ? snaggletooth::Standin::Long : snaggletooth::Standin::Near;
    const bool returned =
        snes_->callOnStack(entry->at24, pending_.s, returns, kMaxCallInstructions);
    if (!returned && snes_->state().master == 0 && snes_->cpuState() == pending_) {
        throw std::logic_error("run: the machine refused the call at " + busAddress(entry->at24) +
                               " — no byte of the image is at that address");
    }
}

std::uint64_t SnesBackend::readRegister(std::uint16_t registerId) {
    return readRegisterField(snes_->cpuState(), static_cast<snes::Reg>(registerId));
}

void SnesBackend::advanceClock(std::uint64_t cycles) {
    if (!imageBuilt_) {
        throw std::logic_error(romHosted_
                                   ? "advanceClock: this machine hosts a game's cartridge, which "
                                     "advances by running"
                                   : "advanceClock: this machine holds no image to idle on (place a "
                                     "routine first)");
    }
    if (cycles == 0) {
        return;
    }
    // Park on the idle loop, spend the cycles, and put the file back: the machine's clock moves and
    // nothing a call marshals does. A call's cycles are spent ahead of the budget the machine runs, so
    // the budget owes them; they are paid here, and the clock moves by the whole of `cycles` from where
    // it stands.
    const snaggletooth::Cpu65816State saved = snes_->cpuState();
    snaggletooth::Cpu65816State idle = saved;
    seatOnIdleLoop(idle);
    snes_->setCpuState(idle);
    const snaggletooth::SnesState& state = snes_->state();
    const std::uint64_t owed = state.master > state.consumed ? state.master - state.consumed : 0;
    snes_->run(cycles + owed);
    snes_->setCpuState(saved);
    drainAudio();  // what the sound chip made while the machine idled reaches the sink
}

void SnesBackend::seatOnIdleLoop(snaggletooth::Cpu65816State& file) const {
    const std::uint32_t idle = snes_image::idleLoop(map_);
    file.pc  = static_cast<std::uint16_t>(idle & 0xFFFF);
    file.pbr = static_cast<std::uint8_t>(idle >> 16);
}

void SnesBackend::finishInstruction() {
    while (snes_->cpuState().tcu != 0 ||
           snes_->cpuState().servicing != snaggletooth::InterruptRequest::None) {
        snes_->step();
    }
}

void SnesBackend::callInContext(std::uint32_t entry, std::span<const ResidentRegister> presets,
                                CallStack stack, std::size_t maxInstructions,
                                const std::function<void()>& readOutputs) {
    if (!snes_) {
        throw std::logic_error("callInContext: this machine holds no code to call");
    }
    const std::optional<snes_address::Decoded> decoded = snes_address::decode(entry);
    if (!decoded || decoded->space != snes::Space::Bus) {
        throw std::invalid_argument("callInContext: the entry " + std::to_string(entry) +
                                    " is not a bus address");
    }
    if (!snes_->addressable(decoded->at24, 1)) {
        throw std::invalid_argument("callInContext: no byte of the image is at " +
                                    busAddress(decoded->at24));
    }
    if (insideWatch_ != 0) {
        // Asked before anything moves: the machine is inside the access the watch is deciding, and
        // stepping it to a boundary from here would run the rest of that access under the call.
        throw std::logic_error("callInContext: a routine cannot be called from inside a watch — the "
                               "machine is part-way through the access the watch is deciding; call it "
                               "from an escape");
    }
    // A cycle budget stops the machine wherever the cycle fell; the instruction it was inside finishes
    // first, under the guest's own registers, so the presets below go over the file at the boundary
    // and the routine runs between two of the guest's instructions.
    finishInstruction();

    const snaggletooth::Cpu65816State saved = snes_->cpuState();
    snaggletooth::Cpu65816State       file  = saved;
    for (const ResidentRegister& p : presets) {
        writeRegisterField(file, static_cast<snes::Reg>(p.registerId), p.value);
    }
    snes_->setCpuState(file);
    // The engine's scratch top is the top of work RAM's first page for a guest in emulation mode, where
    // the chip keeps the stack in page one, and $1FFF otherwise.
    const std::uint16_t top = stack == CallStack::Guest ? saved.s : saved.e ? 0x01FF : engineFrame().s;
    const snaggletooth::Standin returns =
        decoded->rtl ? snaggletooth::Standin::Long : snaggletooth::Standin::Near;
    const std::uint64_t before = snes_->state().master;
    const bool returned = snes_->callOnStack(decoded->at24, top, returns, maxInstructions);
    if (!returned && snes_->state().master == before) {
        // Refused with nothing done: the machine is inside a cycle — an access watcher's or an
        // observer's call — or the stack the landing would land on is not memory.
        snes_->setCpuState(saved);
        throw std::logic_error("callInContext: the machine refused the call — it is inside an access "
                               "watcher's or an observer's call, or the stack at " + busAddress(top) +
                               " is not memory the landing can be pushed to");
    }
    // The output is read while the routine's answer is still in the registers it left it in, and the
    // whole file goes back afterwards — with the interrupt lines as they now stand, so an edge the
    // routine's run took is not taken twice.
    if (readOutputs) {
        readOutputs();
    }
    snaggletooth::Cpu65816State after = saved;
    after.nmiPending = snes_->cpuState().nmiPending;
    after.irqLine    = snes_->cpuState().irqLine;
    snes_->setCpuState(after);
}

// ── Escapes and watches ─────────────────────────────────────────────────────────────────────────
// Both are the machine's own: an instruction watch told before an armed instruction runs, an access
// watch told before an armed access takes effect. Either is on the byte an address reaches, so the
// armed sets here are keyed on that byte — every alias of it fires, and a byte is released only when the
// last entry covering it goes. Neither is part of the machine's state, so a fresh machine is re-armed.

namespace {

// The memories an escape may name: where code can be. A register or open bus holds none.
bool holdsCode(snaggletooth::Snes::Space space) {
    return space == snaggletooth::Snes::Space::WorkRam || space == snaggletooth::Snes::Space::CartridgeRom ||
           space == snaggletooth::Snes::Space::SaveRam;
}

}  // namespace

void SnesBackend::installWatchers() {
    if (!snes_) {
        return;
    }
    snes_->setInstructionWatcher(escapeSink_ && !armedEscapes_.empty() ? this : nullptr);
    snes_->setAccessWatcher(watchSink_ && !armedWatches_.empty() ? this : nullptr);
}

void SnesBackend::rearm() {
    for (const ArmedEscape& e : armedEscapes_) {
        snes_->watchInstruction(*snes_address::busAddressOf(map_, e.place), e.standin);
    }
    for (const ArmedWatch& w : armedWatches_) {
        armWatchBytes(w);
    }
    installWatchers();
}

void SnesBackend::setEscapeSink(EscapeSink sink) {
    escapeSink_ = std::move(sink);
    installWatchers();
}

void SnesBackend::armEscape(std::uint32_t address, bool replacesRoutine) {
    const std::optional<snes_address::Decoded> decoded = snes_address::decode(address);
    if (!snes_ || !decoded || decoded->space != snes::Space::Bus) {
        throw std::invalid_argument("an escape is on the console's bus; address " + std::to_string(address) +
                                    " names a memory the CPU does not run code from");
    }
    const snaggletooth::Snes::Physical place = snes_->physical(decoded->at24);
    if (!holdsCode(place.space)) {
        throw std::invalid_argument("an escape names code in work RAM, the cartridge image or its save; " +
                                    busAddress(decoded->at24) + " is " +
                                    (place.space == snaggletooth::Snes::Space::Register ? "a register"
                                                                                        : "open bus"));
    }
    const snaggletooth::Standin standin = !replacesRoutine ? snaggletooth::Standin::None
                                          : decoded->rtl   ? snaggletooth::Standin::Long
                                                           : snaggletooth::Standin::Near;
    for (const ArmedEscape& e : armedEscapes_) {
        if (e.encoded == address) {
            return;  // already watched
        }
        if (e.place == place && e.standin != standin) {
            // One byte, one thing standing at it: the machine answers its fetch one way.
            throw std::invalid_argument("the escape at " + busAddress(decoded->at24) +
                                        " names the same byte as the one at " + busAddress(e.encoded) +
                                        ", which stands a different answer there");
        }
    }
    armedEscapes_.push_back(ArmedEscape{.encoded = address, .place = place, .standin = standin});
    snes_->watchInstruction(decoded->at24, standin);
    installWatchers();
}

void SnesBackend::disarmEscape(std::uint32_t address) {
    const auto at = std::find_if(armedEscapes_.begin(), armedEscapes_.end(),
                                 [address](const ArmedEscape& e) { return e.encoded == address; });
    if (at == armedEscapes_.end()) {
        return;
    }
    const snaggletooth::Snes::Physical place = at->place;
    armedEscapes_.erase(at);
    const bool stillWatched = std::any_of(armedEscapes_.begin(), armedEscapes_.end(),
                                          [&place](const ArmedEscape& e) { return e.place == place; });
    if (!stillWatched && snes_) {
        snes_->unwatchInstruction(*snes_address::busAddressOf(map_, place));
    }
    installWatchers();
}

void SnesBackend::reached(std::uint32_t address) {
    if (!escapeSink_) {
        return;
    }
    const snaggletooth::Snes::Physical place = snes_->physical(address);
    // Taken before reporting: a handler may declare or drop escapes, moving the list.
    std::vector<std::uint32_t> fired;
    for (const ArmedEscape& e : armedEscapes_) {
        if (e.place == place) {
            fired.push_back(e.encoded);
        }
    }
    for (const std::uint32_t encoded : fired) {
        escapeSink_(encoded);
    }
}

void SnesBackend::writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int /*width*/) {
    snaggletooth::Cpu65816State file = snes_->cpuState();
    writeRegisterField(file, static_cast<snes::Reg>(registerId), value);
    snes_->setCpuState(file);
}

void SnesBackend::setWatchSink(WatchSink sink) {
    watchSink_ = std::move(sink);
    installWatchers();
}

void SnesBackend::armWatch(const MemoryRegion& where, bool onRead, bool onWrite) {
    if (!onRead && !onWrite) {
        return;  // nothing asked for
    }
    // The memory the place's first byte is in, as the machine classifies it. A register is one — its read
    // is told after its side effect, which the answer cannot undo; open bus resolves to nothing.
    const std::optional<Resolved>            resolved = resolve(where.at);
    std::optional<snaggletooth::Snes::Space> space;
    if (resolved) {
        switch (resolved->memory) {
            case Memory::WorkRam:   space = snaggletooth::Snes::Space::WorkRam; break;
            case Memory::Cartridge: space = snaggletooth::Snes::Space::CartridgeRom; break;
            case Memory::Save:      space = snaggletooth::Snes::Space::SaveRam; break;
            case Memory::Register:  space = snaggletooth::Snes::Space::Register; break;
            default:                break;  // a memory the bus cannot name
        }
    }
    if (!space) {
        throw std::invalid_argument(
            "a watch is on the console's bus; the picture chip's memories and the audio unit's are reached "
            "through their ports and are not watchable, and open bus reaches nothing (address " +
            std::to_string(where.at) + ")");
    }
    const std::uint64_t span = where.totalBytes();
    for (const ArmedWatch& w : armedWatches_) {
        if (w.encoded == where.at && w.span == span) {
            return;  // already watched
        }
    }
    armedWatches_.push_back(ArmedWatch{
        .encoded = where.at,
        .span    = span,
        .first   = snaggletooth::Snes::Physical{.space = *space, .index = static_cast<std::uint32_t>(resolved->base)},
        .onRead  = onRead,
        .onWrite = onWrite});
    armWatchBytes(armedWatches_.back());
    installWatchers();
}

void SnesBackend::armWatchBytes(const ArmedWatch& w) {
    // Byte by byte through an address that reaches each one, so a place that runs past the end of a
    // bank's window is armed on the memory's next bytes rather than on whatever the next address mirrors.
    for (std::uint64_t i = 0; i < w.span; ++i) {
        const snaggletooth::Snes::Physical byte{.space = w.first.space,
                                                .index = w.first.index + static_cast<std::uint32_t>(i)};
        snes_->watchAccess(*snes_address::busAddressOf(map_, byte), 1, w.onRead, w.onWrite);
    }
}

void SnesBackend::disarmWatch(const MemoryRegion& where, bool onRead, bool onWrite) {
    const std::uint64_t span = where.totalBytes();
    const auto at = std::find_if(armedWatches_.begin(), armedWatches_.end(), [&where, span](const ArmedWatch& w) {
        return w.encoded == where.at && w.span == span;
    });
    if (at == armedWatches_.end()) {
        return;
    }
    const ArmedWatch gone = *at;
    armedWatches_.erase(at);
    releaseWatchBytes(gone, onRead, onWrite);
    installWatchers();
}

void SnesBackend::releaseWatchBytes(const ArmedWatch& gone, bool onRead, bool onWrite) {
    if (!snes_) {
        return;
    }
    for (std::uint64_t i = 0; i < gone.span; ++i) {
        const snaggletooth::Snes::Physical byte{.space = gone.first.space,
                                                .index = gone.first.index + static_cast<std::uint32_t>(i)};
        bool stillRead  = false;
        bool stillWrite = false;
        for (const ArmedWatch& w : armedWatches_) {
            if (w.covers(byte)) {
                stillRead  = stillRead || w.onRead;
                stillWrite = stillWrite || w.onWrite;
            }
        }
        const bool dropRead  = onRead && gone.onRead && !stillRead;
        const bool dropWrite = onWrite && gone.onWrite && !stillWrite;
        if (dropRead || dropWrite) {
            snes_->unwatchAccess(*snes_address::busAddressOf(map_, byte), 1, dropRead, dropWrite);
        }
    }
}

snaggletooth::AccessAnswer SnesBackend::askWatch(std::uint32_t address, AccessKind kind, std::uint8_t value) {
    if (!watchSink_) {
        return snaggletooth::AccessAnswer::proceed();
    }
    const snaggletooth::Snes::Physical place = snes_->physical(address);
    for (const ArmedWatch& w : armedWatches_) {
        if (!w.covers(place) || (kind == AccessKind::Read ? !w.onRead : !w.onWrite)) {
            continue;
        }
        // Copied before asking: the handler may declare or drop watches, moving the list.
        const std::uint32_t base = w.encoded;
        ++insideWatch_;
        struct Leave {
            int& depth;
            ~Leave() { --depth; }
        } leave{insideWatch_};
        const AccessVerdict verdict = watchSink_(base, address, kind, value);
        switch (verdict.kind()) {
            case AccessVerdict::Kind::Proceed: return snaggletooth::AccessAnswer::proceed();
            // A read cannot be prevented: veto() on a read delivers the machine's own byte.
            case AccessVerdict::Kind::Veto:
                return kind == AccessKind::Read ? snaggletooth::AccessAnswer::proceed()
                                                : snaggletooth::AccessAnswer::veto();
            case AccessVerdict::Kind::Instead: return snaggletooth::AccessAnswer::instead(verdict.value());
        }
        return snaggletooth::AccessAnswer::proceed();
    }
    return snaggletooth::AccessAnswer::proceed();
}

snaggletooth::AccessAnswer SnesBackend::read(std::uint32_t address, std::uint8_t value,
                                             snaggletooth::AccessSource, snaggletooth::CycleKind,
                                             std::uint8_t) {
    return askWatch(address, AccessKind::Read, value);
}

snaggletooth::AccessAnswer SnesBackend::write(std::uint32_t address, std::uint8_t value,
                                              snaggletooth::AccessSource, snaggletooth::CycleKind,
                                              std::uint8_t) {
    return askWatch(address, AccessKind::Write, value);
}

// ── Resident driver ─────────────────────────────────────────────────────────────────────────────
// A driver is hosted in an image the engine writes, the same image its routines are placed in: the
// driver's images at their bus addresses under the map its mapper names, the routines already placed kept
// where they are, the header at the map's own site. Its entries are called in the engine's frame on the
// stack the binding names, and the machine idles on the image's own loop between them.

namespace {

// The map a driver's mapper names — the header's own map-mode byte — or nothing for an id this core has no
// map for. The none mapper is one LoROM bank.
std::optional<snaggletooth::CartridgeMap> mapFor(Mapper mapper) {
    if (mapper.isNone() || mapper.id() == 0x20) {
        return snaggletooth::CartridgeMap::LoRom;
    }
    if (mapper.id() == 0x21) {
        return snaggletooth::CartridgeMap::HiRom;
    }
    return std::nullopt;
}

}  // namespace

void SnesBackend::configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                         std::uint32_t stackTop) {
    if (romHosted_) {
        throw std::logic_error(
            "this VM hosts a game's own cartridge; a driver is hosted in an image the engine writes — its "
            "header and every byte in it — and cannot share the game's");
    }
    const std::optional<snaggletooth::CartridgeMap> map = mapFor(mapper);
    if (!map) {
        throw std::invalid_argument("mapper " + std::to_string(mapper.id()) +
                                    " is not one this core maps; name snes::LoRom or snes::HiRom");
    }
    // The stack is in the low 8 KB of work RAM, the page every bank reaches, so the driver's own bank
    // never hides it.
    if (stackTop > 0x1FFF) {
        throw std::invalid_argument("resident driver stack top " + busAddress(stackTop) +
                                    " is not in the low 8 KB of work RAM ($00:0000-$00:1FFF)");
    }

    std::vector<snes_image::Placement> resident;
    resident.reserve(images.size());
    for (const DriverImage& img : images) {
        if (img.bytes.empty()) {
            throw std::invalid_argument("resident driver image at " + busAddress(img.base) + " has no bytes");
        }
        const std::optional<snes_address::Decoded> decoded = snes_address::decode(img.base);
        if (!decoded || decoded->space != snes::Space::Bus || decoded->rtl) {
            throw std::invalid_argument("a driver image's base is a bus address; " + std::to_string(img.base) +
                                        " is not one");
        }
        const std::optional<std::size_t> offset = snes_image::imageOffset(*map, decoded->at24);
        if (!offset) {
            throw std::invalid_argument("a driver image cannot be placed at " + busAddress(decoded->at24) +
                                        ": no byte of the image is at that address on this map");
        }
        if (mapper.isNone() && *offset + img.bytes.size() > 0x8000) {
            throw std::invalid_argument(
                "the driver image at " + busAddress(decoded->at24) +
                " reaches past the first bank, which needs a mapper (snes::LoRom or snes::HiRom); the none "
                "mapper is one 32 KB LoROM bank");
        }
        if (snes_image::overlapsReserved(*map, *offset, img.bytes.size())) {
            throw std::invalid_argument("the driver image at " + busAddress(decoded->at24) +
                                        " would overlap the image's idle loop or its header");
        }
        const auto overlaps = [&](const snes_image::Placement& p) {
            const std::size_t theirs = *snes_image::imageOffset(*map, p.origin);
            return *offset < theirs + p.bytes.size() && theirs < *offset + img.bytes.size();
        };
        for (const snes_image::Placement& p : resident) {
            if (overlaps(p)) {
                throw std::invalid_argument("the driver images at " + busAddress(decoded->at24) + " and " +
                                            busAddress(p.origin) + " overlap");
            }
        }
        for (const snes_image::Placement& p : placed_) {
            if (!snes_image::imageOffset(*map, p.origin)) {
                throw std::invalid_argument("the routine placed at " + busAddress(p.origin) +
                                            " has no byte of the image on the map this driver names");
            }
            if (overlaps(p)) {
                throw std::invalid_argument("the driver image at " + busAddress(decoded->at24) +
                                            " would overlap the routine placed at " + busAddress(p.origin));
            }
        }
        resident.push_back(snes_image::Placement{
            .origin = decoded->at24, .bytes = std::vector<std::uint8_t>(img.bytes.begin(), img.bytes.end())});
    }

    // A fresh machine on the image, as a cartridge put in the slot and powered on is.
    map_              = *map;
    resident_         = std::move(resident);
    rom_              = snes_image::buildImage(map_, everything());
    imageBuilt_       = true;
    residentStackTop_ = stackTop == 0 ? engineFrame().s : static_cast<std::uint16_t>(stackTop);
    residentHosted_   = true;
    emplaceMachine();
}

std::uint64_t SnesBackend::callResident(std::uint32_t entry, std::span<const ResidentRegister> presets,
                                        std::uint64_t maxCpuCycles) {
    if (!residentHosted_) {
        throw std::logic_error("callResident: no driver is hosted on this machine");
    }
    const std::optional<snes_address::Decoded> decoded = snes_address::decode(entry);
    if (!decoded || decoded->space != snes::Space::Bus) {
        throw std::invalid_argument("callResident: the entry " + std::to_string(entry) +
                                    " is not a bus address");
    }
    // The engine's frame on the driver's stack, the presets over it, and the idle loop as the landing.
    snaggletooth::Cpu65816State file = engineFrame();
    file.s = residentStackTop_;
    for (const ResidentRegister& p : presets) {
        writeRegisterField(file, static_cast<snes::Reg>(p.registerId), p.value);
    }
    seatOnIdleLoop(file);
    snes_->setCpuState(file);
    // The cap is in master cycles and the machine's guard is in instructions. An instruction takes at
    // least two CPU cycles of at least six master cycles each, so a routine that returns inside the cap
    // runs at most a twelfth of it in instructions; the guard trips only for a routine that never returns.
    const std::size_t guard = static_cast<std::size_t>(maxCpuCycles / 12 + 1);
    const snaggletooth::Standin returns =
        decoded->rtl ? snaggletooth::Standin::Long : snaggletooth::Standin::Near;
    const std::uint64_t before   = snes_->state().master;
    const bool          returned = snes_->callOnStack(decoded->at24, file.s, returns, guard);
    const std::uint64_t spent    = snes_->state().master - before;
    if (!returned && spent == 0) {
        throw std::logic_error("callResident: the machine refused the call at " + busAddress(decoded->at24) +
                               " — no byte of the image is at that address");
    }
    drainAudio();  // what the sound chip made during the call reaches the sink
    return spent;
}

void SnesBackend::beginContinuous(std::uint32_t entry) {
    if (!snes_) {
        throw std::logic_error("beginContinuous: this machine holds no code to run (host a driver first)");
    }
    const std::optional<snes_address::Decoded> decoded = snes_address::decode(entry);
    if (!decoded || decoded->space != snes::Space::Bus) {
        throw std::invalid_argument("beginContinuous: the entry " + std::to_string(entry) +
                                    " is not a bus address");
    }
    // The engine's frame with the program counter at the entry and nothing pushed: the program runs from
    // there for as long as the budgets it is given last.
    snaggletooth::Cpu65816State file = engineFrame();
    if (residentHosted_) {
        file.s = residentStackTop_;
    }
    file.pc  = static_cast<std::uint16_t>(decoded->at24 & 0xFFFF);
    file.pbr = static_cast<std::uint8_t>(decoded->at24 >> 16);
    snes_->setCpuState(file);
}

}  // namespace retropp::vm

namespace retropp::detail {

// The SNES core; the region follows the cartridge, so the platform argument is not read.
std::unique_ptr<vm::VmBackend> snesCore(VMPlatform /*platform*/) {
    return std::make_unique<vm::SnesBackend>();
}

}  // namespace retropp::detail
