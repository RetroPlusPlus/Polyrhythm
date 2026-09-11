// A cartridge image the tests author, byte for byte.
//
// SameBoy's buffer loader validates nothing — no logo, no checksum — and reads only the
// cartridge-type and ROM-size header fields to pick a mapper and size the image. So a usable
// cartridge is a planted pattern plus those two bytes, and no third-party ROM is needed anywhere.
//
// An authored image is also the only one with ground truth. Against a real cartridge the assertions
// could only ratify whatever bytes happened to be there; here the planted pattern is the contract.
#ifndef RETROPP_TESTS_AUTHORED_CARTRIDGE_H
#define RETROPP_TESTS_AUTHORED_CARTRIDGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace retropp::testing {

inline constexpr std::size_t kSmallestCartridge = 0x8000;  // 32 KiB, the smallest a cartridge can be
inline constexpr std::uint8_t kRomOnly = 0x00;             // cartridge type: no mapper
inline constexpr std::uint8_t kMbc3    = 0x10;             // cartridge type: MBC3
inline constexpr std::uint8_t kMbc3RamBattery = 0x13;      // cartridge type: MBC3 + RAM + BATTERY

// RAM-size header bytes. A cartridge that keeps anything needs RAM to keep it in, so a battery
// cartridge type pairs with one of these; the default is a cartridge with no RAM at all.
inline constexpr std::uint8_t kNoRam  = 0x00;
inline constexpr std::uint8_t kRam8K  = 0x02;

// A zero-filled cartridge of `bytes`, headered for `cartridgeType`. The ROM-size header byte is
// log2(bytes / 32 KiB), which is the form SameBoy reads.
inline std::vector<std::uint8_t> authorCartridge(std::size_t bytes,
                                                 std::uint8_t cartridgeType = kRomOnly,
                                                 std::uint8_t ramSizeByte   = kNoRam) {
    std::vector<std::uint8_t> rom(bytes, 0x00);
    std::uint8_t sizeByte = 0;
    for (std::size_t s = bytes; s > kSmallestCartridge; s >>= 1) {
        ++sizeByte;
    }
    rom[0x0147] = cartridgeType;
    rom[0x0148] = sizeByte;
    rom[0x0149] = ramSizeByte;
    return rom;
}

}  // namespace retropp::testing

#endif  // RETROPP_TESTS_AUTHORED_CARTRIDGE_H
