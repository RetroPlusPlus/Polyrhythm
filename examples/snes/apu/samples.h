// The S-DSP's sample format, and the waves this demo plays.
//
// The chip plays BRR — bit-rate-reduced samples: blocks of nine bytes, each holding sixteen samples as
// four-bit values. A block's first byte is its header: how far each four-bit value is shifted to make a
// sample, which of four prediction filters to run the result through, whether this block is the last,
// and whether the voice loops when it is. The other eight bytes hold the sixteen values, two to a byte.
//
// One block that loops on itself is a whole wave: sixteen samples long, played round and round at the
// voice's pitch. That is every sound in this demo.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace demo {

// One BRR block: a header byte and sixteen four-bit samples packed two to a byte.
using BrrBlock = std::array<std::uint8_t, 9>;

// A block from sixteen samples in -8..7, looping on itself: shift 12 so a value fills the chip's range,
// filter 0 so each value stands alone, the loop and end flags both set.
[[nodiscard]] BrrBlock loopingBlock(const std::array<int, 16>& samples);

// A wave: its name as the panel shows it, and the block that is it.
struct Wave {
    std::string_view name;
    BrrBlock         block;
};

// The four waves: a square, a saw, a triangle and a narrow pulse.
[[nodiscard]] const std::array<Wave, 4>& waves();

}  // namespace demo
