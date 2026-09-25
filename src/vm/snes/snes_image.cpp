#include "src/vm/snes/snes_image.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace retropp::vm::snes_image {

namespace {

// The header's site in a LoROM image, and its extent: the 32 bytes of the header and the 32 of the
// vectors after it.
constexpr std::size_t kHeaderSite  = 0x7FC0;
constexpr std::size_t kHeaderBytes = 0x40;

// The idle loop as image bytes: `BRA *`, two bytes at the offset kIdleLoop maps to in the first bank.
constexpr std::size_t kIdleSite = 0x7FB0;
constexpr std::uint8_t kIdleBytes[] = {0x80, 0xFE};

// How far past the image an address may reach while its offset is still computed as a straight map:
// the largest LoROM image, so no placement below it is mirrored onto a smaller chip.
constexpr std::size_t kAddressSpaceBytes = 0x400000;

}  // namespace

std::optional<std::size_t> imageOffset(snaggletooth::CartridgeMap map, std::uint32_t address) noexcept {
    return snaggletooth::romOffset(map, address, kAddressSpaceBytes);
}

bool overlapsReserved(std::size_t at, std::size_t bytes) noexcept {
    const auto crosses = [at, bytes](std::size_t site, std::size_t extent) {
        return at < site + extent && site < at + bytes;
    };
    return crosses(kIdleSite, sizeof kIdleBytes) || crosses(kHeaderSite, kHeaderBytes);
}

std::uint32_t nextFreeAddress(snaggletooth::CartridgeMap map, std::span<const Placement> placements,
                              std::size_t bytes) {
    // Walk the candidate forward past whatever it collides with until nothing does: each collision
    // moves it to the end of the thing it hit, and a run that would leave a bank's code window moves it
    // to the next bank's. Every address is an image offset here, and the answer is turned back into the
    // bus address that reads it.
    std::size_t at = *imageOffset(map, kArenaStart);
    for (bool moved = true; moved;) {
        moved = false;
        if (overlapsReserved(at, bytes)) {
            at    = kHeaderSite + kHeaderBytes;
            moved = true;
        }
        for (const Placement& p : placements) {
            const std::size_t theirs = *imageOffset(map, p.origin);
            if (at < theirs + p.bytes.size() && theirs < at + bytes) {
                at    = theirs + p.bytes.size();
                moved = true;
            }
        }
    }
    return *snaggletooth::romAddress(map, at);
}

std::vector<std::uint8_t> buildImage(snaggletooth::CartridgeMap map,
                                     std::span<const Placement> placements) {
    std::size_t highest = kMinimumImageBytes;
    for (const Placement& p : placements) {
        const std::optional<std::size_t> at = imageOffset(map, p.origin);
        highest = std::max(highest, *at + p.bytes.size());
    }
    std::vector<std::uint8_t> image(std::bit_ceil(highest), 0);

    // The header: a title, the map byte, no chipset, the size code (1 KB shifted left by it), no save,
    // one country, and the checksum pair. The pair is written agreeing first, so the sum of the image is
    // the same whatever the pair holds, and then written as the sum it produced.
    std::uint8_t* header = image.data() + kHeaderSite;
    std::memcpy(header, "POLYRHYTHM ROUTINES  ", 21);
    header[0x15] = map == snaggletooth::CartridgeMap::LoRom ? 0x20 : 0x21;
    header[0x16] = 0x00;
    header[0x17] = static_cast<std::uint8_t>(std::countr_zero(image.size() >> 10));
    header[0x18] = 0x00;
    header[0x19] = 0x01;
    header[0x1C] = 0xFF;
    header[0x1D] = 0xFF;
    header[0x1E] = 0x00;
    header[0x1F] = 0x00;
    // Every vector points at the idle loop: nothing in the image runs until a call seats the program
    // counter, and an interrupt no program enabled has somewhere harmless to land.
    for (const std::size_t vector : {0x24u, 0x26u, 0x28u, 0x2Au, 0x2Eu, 0x34u, 0x36u, 0x38u, 0x3Au,
                                     0x3Cu, 0x3Eu}) {
        header[vector]     = static_cast<std::uint8_t>(kIdleLoop & 0xFF);
        header[vector + 1] = static_cast<std::uint8_t>((kIdleLoop >> 8) & 0xFF);
    }
    std::memcpy(image.data() + kIdleSite, kIdleBytes, sizeof kIdleBytes);

    for (const Placement& p : placements) {
        const std::optional<std::size_t> at = imageOffset(map, p.origin);
        std::copy(p.bytes.begin(), p.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(*at));
    }

    std::uint16_t sum = 0;
    for (const std::uint8_t b : image) {
        sum = static_cast<std::uint16_t>(sum + b);
    }
    header[0x1C] = static_cast<std::uint8_t>(~sum & 0xFF);
    header[0x1D] = static_cast<std::uint8_t>((~sum >> 8) & 0xFF);
    header[0x1E] = static_cast<std::uint8_t>(sum & 0xFF);
    header[0x1F] = static_cast<std::uint8_t>((sum >> 8) & 0xFF);
    return image;
}

}  // namespace retropp::vm::snes_image
