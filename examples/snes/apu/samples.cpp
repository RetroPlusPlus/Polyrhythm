#include "samples.h"

namespace demo {

BrrBlock loopingBlock(const std::array<int, 16>& samples) {
    // The header, bit by bit: the shift in the top four (12), the filter in the next two (0), then the
    // loop flag and the end flag. $C3.
    constexpr std::uint8_t kShift  = 12;
    constexpr std::uint8_t kFilter = 0;
    constexpr std::uint8_t kLoop   = 0x02;
    constexpr std::uint8_t kEnd    = 0x01;
    BrrBlock block{};
    block[0] = static_cast<std::uint8_t>((kShift << 4) | (kFilter << 2) | kLoop | kEnd);
    // Two samples a byte, the first in the high four bits, each as a four-bit two's-complement value.
    for (std::size_t i = 0; i < 8; ++i) {
        const auto high = static_cast<std::uint8_t>(samples[2 * i] & 0x0F);
        const auto low  = static_cast<std::uint8_t>(samples[2 * i + 1] & 0x0F);
        block[1 + i]    = static_cast<std::uint8_t>((high << 4) | low);
    }
    return block;
}

const std::array<Wave, 4>& waves() {
    static const std::array<Wave, 4> all{{
        {"SQR", loopingBlock({7, 7, 7, 7, 7, 7, 7, 7, -7, -7, -7, -7, -7, -7, -7, -7})},
        {"SAW", loopingBlock({-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7})},
        {"TRI", loopingBlock({-7, -5, -3, -1, 1, 3, 5, 7, 7, 5, 3, 1, -1, -3, -5, -7})},
        {"PLS", loopingBlock({7, 7, 7, 7, -7, -7, -7, -7, -7, -7, -7, -7, -7, -7, -7, -7})},
    }};
    return all;
}

}  // namespace demo
