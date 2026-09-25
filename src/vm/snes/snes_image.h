// Internal: the cartridge image the SNES core builds for the routines it places.
//
// A machine that hosts no cartridge of the game's own is a routine machine: it runs code the engine
// placed, in an image the engine wrote. The image is a LoROM cartridge sized to a power of two holding
// every placed byte, with a header the console's own parser accepts, an idle loop the machine parks on
// between calls, and every vector pointing at that loop, so nothing in the image runs until a call seats
// the program counter.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_SNES_SNES_IMAGE_H
#define RETROPP_SRC_VM_SNES_SNES_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "snaggletooth/snes/cartridge.h"  // CartridgeMap

namespace retropp::vm::snes_image {

// Where the idle loop is, as a bus address: the last bytes of the first bank's code, below the header.
// `BRA *` — the machine sits here between calls, and a call's return lands here to end it.
inline constexpr std::uint32_t kIdleLoop = 0x00FFB0;

// Where the arena begins: raw bytes with no origin of their own are placed from here up.
inline constexpr std::uint32_t kArenaStart = 0x008000;

// The smallest image the builder writes, in bytes: one LoROM bank.
inline constexpr std::size_t kMinimumImageBytes = 0x8000;

// One routine the image carries: its bytes, and the bus address of its first byte.
struct Placement {
    std::uint32_t             origin;
    std::vector<std::uint8_t> bytes;
};

// The image offset of a bus address under `map`, when the address is ROM the image could hold; nothing
// for work RAM, a register, open bus, the save window, or an address past what the map can address.
[[nodiscard]] std::optional<std::size_t> imageOffset(snaggletooth::CartridgeMap map,
                                                     std::uint32_t address) noexcept;

// Whether `bytes` bytes at image offset `at` cross the idle loop or the header, which every image keeps.
[[nodiscard]] bool overlapsReserved(std::size_t at, std::size_t bytes) noexcept;

// The first address from kArenaStart up where `bytes` bytes fit without crossing a placement, the idle
// loop or the header — the arena continuing into the next bank's code window when a bank fills. A
// routine with no address of its own lands here.
[[nodiscard]] std::uint32_t nextFreeAddress(snaggletooth::CartridgeMap map,
                                            std::span<const Placement> placements, std::size_t bytes);

// The image holding every placement: sized to a power of two no smaller than kMinimumImageBytes that
// holds the highest placed byte, the header and the idle loop written, the placements copied in.
// Placements are assumed disjoint from each other and from the reserved bytes — placeRoutine refuses
// the overlap before it gets here.
[[nodiscard]] std::vector<std::uint8_t> buildImage(snaggletooth::CartridgeMap map,
                                                   std::span<const Placement> placements);

}  // namespace retropp::vm::snes_image

#endif  // RETROPP_SRC_VM_SNES_SNES_IMAGE_H
