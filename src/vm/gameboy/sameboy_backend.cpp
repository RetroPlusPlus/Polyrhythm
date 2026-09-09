// The SM83 / Game Boy VM backend implementation. The ONE place SM83 / Game Boy machine
// idiom lives (the generic Vm drives this only through the VmBackend interface).
//
// Execution model for PLACED ROUTINES: the backend builds a blank 32 KiB cartridge-shaped image, loads it once,
// and resets the machine. placeRoutine PATCHES a routine's extracted bytes straight into the loaded
// code space (via the ROM direct-access region) at the next free offset in the boot-ROM-safe header
// gap — no reload, no further reset — so RNG state (the seed in HRAM, the rDIV cadence) persists
// across calls. A call sets PC to the routine's entry and SP to a scratch stack top, plants the
// return landing's address on the stack, marshals the inputs, runs to the landing, and reads the
// output back.
#include "src/vm/gameboy/sameboy_backend.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "retropp/gb.h"  // gb::Reg — the SM83 register-id authority (the generic layer stays neutral)
#include "src/vm/gameboy/gb_symbols.h"      // gbHardwareSymbols() — shared with the compile-time bake
#include "src/vm/gameboy/sm83_assembler.h"

namespace retropp::vm {

namespace {

// ── Game Boy code arena ─────────────────────────────────────────────────────────────────────────
// Routines live in 0x0100–0x01FF, the header gap both the DMG and the CGB boot ROM leave mapped to
// cartridge ROM (the proven window — see tests/vm_machine_test.cpp). We bypass the boot ROM
// by setting PC directly, so the rest of low ROM stays boot-overlaid and unusable; this gap is the
// arena.
constexpr std::size_t   kRomSize     = 0x8000;  // 32 KiB ROM-only image (smallest GB cartridge)
constexpr std::uint16_t kRoutineBase = 0x0160;  // routines are placed from here, growing upward
constexpr std::uint16_t kArenaEnd    = 0x0200;  // first address the CGB boot ROM overlays
constexpr std::uint16_t kStackTop    = 0xDFFC;  // SP scratch top (high WRAM); landing planted here

// Game Boy memory map bases for the directly-accessible regions a binding can name.
constexpr std::uint16_t kVRamBase = 0x8000;
constexpr std::uint16_t kWramBase = 0xC000;
constexpr std::uint16_t kOamBase  = 0xFE00;
constexpr std::uint16_t kIoBase   = 0xFF00;
constexpr std::uint16_t kHramBase = 0xFF80;

// ── Run-to-return landing ───────────────────────────────────────────────────────────────────────
// The landing is a stop condition, not part of any program: control reaches it only because the
// engine planted its address on the scratch stack, so a routine's final RET pops it and the run
// ends. It lives in the top two bytes of high RAM, which is never banked and always mapped — the
// address means the same thing whatever cartridge is loaded and whatever the guest has selected.
constexpr std::uint16_t kReturnLanding  = 0xFFFD;
constexpr std::size_t   kLandingOffset  = kReturnLanding - kHramBase;

// advanceClock ticks the divider by running the landing's JR $ idle; JR $ is 12 T-cycles when
// taken, so one frame's 70'224-cycle budget is exactly 5'852 idle steps.
constexpr std::uint64_t kCyclesPerIdleStep = 12;

// Borrows the landing's two bytes for the length of one run: writes the JR-$ self-loop the CPU
// parks on, and puts the guest's own bytes back on the way out. The marker therefore needs no
// permanent home in the image, and an engine-built cartridge and a game's own behave identically.
class SentinelInjection {
public:
    explicit SentinelInjection(std::span<std::uint8_t> hram) : hram_(hram) {
        saved_ = {hram_[kLandingOffset], hram_[kLandingOffset + 1]};
        hram_[kLandingOffset]     = 0x18;  // JR $ (0x18 0xFE)
        hram_[kLandingOffset + 1] = 0xFE;
    }
    ~SentinelInjection() {
        hram_[kLandingOffset]     = saved_[0];
        hram_[kLandingOffset + 1] = saved_[1];
    }
    SentinelInjection(const SentinelInjection&)            = delete;
    SentinelInjection& operator=(const SentinelInjection&) = delete;

private:
    std::span<std::uint8_t>      hram_;
    std::array<std::uint8_t, 2>  saved_{};
};

// Marshal a value into a register field of the pending register file. 8-bit registers set the
// matching byte of their pair; 16-bit locations set the whole pair (or SP / PC).
void writeRegisterField(Registers& regs, gb::Reg reg, std::uint64_t value) {
    const auto lo = static_cast<std::uint16_t>(value & 0xFF);
    const auto wide = static_cast<std::uint16_t>(value & 0xFFFF);
    switch (reg) {
        case gb::Reg::A:  regs.af = static_cast<std::uint16_t>((regs.af & 0x00FF) | (lo << 8)); break;
        case gb::Reg::F:  regs.af = static_cast<std::uint16_t>((regs.af & 0xFF00) | lo);        break;
        case gb::Reg::B:  regs.bc = static_cast<std::uint16_t>((regs.bc & 0x00FF) | (lo << 8)); break;
        case gb::Reg::C:  regs.bc = static_cast<std::uint16_t>((regs.bc & 0xFF00) | lo);        break;
        case gb::Reg::D:  regs.de = static_cast<std::uint16_t>((regs.de & 0x00FF) | (lo << 8)); break;
        case gb::Reg::E:  regs.de = static_cast<std::uint16_t>((regs.de & 0xFF00) | lo);        break;
        case gb::Reg::H:  regs.hl = static_cast<std::uint16_t>((regs.hl & 0x00FF) | (lo << 8)); break;
        case gb::Reg::L:  regs.hl = static_cast<std::uint16_t>((regs.hl & 0xFF00) | lo);        break;
        case gb::Reg::AF: regs.af = wide; break;
        case gb::Reg::BC: regs.bc = wide; break;
        case gb::Reg::DE: regs.de = wide; break;
        case gb::Reg::HL: regs.hl = wide; break;
        case gb::Reg::SP: regs.sp = wide; break;
        case gb::Reg::PC: regs.pc = wide; break;
    }
}

// Decode a possibly bank-qualified cartridge address to a physical offset in the flat cartridge
// image. Bank 0 names a byte of the fixed low 32 KiB directly; a higher bank names one through the
// switchable window, which is the space a long array keeps running through as it crosses boundaries.
// Returns false when the encoding names no cartridge byte.
bool decodeCartridgeAddress(std::uint32_t address, std::uint32_t& physicalOut) {
    const unsigned      bank   = address >> 16;
    const std::uint32_t addr16 = address & 0xFFFF;
    if (bank == 0) {
        if (addr16 > 0x7FFF) {
            return false;
        }
        physicalOut = addr16;
        return true;
    }
    if (addr16 < 0x4000 || addr16 > 0x7FFF) {
        return false;
    }
    physicalOut = bank * 0x4000u + (addr16 - 0x4000u);
    return true;
}

std::uint64_t readRegisterField(const Registers& regs, gb::Reg reg) {
    switch (reg) {
        case gb::Reg::A:  return (regs.af >> 8) & 0xFF;
        case gb::Reg::F:  return regs.af & 0xFF;
        case gb::Reg::B:  return (regs.bc >> 8) & 0xFF;
        case gb::Reg::C:  return regs.bc & 0xFF;
        case gb::Reg::D:  return (regs.de >> 8) & 0xFF;
        case gb::Reg::E:  return regs.de & 0xFF;
        case gb::Reg::H:  return (regs.hl >> 8) & 0xFF;
        case gb::Reg::L:  return regs.hl & 0xFF;
        case gb::Reg::AF: return regs.af;
        case gb::Reg::BC: return regs.bc;
        case gb::Reg::DE: return regs.de;
        case gb::Reg::HL: return regs.hl;
        case gb::Reg::SP: return regs.sp;
        case gb::Reg::PC: return regs.pc;
    }
    return 0;  // unreachable; quiets -Wreturn-type
}

}  // namespace

SameBoyBackend::SameBoyBackend(ConsoleModel model)
    : machine_(model), nextOffset_(kRoutineBase) {
    // A blank cartridge image: header bytes for ROM-ONLY / 32 KiB / no-RAM.
    std::vector<std::uint8_t> rom(kRomSize, 0x00);
    rom[0x0147] = 0x00;  // cartridge type: ROM ONLY (no MBC)
    rom[0x0148] = 0x00;  // ROM size: 32 KiB
    rom[0x0149] = 0x00;  // RAM size: none
    machine_.loadRom(rom);
    machine_.reset();
    romBytes_ = machine_.memory(GbHardwareMemory::Rom).size();
}

void SameBoyBackend::reset() { machine_.reset(); }

void SameBoyBackend::advanceClock(std::uint64_t cycles) {
    if (cycles == 0) {
        return;
    }
    // Tick the free-running divider without executing a routine: park the CPU on the landing's JR $
    // and run idle instructions, so rDIV advances exactly as it does on hardware between routine
    // calls. JR $ is the engine's busy-idle — each iteration is a fixed handful of cycles; run enough
    // to cover `cycles`. The loop never reaches the (unreachable) return address, so it stops at the
    // instruction cap. Only timing/divider state advances — the RNG seed and placed routine bytes are
    // untouched, and the landing's own bytes are restored on the way out; the next beginCall resets
    // PC anyway.
    // (The machine runs headless with PPU rendering disabled — see SameBoyMachine's ctor — so running
    // the CPU for extended periods here is safe.)
    //
    // The register file is put back afterwards, so a machine that was parked in the middle of its own
    // program is still there: idling moves the timing state and leaves everything else where it was.
    const std::size_t instructions = static_cast<std::size_t>(cycles / kCyclesPerIdleStep);
    if (instructions == 0) {
        return;
    }
    const Registers before = machine_.registers();
    Registers regs = before;
    regs.pc = kReturnLanding;
    machine_.setRegisters(regs);
    {
        const SentinelInjection landing{machine_.memory(GbHardwareMemory::Hram)};
        machine_.runToReturn(/*returnAddress=*/0x0000, instructions);  // never reached from JR $
    }
    machine_.setRegisters(before);
}

std::uint32_t SameBoyBackend::placeRoutine(std::span<const std::uint8_t> bytes) {
    if (romHosted_) {
        throw std::logic_error(
            "this VM hosts a game's own cartridge, which has no arena to place a routine into; call "
            "the hosted image's existing entries instead of injecting new code");
    }
    const std::size_t end = static_cast<std::size_t>(nextOffset_) + bytes.size();
    if (end > kArenaEnd) {
        throw std::runtime_error(
            "Game Boy routine arena exhausted (the boot-safe window 0x0160-0x01FF holds the "
            "registered routines; this one does not fit)");
    }
    std::span<std::uint8_t> rom = machine_.memory(GbHardwareMemory::Rom);
    const std::uint16_t base = nextOffset_;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        rom[static_cast<std::size_t>(base) + i] = bytes[i];
    }
    nextOffset_ = static_cast<std::uint16_t>(base + bytes.size());
    return base;
}

void SameBoyBackend::loadRom(std::span<const std::uint8_t> rom) {
    if (rom.empty()) {
        throw std::invalid_argument("the cartridge image has no bytes");
    }
    if (residentConfigured_) {
        throw std::logic_error(
            "this VM already hosts an engine-built cartridge; the engine writes that image's header "
            "and places its own content into the gaps, so a game's cartridge cannot share it");
    }
    // SameBoy reads the image's own header to pick the mapper and size the cartridge; the engine
    // reads none of it. Reset brings the machine up on the loaded image.
    machine_.loadRom(rom);
    machine_.reset();
    // SameBoy rounds an image up to a power-of-two bank count, so the addressable size is the one it
    // reports back, not the one handed in.
    romBytes_ = machine_.memory(GbHardwareMemory::Rom).size();
    romHosted_ = true;
}

namespace {

// The B register a DMG-compatibility boot hands over: the sum of the header's sixteen title bytes,
// computed only for a Nintendo-licensed image exactly as the firmware computes it (old licensee
// $01, or $33 deferring to new licensee "01" — BootROMs/cgb_boot.asm GetPaletteIndex); anything
// else hands over the cleared value.
std::uint8_t titleChecksum(std::span<const std::uint8_t> rom) {
    if (rom.size() < 0x150) {
        return 0;
    }
    const bool nintendo =
        rom[0x14B] == 0x01 || (rom[0x14B] == 0x33 && rom[0x144] == '0' && rom[0x145] == '1');
    if (!nintendo) {
        return 0;
    }
    std::uint8_t sum = 0;
    for (std::size_t i = 0x134; i <= 0x143; ++i) {
        sum = static_cast<std::uint8_t>(sum + rom[i]);
    }
    return sum;
}

}  // namespace

void SameBoyBackend::bootHostedRom() {
    if (!romHosted_) {
        throw std::logic_error(
            "bootHostedRom: no game cartridge is hosted on this machine (host the image first)");
    }
    // Power-on: reset re-arms the boot overlay and zeroes the machine. Everything after reproduces
    // what the boot firmware leaves, through the machine's own bus so every latch performs — the
    // values are read from the vendored firmware source (BootROMs/dmg_boot.asm's final block;
    // BootROMs/cgb_boot.asm's KEY0 handoff and EmulateDMG's "final values for DMG mode").
    machine_.reset();

    const std::span<const std::uint8_t> rom = machine_.memory(GbHardwareMemory::Rom);
    const bool cgbImage = rom.size() > 0x143 && (rom[0x143] & 0x80) != 0;

    Registers regs{};
    regs.sp = 0xFFFE;
    regs.pc = 0x0100;  // the cartridge's entry point
    if (machine_.model() == ConsoleModel::GameBoyColor) {
        // KEY0 latches the machine's mode only while the boot overlay is mapped, so the mode writes
        // come first. A CGB-flagged image keeps CGB mode; anything else gets DMG compatibility with
        // the firmware's sprite-priority selection.
        if (cgbImage) {
            machine_.busWrite(0xFF4C, 0x80);  // KEY0: CGB mode
        } else {
            machine_.busWrite(0xFF6C, 0x01);  // OPRI: DMG sprite ordering
            machine_.busWrite(0xFF4C, 0x04);  // KEY0: DMG compatibility
        }
        regs.af = 0x1180;
        regs.bc = cgbImage ? 0x0000 : static_cast<std::uint16_t>(titleChecksum(rom) << 8);
        regs.de = cgbImage ? 0xFF56 : 0x0008;
        regs.hl = cgbImage ? 0x000D : 0x007C;
    } else {
        regs.af = 0x01B0;
        regs.bc = 0x0013;
        regs.de = 0x00D8;
        regs.hl = 0x014D;
    }
    machine_.busWrite(0xFF50, 0x01);  // BANK: unmap the overlay — the cartridge answers low ROM now

    // The IO the firmware leaves: LCD on with the background enabled (the PPU keeps frame time from
    // here), the classic palette, the APU powered with the firmware's channel routing, and the
    // VBlank flag pending as the firmware's own last frame wait leaves it. The APU powers on first —
    // a powered-off APU ignores its registers. The jingle channel's frequency registers are not
    // reproduced: they are write-only, the tone has decayed to silence by handoff, and no running
    // program can observe them. The divider's post-firmware phase is not reproduced either — its
    // value at handoff is an accident of firmware duration, not a contract.
    machine_.busWrite(0xFF40, 0x91);  // LCDC
    machine_.busWrite(0xFF47, 0xFC);  // BGP
    machine_.busWrite(0xFF26, 0x80);  // NR52
    machine_.busWrite(0xFF11, 0x80);  // NR11
    machine_.busWrite(0xFF12, 0xF3);  // NR12
    machine_.busWrite(0xFF25, 0xF3);  // NR51
    machine_.busWrite(0xFF24, 0x77);  // NR50
    machine_.busWrite(0xFF0F, 0xE1);  // IF

    machine_.setRegisters(regs);
}

AssembledRoutine SameBoyBackend::assemble(std::string_view source) const {
    // The Game Boy backend's assembler is SM83 (the engine's own, no external toolchain), with the
    // standard hardware-register names predefined. A future console's backend assembles its own ISA.
    return assembleSm83(source, gbHardwareSymbols());
}

int SameBoyBackend::registerWidthBytes(std::uint16_t registerId) const {
    if (registerId > static_cast<std::uint16_t>(gb::Reg::PC)) {
        return 0;  // not an SM83 register
    }
    const bool wide = registerId >= static_cast<std::uint16_t>(gb::Reg::AF);
    return wide ? 2 : 1;
}


bool SameBoyBackend::regionIsAddressable(const MemoryRegion& region) const {
    const std::uint64_t total = region.totalBytes();
    if (total == 0) {
        return false;  // a place spanning no bytes names nothing
    }
    const unsigned      bank   = region.at >> 16;
    const std::uint32_t addr16 = region.at & 0xFFFF;

    // Outside the cartridge: work RAM, IO and high RAM are not banked, so the extent is checked
    // against the end of the one memory the place starts in.
    if (bank == 0 && addr16 > 0x7FFF) {
        std::uint32_t end = 0;
        if (addr16 >= kVRamBase && addr16 <= 0x9FFF) {
            end = 0xA000;
        } else if (addr16 >= kWramBase && addr16 <= 0xDFFF) {
            end = 0xE000;
        } else if (addr16 >= kOamBase && addr16 <= 0xFE9F) {
            end = 0xFEA0;
        } else if (addr16 >= kHramBase && addr16 <= 0xFFFE) {
            end = 0xFFFF;
        } else if (addr16 >= kIoBase && addr16 <= 0xFF7F) {
            end = 0xFF80;
        } else {
            return false;
        }
        return addr16 + total <= end;
    }

    // In the cartridge: decode to a physical offset in the flat image and check the whole extent
    // fits it. An array longer than the switchable window keeps running through the banks above it,
    // which is exactly what the physical space expresses and the encoded base does not.
    std::uint32_t physical = 0;
    if (!decodeCartridgeAddress(region.at, physical)) {
        return false;
    }
    return physical + total <= romBytes_;
}

std::span<std::uint8_t> SameBoyBackend::regionSpanFor(const MemoryRegion& region,
                                                      std::uint32_t index, std::size_t& offsetOut) {
    if (!region.contains(index)) {
        throw std::out_of_range("entry " + std::to_string(index) + " is past the " +
                                std::to_string(region.count) + " this place declares");
    }
    // The stride is applied in DECODED space, below. Applying it to the encoded base instead would
    // be right only until the run leaves the window that base names — the first bank boundary.
    const std::uint64_t stride = static_cast<std::uint64_t>(region.size) * index;
    const unsigned      bank   = region.at >> 16;
    const std::uint32_t addr16 = region.at & 0xFFFF;

    if (bank == 0 && addr16 > 0x7FFF) {
        std::size_t base = 0;
        std::span<std::uint8_t> memory = regionFor(addr16, base);  // work RAM / IO / high RAM
        offsetOut = base + static_cast<std::size_t>(stride);
        return memory;
    }

    std::uint32_t physical = 0;
    if (!decodeCartridgeAddress(region.at, physical)) {
        throw std::out_of_range("Game Boy address " + std::to_string(region.at) +
                                " names no cartridge byte");
    }
    offsetOut = static_cast<std::size_t>(physical) + static_cast<std::size_t>(stride);
    return machine_.memory(GbHardwareMemory::Rom);
}

void SameBoyBackend::readRegion(const MemoryRegion& region, std::uint32_t index,
                                std::span<std::uint8_t> out) {
    if (out.size() != region.size) {
        throw std::invalid_argument("a region read takes exactly one entry's worth of bytes");
    }
    std::size_t offset = 0;
    std::span<std::uint8_t> memory = regionSpanFor(region, index, offset);
    if (offset + out.size() > memory.size()) {
        throw std::out_of_range("this place runs past the end of the memory it starts in");
    }
    std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
}

void SameBoyBackend::writeRegion(const MemoryRegion& region, std::uint32_t index,
                                 std::span<const std::uint8_t> bytes) {
    if (bytes.size() != region.size) {
        throw std::invalid_argument("a region write takes exactly one entry's worth of bytes");
    }
    std::size_t offset = 0;
    std::span<std::uint8_t> memory = regionSpanFor(region, index, offset);
    if (offset + bytes.size() > memory.size()) {
        throw std::out_of_range("this place runs past the end of the memory it starts in");
    }
    std::copy_n(bytes.begin(), bytes.size(),
                memory.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::span<std::uint8_t> SameBoyBackend::regionFor(std::uint32_t address, std::size_t& offsetOut) {
    if (address <= 0x7FFF) {
        offsetOut = address;
        return machine_.memory(GbHardwareMemory::Rom);
    }
    if (address >= kVRamBase && address <= 0x9FFF) {
        offsetOut = address - kVRamBase;
        return machine_.memory(GbHardwareMemory::VRam);
    }
    if (address >= kWramBase && address <= 0xDFFF) {
        offsetOut = address - kWramBase;
        return machine_.memory(GbHardwareMemory::WorkRam);
    }
    if (address >= kOamBase && address <= 0xFE9F) {
        offsetOut = address - kOamBase;
        return machine_.memory(GbHardwareMemory::Oam);
    }
    if (address >= kHramBase && address <= 0xFFFE) {
        offsetOut = address - kHramBase;
        return machine_.memory(GbHardwareMemory::Hram);
    }
    if (address >= kIoBase && address <= 0xFF7F) {
        offsetOut = address - kIoBase;
        return machine_.memory(GbHardwareMemory::Io);
    }
    throw std::out_of_range(
        "Game Boy address " + std::to_string(address) +
        " is not in a directly-accessible region (ROM / WRAM / IO / HRAM)");
}

void SameBoyBackend::plantSentinel(std::uint16_t stackTop) {
    // Plant the sentinel return address on the stack so the routine's RET pops it and the run stops.
    // Through the machine's own bus, so it lands in the work-RAM bank the machine has selected.
    machine_.busWrite(stackTop, static_cast<std::uint8_t>(kReturnLanding & 0xFF));
    machine_.busWrite(static_cast<std::uint16_t>(stackTop + 1),
                      static_cast<std::uint8_t>((kReturnLanding >> 8) & 0xFF));
}

void SameBoyBackend::beginCall(std::uint32_t entry) {
    pending_ = Registers{};
    pending_.pc = static_cast<std::uint16_t>(entry);
    pending_.sp = kStackTop;
    plantSentinel(kStackTop);
}

void SameBoyBackend::writeRegister(std::uint16_t registerId, std::uint64_t value, int /*width*/) {
    writeRegisterField(pending_, static_cast<gb::Reg>(registerId), value);
}

void SameBoyBackend::writeMemory(std::uint32_t address, std::uint64_t value, int width) {
    // Through the same resolver a region write uses, so a word and a range cannot disagree about
    // where an address is.
    std::size_t off = 0;
    std::span<std::uint8_t> region =
        regionSpanFor(MemoryRegion{.at = address, .size = static_cast<std::uint32_t>(width)}, 0, off);
    for (int i = 0; i < width; ++i) {
        region[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);  // little-endian (SM83)
    }
}

void SameBoyBackend::run() {
    machine_.setRegisters(pending_);
    const SentinelInjection landing{machine_.memory(GbHardwareMemory::Hram)};
    machine_.runToReturn(kReturnLanding);
}

std::uint64_t SameBoyBackend::readRegister(std::uint16_t registerId) {
    return readRegisterField(machine_.registers(), static_cast<gb::Reg>(registerId));
}

std::uint64_t SameBoyBackend::readMemory(std::uint32_t address, int width) {
    // Through the same resolver a region read uses — see writeMemory.
    std::size_t off = 0;
    std::span<std::uint8_t> region =
        regionSpanFor(MemoryRegion{.at = address, .size = static_cast<std::uint32_t>(width)}, 0, off);
    std::uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(region[off + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

// ── Audio chain ─────────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::enableAudio(unsigned sampleRate, AudioSampleSink sink) {
    machine_.enableAudio(sampleRate);
    machine_.setSampleSink(std::move(sink));
    audioOvershoot8MHz_ = 0;
}

void SameBoyBackend::beginContinuous(std::uint32_t entry) {
    // Position the machine at the driver's entry with a scratch stack — applied immediately (unlike
    // beginCall, which only stages pending_ for run()). No return sentinel: the driver runs forever;
    // runForCycles stops it on a cycle budget, not a return. Subsequent runForCycles calls continue
    // from wherever the driver left off (its idle loop), sustaining the APU output.
    Registers regs{};
    regs.pc = static_cast<std::uint16_t>(entry);
    regs.sp = kStackTop;
    machine_.setRegisters(regs);
    audioOvershoot8MHz_ = 0;
}

std::uint64_t SameBoyBackend::runForCycles(std::uint64_t cpuCycles) {
    // The TimingProfile CPU unit is 4 MHz T-cycles; SameBoy counts in 8 MHz ticks (GB_run's unit), so
    // a frame's 70'224 T-cycles is 140'448 ticks. Pay back any overshoot the previous run carried, so
    // the long-run production rate tracks real time exactly (no slow drift, no periodic dropped frame).
    const std::uint64_t budget8MHz = cpuCycles * 2;
    if (budget8MHz <= audioOvershoot8MHz_) {
        audioOvershoot8MHz_ -= budget8MHz;  // still paying back a prior overshoot; run nothing this tick
        return 0;
    }
    const std::uint64_t target = budget8MHz - audioOvershoot8MHz_;
    const std::uint64_t ran = machine_.runForCycles(target);
    audioOvershoot8MHz_ = ran - target;  // ran ≥ target (a partial last instruction); carry it forward
    return ran / 2;                       // report in CPU T-cycles
}

// ── Resident driver ───────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::configureResidentImage(std::span<const DriverImage> images, Mapper mapper,
                                            std::uint32_t stackTop) {
    if (romHosted_) {
        throw std::logic_error(
            "this VM hosts a game's own cartridge; the resident-driver path synthesizes an image of "
            "its own — writing its header and placing engine content into the gaps — and cannot "
            "share the game's");
    }
    // Resolve + validate the scratch stack top (0 = the platform default; else must be in work RAM).
    if (stackTop != 0 && (stackTop < kWramBase || stackTop > 0xDFFF)) {
        throw std::invalid_argument("resident driver stack top " + std::to_string(stackTop) +
                                    " is not in work RAM (0xC000-0xDFFF)");
    }
    const std::uint16_t resolvedStack =
        (stackTop == 0) ? kStackTop : static_cast<std::uint16_t>(stackTop);

    // The first freely-placeable byte in the fixed bank-0 region: below it is either boot-ROM-overlaid
    // (DMG 0x0000-0x00FF; CGB 0x0000-0x08FF) or the engine-reserved routine arena (0x0100-0x01FF).
    // A flat address in the switchable half (0x4000-0x7FFF) is the first bank, unaffected.
    const std::uint32_t firstFreeBank0 =
        (machine_.model() == ConsoleModel::GameBoyColor) ? 0x0900u : 0x0200u;

    // Decode each placement to a physical cartridge range; validate; track overlap + the highest end.
    struct Placed {
        std::uint32_t begin;
        std::uint32_t end;
        std::span<const std::uint8_t> bytes;
    };
    std::vector<Placed> placed;
    placed.reserve(images.size());
    std::uint32_t highestEnd = kRomSize;  // a cartridge is at least 32 KiB

    for (const DriverImage& img : images) {
        if (img.bytes.empty()) {
            throw std::invalid_argument("resident driver image has no bytes");
        }
        const unsigned      bankHi = img.base >> 16;
        const std::uint32_t addr16 = img.base & 0xFFFF;
        std::uint32_t       physical = 0;
        if (bankHi == 0) {
            if (addr16 > 0x7FFF) {
                throw std::invalid_argument(
                    "flat driver image base " + std::to_string(img.base) +
                    " is outside the low 32 KiB (0x0000-0x7FFF); use gb::banked for a higher bank");
            }
            physical = addr16;
            if (physical < 0x4000 && physical < firstFreeBank0) {
                throw std::invalid_argument(
                    "driver image base " + std::to_string(img.base) +
                    " falls in the boot-ROM / engine-reserved low window (first free bank-0 byte is " +
                    std::to_string(firstFreeBank0) + ")");
            }
        } else {
            if (mapper.isNone()) {
                throw std::invalid_argument(
                    "banked driver placement requires a mapper (DriverBinding.mapper); the none mapper "
                    "is a flat 32 KiB image");
            }
            if (addr16 < 0x4000 || addr16 > 0x7FFF) {
                throw std::invalid_argument(
                    "banked driver image address must be in the switchable window 0x4000-0x7FFF");
            }
            physical = bankHi * 0x4000u + (addr16 - 0x4000u);
        }
        const std::uint32_t begin = physical;
        const std::uint32_t end   = physical + static_cast<std::uint32_t>(img.bytes.size());
        // The engine-reserved routine arena in the header gap (0x0160-0x0200).
        if (begin < kArenaEnd && end > kRoutineBase) {
            throw std::invalid_argument(
                "driver image overlaps the engine-reserved routine arena");
        }
        // Any uploaded-routine arena already claimed on this VM.
        if (begin < nextOffset_ && end > kRoutineBase) {
            throw std::invalid_argument("driver image overlaps the uploaded-routine arena on this VM");
        }
        for (const Placed& p : placed) {
            if (begin < p.end && p.begin < end) {
                throw std::invalid_argument("driver images overlap in the cartridge image");
            }
        }
        placed.push_back({begin, end, img.bytes});
        highestEnd = std::max(highestEnd, end);
    }

    // Size the cartridge to a power-of-two byte count holding the highest placement.
    std::uint32_t totalBytes = kRomSize;
    while (totalBytes < highestEnd) {
        totalBytes <<= 1;
    }
    if (mapper.isNone() && totalBytes > kRomSize) {
        throw std::invalid_argument(
            "driver placement exceeds 32 KiB but no mapper is declared (use gb::Mbc3)");
    }
    // ROM-size header byte: log2(totalBytes / 32 KiB).
    std::uint8_t sizeByte = 0;
    for (std::uint32_t s = totalBytes; s > kRomSize; s >>= 1) {
        ++sizeByte;
    }

    // Build the image: header + preserved routine arena + the placed images.
    std::vector<std::uint8_t> rom(totalBytes, 0x00);
    rom[0x0147] = static_cast<std::uint8_t>(mapper.id());  // cartridge type (0x00 none / 0x10 MBC3)
    rom[0x0148] = sizeByte;                                // ROM size
    rom[0x0149] = 0x00;                                    // RAM size: none
    if (nextOffset_ > kRoutineBase) {  // preserve any uploaded-routine bytes from the current image
        std::span<std::uint8_t> oldRom = machine_.memory(GbHardwareMemory::Rom);
        for (std::uint16_t a = kRoutineBase; a < nextOffset_; ++a) {
            rom[a] = oldRom[a];
        }
    }
    for (const Placed& p : placed) {
        for (std::size_t i = 0; i < p.bytes.size(); ++i) {
            rom[p.begin + i] = p.bytes[i];
        }
    }

    machine_.loadRom(rom);
    machine_.reset();
    romBytes_ = machine_.memory(GbHardwareMemory::Rom).size();
    residentStackTop_ = resolvedStack;
    residentConfigured_ = true;
    audioOvershoot8MHz_ = 0;
}

std::uint64_t SameBoyBackend::callResident(std::uint32_t entry,
                                           std::span<const ResidentRegister> presets,
                                           std::uint64_t maxCpuCycles) {
    if (!residentConfigured_) {
        throw std::logic_error("callResident: configureResidentImage has not run on this backend");
    }
    Registers regs{};
    regs.pc = static_cast<std::uint16_t>(entry);
    regs.sp = residentStackTop_;
    for (const ResidentRegister& p : presets) {
        writeRegisterField(regs, static_cast<gb::Reg>(p.registerId), p.value);
    }
    machine_.setRegisters(regs);
    plantSentinel(residentStackTop_);
    // Run to the routine's return, counting cycles (the SameBoy 8 MHz tick is twice the 4 MHz CPU unit),
    // capped so a driver entry that never returns still terminates. Report back in CPU T-cycles.
    const SentinelInjection landing{machine_.memory(GbHardwareMemory::Hram)};
    const std::uint64_t ran8 = machine_.runToReturnCycles(kReturnLanding, maxCpuCycles * 2);
    return ran8 / 2;
}

void SameBoyBackend::callInContext(std::uint32_t entry, std::span<const ResidentRegister> presets,
                                   CallStack stack, std::size_t maxInstructions,
                                   const std::function<void()>& readOutputs) {
    // A bank-qualified entry names a byte of one bank, and only that bank answers at that address.
    // The guest itself cannot call into a bank it has not selected; selecting one on its behalf
    // would be the engine deciding what the cartridge's mapper is doing.
    const unsigned bank = entry >> 16;
    if (bank != 0) {
        const unsigned live = machine_.mappedRomBank();
        if (live != bank) {
            throw std::logic_error(
                "this routine is in ROM bank " + std::to_string(bank) + ", and bank " +
                std::to_string(live) +
                " is the one mapped; the guest's own code selects the bank it calls into");
        }
    }

    const Registers saved = machine_.registers();

    Registers regs = saved;
    for (const ResidentRegister& p : presets) {
        writeRegisterField(regs, static_cast<gb::Reg>(p.registerId), p.value);
    }
    // Push the landing where the guest's own `call` would push it — through the bus, so the work-RAM
    // bank the guest selected holds — and seat the stack pointer below it. The landing's own bytes
    // stay the guest's throughout: the run leaves when that address is fetched, so whatever is there
    // is never executed. That is what lets a guest whose stack reaches down into those bytes — the
    // stack pointer a boot hands over does — be called into at all.
    const std::uint16_t top = (stack == CallStack::Guest) ? saved.sp : kStackTop;
    machine_.busWrite(static_cast<std::uint16_t>(top - 1),
                      static_cast<std::uint8_t>((kReturnLanding >> 8) & 0xFF));
    machine_.busWrite(static_cast<std::uint16_t>(top - 2),
                      static_cast<std::uint8_t>(kReturnLanding & 0xFF));
    regs.sp = static_cast<std::uint16_t>(top - 2);
    regs.pc = static_cast<std::uint16_t>(entry & 0xFFFFu);
    machine_.setRegisters(regs);

    machine_.runInContext(kReturnLanding, maxInstructions);

    // The output is read while the routine's answer is still in the registers it left it in, and the
    // whole file goes back afterwards: what the routine promised its callers reaches native code
    // through the binding, never through a register left behind.
    if (readOutputs) {
        readOutputs();
    }
    machine_.setRegisters(saved);
}

// ── Escapes ─────────────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::setEscapeSink(EscapeSink sink) {
    escapeSink_ = std::move(sink);
    if (!escapeSink_) {
        machine_.setEscapeSink({});
        return;
    }
    machine_.setEscapeSink([this](std::uint16_t addr16) { onEscapeReached(addr16); });
}

void SameBoyBackend::armEscape(std::uint32_t address, bool replacesRoutine) {
    const unsigned      bank   = address >> 16;
    const std::uint16_t addr16 = static_cast<std::uint16_t>(address & 0xFFFFu);

    // The address a routine's return lands on is the machine's own terminator: escaping there would
    // put a game's code between a routine returning and the call reporting that it did.
    if (addr16 == kReturnLanding) {
        throw std::invalid_argument(
            "escape address " + std::to_string(address) +
            " is where this machine lands when a routine returns; it cannot be escaped");
    }

    for (const ArmedEscape& e : armedEscapes_) {
        if (e.encoded == address) {
            return;  // already watched
        }
    }

    ArmedEscape armed{
        .encoded = address, .addr16 = addr16, .bank = bank, .banked = bank != 0,
        .replaces = replacesRoutine};
    if (replacesRoutine) {
        // The answering kind: RET (the SM83's one-byte return) sits at the entry while this escape is
        // armed, so the one instruction that executes after the sink returns sends control back to
        // whatever called the routine. The displaced byte rides the record and disarm restores it.
        constexpr std::uint8_t kSm83Ret = 0xC9;
        std::size_t off = 0;
        std::span<std::uint8_t> region = regionSpanFor(MemoryRegion{.at = address, .size = 1}, 0, off);
        armed.savedByte = region[off];
        region[off]     = kSm83Ret;
    }
    armedEscapes_.push_back(armed);
    machine_.armEscape(addr16);
}

void SameBoyBackend::disarmEscape(std::uint32_t address) {
    const std::uint16_t addr16 = static_cast<std::uint16_t>(address & 0xFFFFu);
    const auto at = std::find_if(armedEscapes_.begin(), armedEscapes_.end(),
                                 [address](const ArmedEscape& e) { return e.encoded == address; });
    if (at == armedEscapes_.end()) {
        return;
    }
    if (at->replaces) {
        // Put the routine back exactly as it was: the displaced byte returns to its address, through
        // the same resolver that displaced it.
        std::size_t off = 0;
        std::span<std::uint8_t> region = regionSpanFor(MemoryRegion{.at = address, .size = 1}, 0, off);
        region[off] = at->savedByte;
    }
    armedEscapes_.erase(at);

    // Two escapes in different banks decode to the same address in the switchable window, so the
    // machine keeps watching it until the last one naming it is gone.
    const bool stillWatched =
        std::any_of(armedEscapes_.begin(), armedEscapes_.end(),
                    [addr16](const ArmedEscape& e) { return e.addr16 == addr16; });
    if (!stillWatched) {
        machine_.disarmEscape(addr16);
    }
}

void SameBoyBackend::writeLiveRegister(std::uint16_t registerId, std::uint64_t value, int /*width*/) {
    // Read-modify-write of the parked machine's real register file — NOT the pending frame, which
    // only a beginCall applies. The helper masks the value to the register's own width.
    Registers regs = machine_.registers();
    writeRegisterField(regs, static_cast<gb::Reg>(registerId), value);
    machine_.setRegisters(regs);
}

// ── Watches ─────────────────────────────────────────────────────────────────────────────────────

void SameBoyBackend::setWatchSink(WatchSink sink) {
    watchSink_ = std::move(sink);
    if (!watchSink_) {
        machine_.setAccessSinks({}, {});
        return;
    }
    machine_.setAccessSinks(
        [this](std::uint16_t addr16, std::uint8_t data) -> std::uint8_t {
            const AccessVerdict verdict = onWatchedAccess(addr16, AccessKind::Read, data);
            // A read answers with a byte and nothing else — there is no way to not answer, so
            // veto() and proceed() are the same thing here and the surface says so.
            return verdict.kind() == AccessVerdict::Kind::Instead ? verdict.value() : data;
        },
        [this](std::uint16_t addr16, std::uint8_t value,
               std::uint8_t& substitute) -> SameBoyMachine::WriteAction {
            const AccessVerdict verdict = onWatchedAccess(addr16, AccessKind::Write, value);
            switch (verdict.kind()) {
                case AccessVerdict::Kind::Proceed:
                    return SameBoyMachine::WriteAction::Allow;
                case AccessVerdict::Kind::Veto:
                    return SameBoyMachine::WriteAction::Prevent;
                case AccessVerdict::Kind::Instead:
                    substitute = verdict.value();
                    return SameBoyMachine::WriteAction::Substitute;
            }
            return SameBoyMachine::WriteAction::Allow;
        });
}

void SameBoyBackend::armWatch(const MemoryRegion& where, bool onRead, bool onWrite) {
    if (!onRead && !onWrite) {
        return;  // nothing asked for
    }
    const unsigned      bank   = where.at >> 16;
    const std::uint16_t base16 = static_cast<std::uint16_t>(where.at & 0xFFFFu);
    const std::uint64_t span   = where.totalBytes();

    // The hook this machine has sees the CPU's own 16-bit address, so a place is watchable only
    // where the CPU can name every byte of it at once. A place running off the end of the address
    // space, or out of the switchable window it is banked into, is refused naming what it could not
    // reach rather than being watched in part.
    if (span == 0 || static_cast<std::uint64_t>(base16) + span > 0x10000u) {
        throw std::invalid_argument(
            "a watched place must fit the machine's 16-bit address space; this one runs from " +
            std::to_string(base16) + " for " + std::to_string(span) + " bytes");
    }
    if (bank != 0 && (base16 < 0x4000u || static_cast<std::uint64_t>(base16) + span > 0x8000u)) {
        throw std::invalid_argument(
            "a bank-qualified watched place must lie in the switchable window 0x4000-0x7FFF, "
            "where its bank is what the CPU sees; this one runs from " +
            std::to_string(base16) + " for " + std::to_string(span) + " bytes");
    }

    for (const ArmedWatch& w : armedWatches_) {
        if (w.encoded == where.at && w.span == span) {
            return;  // already watched
        }
    }

    const ArmedWatch armed{.encoded = where.at,
                           .base16  = base16,
                           .span    = static_cast<std::uint32_t>(span),
                           .bank    = bank,
                           .banked  = bank != 0,
                           .onRead  = onRead,
                           .onWrite = onWrite};
    armedWatches_.push_back(armed);
    for (std::uint32_t i = 0; i < armed.span; ++i) {
        const auto addr16 = static_cast<std::uint16_t>(armed.base16 + i);
        if (onRead) {
            machine_.armReadWatch(addr16);
        }
        if (onWrite) {
            machine_.armWriteWatch(addr16);
        }
    }
}

void SameBoyBackend::disarmWatch(const MemoryRegion& where, bool onRead, bool onWrite) {
    const std::uint64_t span = where.totalBytes();
    const auto at = std::find_if(armedWatches_.begin(), armedWatches_.end(),
                                 [&where, span](const ArmedWatch& w) {
                                     return w.encoded == where.at && w.span == span;
                                 });
    if (at == armedWatches_.end()) {
        return;
    }
    const ArmedWatch gone = *at;
    armedWatches_.erase(at);
    releaseWatchBytes(gone, onRead, onWrite);
}

void SameBoyBackend::releaseWatchBytes(const ArmedWatch& gone, bool onRead, bool onWrite) {
    for (std::uint32_t i = 0; i < gone.span; ++i) {
        const auto addr16 = static_cast<std::uint16_t>(gone.base16 + i);
        bool       stillRead  = false;
        bool       stillWrite = false;
        for (const ArmedWatch& w : armedWatches_) {
            if (!w.covers(addr16)) {
                continue;
            }
            stillRead  = stillRead || w.onRead;
            stillWrite = stillWrite || w.onWrite;
        }
        if (onRead && gone.onRead && !stillRead) {
            machine_.disarmReadWatch(addr16);
        }
        if (onWrite && gone.onWrite && !stillWrite) {
            machine_.disarmWriteWatch(addr16);
        }
    }
}

AccessVerdict SameBoyBackend::onWatchedAccess(std::uint16_t addr16, AccessKind kind,
                                              std::uint8_t value) {
    if (!watchSink_) {
        return AccessVerdict::proceed();
    }
    const std::uint16_t liveBank = machine_.mappedRomBank();
    for (const ArmedWatch& w : armedWatches_) {
        if (!w.covers(addr16)) {
            continue;
        }
        if (kind == AccessKind::Read ? !w.onRead : !w.onWrite) {
            continue;
        }
        if (w.banked && w.bank != liveBank) {
            continue;
        }
        // Copy before asking: the handler may declare or drop watches, moving this vector. The
        // byte's own encoded address carries the bank it was reached through, so a handler serving
        // a span still knows exactly which byte moved.
        const std::uint32_t base = w.encoded;
        const std::uint32_t at =
            w.banked ? ((static_cast<std::uint32_t>(w.bank) << 16) | addr16) : addr16;
        return watchSink_(base, at, kind, value);
    }
    return AccessVerdict::proceed();
}

void SameBoyBackend::onEscapeReached(std::uint16_t addr16) {
    if (!escapeSink_) {
        return;
    }
    const std::uint16_t liveBank = machine_.mappedRomBank();
    for (const ArmedEscape& e : armedEscapes_) {
        if (e.addr16 != addr16) {
            continue;
        }
        if (e.banked && e.bank != liveBank) {
            continue;
        }
        // Copy before reporting: the handler may declare or drop escapes, moving this vector.
        const std::uint32_t encoded = e.encoded;
        escapeSink_(encoded);
        return;
    }
}

void SameBoyBackend::setFrameSink(FrameSink sink) {
    frameSink_ = std::move(sink);
    if (!frameSink_) {
        machine_.setFrameSink({});
        return;
    }
    // The machine reports the dimensions it drew; the layout is this core's, declared once here. A
    // core whose PPU wrote a different layout would name a different enumerator, and nothing above
    // would convert anything.
    machine_.setFrameSink([this](std::span<const std::uint8_t> pixels, int width, int height) {
        frameSink_(pixels, width, height, GuestPixelFormat::Rgba8888);
    });
}

void SameBoyBackend::setVideoEnabled(bool enabled) { machine_.setVideoEnabled(enabled); }

}  // namespace retropp::vm
