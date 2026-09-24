// A stream at one rate becomes a stream at another, by the exact ratio of the two: 32'000 Hz to
// 48'000 Hz is three output frames for every two input frames, 32'000 to 44'100 is 441 for every 320.
// The SNES sound chip produces at 32'000 Hz and nothing else, and the engine's sink runs at one rate for
// the whole chain (retropp/audio.h), so a hosted SNES cartridge's sound passes through here on its way.
//
// Polyphase: one low-pass prototype, designed at the up-sampled rate and split into `upFactor()` phases,
// so an output frame is one dot product of kTapsPerPhase input frames against the phase its position
// selects, and no inserted zero is ever multiplied. The prototype is a Kaiser-windowed sinc whose stop
// band begins at the lower of the two rates' Nyquist frequencies, so nothing folds back into the band.
// The coefficients are fixed-point (Q24) and the sum is 64-bit, so one input produces one output on
// every platform, bit for bit.
//
// The output count is exact: after n input frames exactly ceil(n x up / down) output frames have come
// out, so a second of 32'000 input frames is 48'000 or 44'100 output frames, with no drift, ever. The
// delay is kTapsPerPhase / 2 input frames: 32 at 32'000 Hz, one millisecond.
//
// Equal rates are the identity: every frame passes through untouched.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_SNES_RESAMPLER_H
#define RETROPP_SRC_VM_SNES_RESAMPLER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace retropp::vm {

class RationalResampler {
public:
    // Input frames every output frame is computed from. The stop band's edge sits at the lower
    // Nyquist frequency and the transition band below it is 3.62 x inputRate / kTapsPerPhase wide
    // (the Kaiser estimate for 60 dB): 1'811 Hz at 32'000 Hz, so the pass band reaches 14'189 Hz.
    static constexpr int kTapsPerPhase = 64;

    RationalResampler(unsigned inputRate, unsigned outputRate) {
        const unsigned divisor = std::gcd(inputRate, outputRate);
        up_       = outputRate / divisor;
        down_     = inputRate / divisor;
        identity_ = (up_ == 1 && down_ == 1);
        offset_   = up_;
        if (!identity_) {
            design(inputRate, outputRate);
        }
    }

    [[nodiscard]] unsigned upFactor() const noexcept { return up_; }
    [[nodiscard]] unsigned downFactor() const noexcept { return down_; }

    // Forget every frame seen: the history is silence and the next output is phase zero of the next
    // input, as at construction.
    void reset() noexcept {
        left_.fill(0);
        right_.fill(0);
        head_   = 0;
        offset_ = up_;
    }

    // Feed one input frame; `emit(left, right)` is called for each output frame it completes: none,
    // one or several, as the ratio dictates.
    template <class Emit>
    void push(std::int16_t left, std::int16_t right, Emit&& emit) {
        if (identity_) {
            emit(left, right);
            return;
        }
        head_ = (head_ + 1 == static_cast<std::size_t>(kTapsPerPhase)) ? 0 : head_ + 1;
        left_[head_]                 = left;   // twice, so the newest kTapsPerPhase frames are always
        left_[head_ + kTapsPerPhase] = left;   // contiguous, ending at head_ + kTapsPerPhase
        right_[head_]                 = right;
        right_[head_ + kTapsPerPhase] = right;
        offset_ -= up_;
        while (offset_ < up_) {
            const std::int32_t* phase = taps_.data() + static_cast<std::size_t>(offset_) * kTapsPerPhase;
            std::int64_t        l     = 0;
            std::int64_t        r     = 0;
            for (int j = 0; j < kTapsPerPhase; ++j) {
                const std::size_t at = head_ + static_cast<std::size_t>(kTapsPerPhase - j);
                l += std::int64_t{phase[j]} * left_[at];
                r += std::int64_t{phase[j]} * right_[at];
            }
            emit(toSample(l), toSample(r));
            offset_ += down_;
        }
    }

private:
    static constexpr int kFractionBits = 24;

    // Q24 back to a sample, rounded to nearest and clipped to the 16-bit range.
    [[nodiscard]] static std::int16_t toSample(std::int64_t acc) noexcept {
        const std::int64_t v = (acc + (std::int64_t{1} << (kFractionBits - 1))) >> kFractionBits;
        return static_cast<std::int16_t>(std::clamp<std::int64_t>(v, -32768, 32767));
    }

    // The zeroth-order modified Bessel function, by its series; what the Kaiser window is made of.
    [[nodiscard]] static double besselI0(double x) noexcept {
        double sum  = 1.0;
        double term = 1.0;
        for (int k = 1; k < 200; ++k) {
            const double half = x / (2.0 * k);
            term *= half * half;
            sum += term;
            if (term < sum * 1e-15) {
                break;
            }
        }
        return sum;
    }

    // The prototype and its phases. Every figure here is at the up-sampled rate (inputRate x up_).
    void design(unsigned inputRate, unsigned outputRate) {
        constexpr double kPi         = 3.14159265358979323846;
        constexpr double kBeta       = 5.65326;  // Kaiser: 0.1102 x (60 - 8.7), 60 dB of rejection
        constexpr double kTransition = 3.62186;  // (60 - 8) / (2.285 x 2 pi), in input rates per tap
        const std::size_t n          = static_cast<std::size_t>(kTapsPerPhase) * up_;
        const double      protoRate  = static_cast<double>(inputRate) * up_;
        const double      stop       = 0.5 * static_cast<double>(std::min(inputRate, outputRate));
        const double      transition = kTransition * static_cast<double>(inputRate) / kTapsPerPhase;
        const double      cutoff     = stop - 0.5 * transition;
        const double      center     = 0.5 * static_cast<double>(n - 1);
        const double      i0Beta     = besselI0(kBeta);

        std::vector<double> prototype(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) - center;
            const double x = 2.0 * cutoff * t / protoRate;
            const double sinc = (x == 0.0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double u = t / center;
            const double window = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0Beta;
            prototype[i] = sinc * window;
        }

        // Phase r holds prototype[j x up_ + r] for j = 0..kTapsPerPhase-1, scaled so its taps sum to
        // exactly one in Q24: every phase passes a constant at its own value, and the phases agree.
        taps_.assign(n, 0);
        for (unsigned r = 0; r < up_; ++r) {
            double sum = 0.0;
            for (int j = 0; j < kTapsPerPhase; ++j) {
                sum += prototype[static_cast<std::size_t>(j) * up_ + r];
            }
            std::int32_t* phase   = taps_.data() + static_cast<std::size_t>(r) * kTapsPerPhase;
            std::int64_t  total   = 0;
            int           largest = 0;
            for (int j = 0; j < kTapsPerPhase; ++j) {
                const double scaled = prototype[static_cast<std::size_t>(j) * up_ + r] / sum *
                                      static_cast<double>(std::int64_t{1} << kFractionBits);
                phase[j] = static_cast<std::int32_t>(std::llround(scaled));
                total += phase[j];
                if (std::abs(phase[j]) > std::abs(phase[largest])) {
                    largest = j;
                }
            }
            phase[largest] += static_cast<std::int32_t>((std::int64_t{1} << kFractionBits) - total);
        }
    }

    unsigned                  up_       = 1;
    unsigned                  down_     = 1;
    bool                      identity_ = true;
    std::vector<std::int32_t> taps_;                    // [phase][tap], Q24; empty for the identity
    std::array<std::int16_t, 2 * kTapsPerPhase> left_{};   // the newest kTapsPerPhase frames, twice
    std::array<std::int16_t, 2 * kTapsPerPhase> right_{};
    std::size_t               head_   = 0;   // where the newest frame is
    unsigned                  offset_ = 1;   // the next output's position past the newest input, in up_ths of a frame
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_RESAMPLER_H
