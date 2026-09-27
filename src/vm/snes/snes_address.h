// Internal: the SNES core's addresses, decoded.
//
// A place on this console is a byte, not an address. The bus reaches one byte through many addresses —
// the low 8 KB of work RAM at $7E:0000-$1FFF and at $0000-$1FFF of every system bank, a register at its
// offset in every system bank, the image and the save across every bank the cartridge's map repeats
// them in — and Snaggletooth's `Snes::physical` classifies each of them to the one {memory, offset} it
// reaches. The backend resolves a place there and strides through it there, so a run that crosses a
// bank boundary reads the memory's next bytes and not whatever the next address happens to mirror.
//
// busAddressOf is the way back: one address that reaches a given byte, for the verbs that take an
// address (poke, peekRegister). It answers one alias of the byte, never all of them.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_SNES_SNES_ADDRESS_H
#define RETROPP_SRC_VM_SNES_SNES_ADDRESS_H

#include <cstdint>
#include <optional>

#include "retropp/snes.h"                 // snes::Space — the top byte of an address
#include "snaggletooth/snes/cartridge.h"  // CartridgeMap, romAddress
#include "snaggletooth/snes/snes.h"       // Snes::Physical

namespace retropp::vm::snes_address {

// An address as the backend reads it: the space its top byte names, the byte's 24-bit address within
// that space — a bus address for Space::Bus, an offset into the memory for every other space — and
// whether the address names a routine an RTL leaves (snes::rtl), which rides in the top bit.
struct Decoded {
    snes::Space   space = snes::Space::Bus;
    std::uint32_t at24  = 0;
    bool          rtl   = false;
};

// The space and offset `address` names, or nothing when its top byte names no space on this console. A
// routine an RTL leaves is code, and code is on the bus: the bit is refused with any other space.
[[nodiscard]] constexpr std::optional<Decoded> decode(std::uint32_t address) noexcept {
    const bool          rtl = (address & 0x80000000u) != 0;
    const std::uint32_t tag = (address >> 24) & 0x7Fu;
    if (tag > static_cast<std::uint32_t>(snes::Space::AudioRam) || (rtl && tag != 0)) {
        return std::nullopt;
    }
    return Decoded{.space = static_cast<snes::Space>(tag), .at24 = address & 0x00FFFFFFu, .rtl = rtl};
}

// One bus address that reaches `place` under `map`, or nothing when no address does: open bus (no
// memory), or an image offset past what the map can address. Work RAM answers through bank $7E; the
// image through the banks that carry it without a gap (snaggletooth::romAddress); the save through the
// first bank of its window — LoROM $70-$7D at $0000-$7FFF, HiROM $20-$3F and ExHiROM $80-$BF at
// $6000-$7FFF; a register through bank $00, one of the system banks it answers in.
[[nodiscard]] inline std::optional<std::uint32_t> busAddressOf(snaggletooth::CartridgeMap           map,
                                                               snaggletooth::Snes::Physical place) noexcept {
    using Space = snaggletooth::Snes::Space;
    using Map   = snaggletooth::CartridgeMap;
    const std::uint32_t i = place.index;
    switch (place.space) {
        case Space::WorkRam:
            return 0x7E0000u + i;
        case Space::CartridgeRom:
            return snaggletooth::romAddress(map, i);
        case Space::SaveRam:
            switch (map) {
                case Map::LoRom:   return ((0x70u + i / 0x8000u) << 16) | (i % 0x8000u);
                case Map::HiRom:   return ((0x20u + i / 0x2000u) << 16) | (0x6000u + i % 0x2000u);
                case Map::ExHiRom: return ((0x80u + i / 0x2000u) << 16) | (0x6000u + i % 0x2000u);
            }
            return std::nullopt;
        case Space::Register:
            return i;
        case Space::OpenBus:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace retropp::vm::snes_address

#endif  // RETROPP_SRC_VM_SNES_SNES_ADDRESS_H
