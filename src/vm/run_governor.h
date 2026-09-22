#ifndef RETROPP_SRC_VM_RUN_GOVERNOR_H
#define RETROPP_SRC_VM_RUN_GOVERNOR_H

// The time governor a continuously-running machine paces against: how many of its own cycles the
// machine owes for the wall time that has passed, at the platform's speed times a rational factor.
//
// The arithmetic is exact. Owed cycles accumulate as integers with the sub-cycle remainder carried,
// never rounded — the same discipline TimingProfile::cyclesForTick keeps for tick budgets — so a
// machine paced against this governor holds the hardware cadence over any duration instead of
// drifting. The factor {num, den} is exact too: {1, 1} is the platform's own speed, {2, 1} double,
// {1, 2} half, and {0, den} owes nothing — pause is the degenerate case, not a second mechanism.
//
// A machine's clock is itself a ratio — the 60 Hz SNES master clock is 236'250'000 / 11 Hz — so it
// is held as numerator and divisor, and every product is carried in 128 bits before it is divided
// back down (wide_math.h). At a divisor of 1 each answer is the number the 64-bit arithmetic gave.
//
// One thread accrues, any thread steers. owedThrough(), timeUntilOwed() and restart() belong to the
// stepping thread; setFactor() and factor() are wait-free from anywhere (the pair is packed into one
// atomic word, so a torn {num, den} is unrepresentable).
//
// INTERNAL — under src/vm/, never include/retropp/.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/vm/wide_math.h"

namespace retropp::vm {

class RunGovernor {
public:
    // The largest numerator or denominator a factor may carry. The bound is what keeps the fold's
    // scale (1e9 × den × the clock's divisor) inside 64 bits, and {1024, 1} is far past any speed a
    // running game survives.
    static constexpr std::uint32_t kMaxFactorTerm = 1024;

    // The most wall time one accrual folds. The stepping loop calls owedThrough at least once per
    // park interval, so the cap never binds in normal operation; it binds only after the loop was
    // starved longer than this, where the excess time is dropped rather than owed — a machine that
    // far behind catches up by a bounded burst, not a spiral (the run loop's own late-frame
    // re-anchor is this same answer one level up).
    static constexpr std::chrono::nanoseconds kMaxFold{250'000'000};

    // The largest clock divisor. 1e9 x den x divisor is the scale every fold divides by, and it has
    // to fit 64 bits at the largest denominator a factor may carry.
    static constexpr std::uint32_t kMaxClockDivisor = 18'014'398;

    explicit RunGovernor(std::uint32_t clockHz) : RunGovernor(clockHz, 1u) {}

    // A clock of clockHzNumerator / clockHzDivisor cycles a second.
    RunGovernor(std::uint32_t clockHzNumerator, std::uint32_t clockHzDivisor)
        : clockNum_(clockHzNumerator), clockDiv_(clockHzDivisor) {
        if (clockHzNumerator == 0) {
            throw std::invalid_argument("RunGovernor: the machine's clock rate is zero");
        }
        if (clockHzDivisor == 0) {
            throw std::invalid_argument("RunGovernor: the machine's clock divisor is zero");
        }
        static_assert(1'000'000'000ull * kMaxFactorTerm <= UINT64_MAX / kMaxClockDivisor,
                      "the fold's scale must fit 64 bits at the largest denominator and divisor");
        if (clockHzDivisor > kMaxClockDivisor) {
            throw std::invalid_argument("RunGovernor: the clock divisor overflows the owed arithmetic");
        }
    }

    // Steer the factor. Throws for a zero denominator (no rate is a fraction of nothing) and for a
    // term past the bound. {0, den} is pause.
    void setFactor(std::uint32_t num, std::uint32_t den) {
        if (den == 0) {
            throw std::invalid_argument("speed: the denominator is zero");
        }
        if (num > kMaxFactorTerm || den > kMaxFactorTerm) {
            throw std::invalid_argument("speed: a factor term is past " +
                                        std::to_string(kMaxFactorTerm));
        }
        packed_.store((static_cast<std::uint64_t>(num) << 32) | den, std::memory_order_release);
    }

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> factor() const {
        const std::uint64_t p = packed_.load(std::memory_order_acquire);
        return {static_cast<std::uint32_t>(p >> 32), static_cast<std::uint32_t>(p)};
    }

    // Begin (or resume) an episode at `now`: owed returns to zero and time before `now` accrues
    // nothing. The sub-cycle carry survives — stopping and running again loses no fraction.
    void restart(std::chrono::steady_clock::time_point now) {
        anchor_ = now;
        owed_   = 0;
    }

    // Total cycles owed through `now` for this episode. Folds the wall time since the last call
    // under the factor as it stands: elapsed × clock × num / den, integer-exact, remainder kept
    // against the next fold. A paused factor accrues nothing and re-anchors, so unpausing owes
    // nothing for the paused span.
    [[nodiscard]] std::uint64_t owedThrough(std::chrono::steady_clock::time_point now) {
        const auto [num, den] = factor();
        // The carry is a fraction of one cycle held as numerator / (1e9 × den-it-accrued-under ×
        // the clock's divisor). A den change re-denominates it; the truncation is under one
        // billionth of a cycle, once per change.
        if (den != carryDen_) {
            carryNum_ = mulAddDiv(carryNum_, den, 0, carryDen_).quotient;
            carryDen_ = den;
        }
        std::chrono::nanoseconds elapsed = now - anchor_;
        anchor_ = now;
        if (num == 0 || elapsed.count() <= 0) {
            return owed_;  // paused owes nothing, and time never flows backward into a debt
        }
        if (elapsed > kMaxFold) {
            elapsed = kMaxFold;  // the excess is dropped, not owed — see kMaxFold
        }
        const std::uint64_t scale = 1'000'000'000ull * den * clockDiv_;
        const WideQuotient  fold =
            mulAddDiv(static_cast<std::uint64_t>(clockNum_) * num,
                      static_cast<std::uint64_t>(elapsed.count()), carryNum_, scale);
        owed_ += fold.quotient;
        carryNum_ = fold.remainder;
        return owed_;
    }

    // Wall time until the machine is owed `cycles` in total, at the factor as it stands — the owed
    // arithmetic run backwards. A stepping loop parks on this: a step lands a whole frame of the
    // machine's own time at once, so the wait is what the wall clock still owes for it, and a park
    // measured this way ends when the next step is due rather than at a poll boundary. Cycles
    // already owed are no wait at all. The answer is bounded by kMaxFold — a paused factor never
    // reaches the target, and neither does a target further out than one fold, so both report the
    // bound and a caller's park stays finite. Stepping thread only, like owedThrough.
    [[nodiscard]] std::chrono::nanoseconds timeUntilOwed(std::uint64_t cycles) const {
        if (cycles <= owed_) {
            return std::chrono::nanoseconds::zero();
        }
        const auto [num, den] = factor();
        if (num == 0) {
            return kMaxFold;
        }
        const std::uint64_t deficit = cycles - owed_;
        const std::uint64_t scale   = 1'000'000'000ull * den * clockDiv_;
        const std::uint64_t rate    = static_cast<std::uint64_t>(clockNum_) * num;
        const std::uint64_t perFold =
            mulAddDiv(rate, static_cast<std::uint64_t>(kMaxFold.count()), 0, scale).quotient;
        if (deficit > perFold) {
            return kMaxFold;
        }
        // (deficit x scale - have + rate - 1) / rate, with the first product carried wide. `have`
        // is under `scale`, so taking one `scale` out of the product keeps the addend positive.
        const std::uint64_t have = mulAddDiv(carryNum_, den, 0, carryDen_).quotient;
        const std::uint64_t ns =
            mulAddDiv(deficit - 1, scale, scale - have + rate - 1, rate).quotient;  // never short
        const auto cap = static_cast<std::uint64_t>(kMaxFold.count());
        return std::chrono::nanoseconds{
            static_cast<std::chrono::nanoseconds::rep>(ns < cap ? ns : cap)};
    }

private:
    std::uint32_t clockNum_;
    std::uint32_t clockDiv_;
    // {num, den} in one word: a reader sees a pair that was set together, never halves of two.
    std::atomic<std::uint64_t> packed_{(1ull << 32) | 1ull};

    std::chrono::steady_clock::time_point anchor_{};  // stepping thread only, from here down
    std::uint64_t owed_     = 0;
    std::uint64_t carryNum_ = 0;
    std::uint64_t carryDen_ = 1;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_RUN_GOVERNOR_H
