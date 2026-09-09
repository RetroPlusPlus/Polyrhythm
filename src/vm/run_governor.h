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

namespace retropp::vm {

class RunGovernor {
public:
    // The largest numerator or denominator a factor may carry. The bound is what keeps the owed
    // arithmetic provably inside 64 bits for any machine clock (clockHz × fold-cap × factor), and
    // {1024, 1} is far past any speed a running game survives.
    static constexpr std::uint32_t kMaxFactorTerm = 1024;

    // The most wall time one accrual folds. The stepping loop calls owedThrough at least once per
    // park interval, so the cap never binds in normal operation; it binds only after the loop was
    // starved longer than this, where the excess time is dropped rather than owed — a machine that
    // far behind catches up by a bounded burst, not a spiral (the run loop's own late-frame
    // re-anchor is this same answer one level up).
    static constexpr std::chrono::nanoseconds kMaxFold{250'000'000};

    explicit RunGovernor(std::uint32_t clockHz) : clockHz_(clockHz) {
        if (clockHz == 0) {
            throw std::invalid_argument("RunGovernor: the machine's clock rate is zero");
        }
        // One fold's numerator is clockHz × kMaxFold × num, and it must fit 64 bits at the worst
        // factor. The bound is ~72 GHz-per-1024ths — every console clock the engine will ever host
        // is orders of magnitude under it; the guard exists so an impossible clock fails loudly
        // here rather than silently wrapping in a fold.
        constexpr std::uint64_t kMaxClockHz =
            UINT64_MAX / (static_cast<std::uint64_t>(kMaxFold.count()) * kMaxFactorTerm);
        static_assert(kMaxClockHz > 33'000'000, "the guard must clear every hosted console clock");
        if (clockHz > kMaxClockHz) {
            throw std::invalid_argument("RunGovernor: the clock rate overflows the owed arithmetic");
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
    // under the factor as it stands: elapsed × clockHz × num / den, integer-exact, remainder kept
    // against the next fold. A paused factor accrues nothing and re-anchors, so unpausing owes
    // nothing for the paused span.
    [[nodiscard]] std::uint64_t owedThrough(std::chrono::steady_clock::time_point now) {
        const auto [num, den] = factor();
        // The carry is a fraction of one cycle held as numerator / (1e9 × den-it-accrued-under).
        // A den change re-denominates it; the truncation is under one billionth of a cycle, once
        // per change.
        if (den != carryDen_) {
            carryNum_ = carryNum_ * den / carryDen_;
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
        const std::uint64_t acc =
            carryNum_ + static_cast<std::uint64_t>(clockHz_) *
                            static_cast<std::uint64_t>(elapsed.count()) * num;
        const std::uint64_t scale = 1'000'000'000ull * den;
        owed_ += acc / scale;
        carryNum_ = acc % scale;
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
        // What one fold's worth of wall time accrues. The constructor's clock guard is what makes
        // this product safe at any factor, and it is also the bound that keeps `want` below in
        // range: a deficit within it costs at most this many cycle-nanoseconds.
        const std::uint64_t perFold = static_cast<std::uint64_t>(clockHz_) *
                                      static_cast<std::uint64_t>(kMaxFold.count()) * num /
                                      (1'000'000'000ull * den);
        if (deficit > perFold) {
            return kMaxFold;
        }
        // Both sides in cycle-nanoseconds under this denominator: what the deficit costs, less what
        // the carried remainder already covers. The carry is under one cycle and the deficit is at
        // least one, so the subtraction stays positive.
        const std::uint64_t want = deficit * 1'000'000'000ull * den;
        const std::uint64_t have = carryNum_ * den / carryDen_;
        const std::uint64_t rate = static_cast<std::uint64_t>(clockHz_) * num;
        const std::uint64_t ns   = (want - have + rate - 1) / rate;  // wake owing it, never short
        const auto          cap  = static_cast<std::uint64_t>(kMaxFold.count());
        return std::chrono::nanoseconds{
            static_cast<std::chrono::nanoseconds::rep>(ns < cap ? ns : cap)};
    }

private:
    std::uint32_t clockHz_;
    // {num, den} in one word: a reader sees a pair that was set together, never halves of two.
    std::atomic<std::uint64_t> packed_{(1ull << 32) | 1ull};

    std::chrono::steady_clock::time_point anchor_{};  // stepping thread only, from here down
    std::uint64_t owed_     = 0;
    std::uint64_t carryNum_ = 0;
    std::uint64_t carryDen_ = 1;
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_RUN_GOVERNOR_H
