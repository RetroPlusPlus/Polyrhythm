#ifndef RETROPP_SRC_VM_WIDE_MATH_H
#define RETROPP_SRC_VM_WIDE_MATH_H

// A multiply whose product is carried in 128 bits before it is divided back down — what pacing a
// machine against the wall clock needs once the clock is a ratio: 236'250'000 / 11 Hz times a
// quarter second times a speed factor passes 64 bits, and the answer still fits them.
//
// INTERNAL — under src/vm/, never include/retropp/.

#include <cstdint>

namespace retropp::vm {

struct WideQuotient {
    std::uint64_t quotient;
    std::uint64_t remainder;
};

// (a x b + addend) / divisor, exactly. The caller guarantees the quotient fits 64 bits — true
// whenever a x b + addend is under divisor x 2^64 — and a nonzero divisor.
[[nodiscard]] constexpr WideQuotient mulAddDiv(std::uint64_t a, std::uint64_t b, std::uint64_t addend,
                                               std::uint64_t divisor) noexcept {
    constexpr std::uint64_t kLow = 0xFFFF'FFFFull;
    const std::uint64_t a0 = a & kLow;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & kLow;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0;
    const std::uint64_t p01 = a0 * b1;
    const std::uint64_t p10 = a1 * b0;
    const std::uint64_t p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & kLow) + (p10 & kLow);
    std::uint64_t       lo  = (p00 & kLow) | (mid << 32);
    std::uint64_t       hi  = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    lo += addend;
    if (lo < addend) {
        ++hi;
    }
    // Long division of hi:lo, one bit at a time. `remainder` stays under the divisor, so the bit
    // shifted out of its top is the only place a 65th bit can appear.
    std::uint64_t remainder = hi;
    std::uint64_t quotient  = 0;
    for (int bit = 63; bit >= 0; --bit) {
        const bool carried = (remainder >> 63) != 0;
        remainder          = (remainder << 1) | ((lo >> bit) & 1u);
        if (carried || remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1ull << bit;
        }
    }
    return WideQuotient{.quotient = quotient, .remainder = remainder};
}

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_WIDE_MATH_H
