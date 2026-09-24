#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace retropp {

// The host-selected timing profile for the run loop.
//
// The platform targets the 8-/16-bit console family, so the loop cadence is a developer-selectable
// profile rather than a fixed rate: pass a named preset or a raw period. The run loop reads the
// tick PERIOD; the optional CPU block is a console's cycle arithmetic, for a game that spends
// cycles by hand (Vm::advanceClock, Vm::stepDriver). A hosted machine keeps its own clock. See
// vm-and-routines.md for the VM side.

// Render tick period in NANOSECONDS — named presets whose underlying value IS the exact period
// in ns (integral and drift-free; a frequency like 59.7275 Hz is fractional and cannot be an
// enum value, but its period 16'742'706 ns is exact). Pass a preset or a raw ns period
// interchangeably: static_cast<TickPeriodNs>(16'700'000). The value is the PERIOD, not a rate —
// hence TickPeriodNs, not "tick rate" (a rate is the fractional reciprocal of what is stored).
enum class TickPeriodNs : std::int64_t {
    GameBoy      = 16'742'706,  // 59.7275 Hz — one real GB frame (70'224 cycles @ 4'194'304 Hz)
    GameBoyColor = 16'742'706,  // identical refresh; double-speed is CPU-only (see CpuTiming)
    Snes         = 16'639'263,  // 60.0988 Hz — one SNES frame (357'366 cycles @ 236'250'000/11 Hz)
    SnesPal      = 19'997'208,  // 50.0070 Hz — one PAL SNES frame (425'568 cycles @ 21'281'370 Hz)
    Hz60         = 16'666'667,  // a clean 60 Hz for an original game that just wants a round rate
};

// How many of a machine's cycles a span of wall time is worth, plus the fraction of a cycle left
// over. The leftover is carried into the next draw and never rounded away, so a machine whose clock
// does not divide the tick period stays exact over any number of ticks instead of drifting.
//
// The carry is in the same nanosecond numerator the draw accumulates in — hand back what the last
// draw returned. Zero is the right starting value.
struct CycleDraw {
    std::uint64_t cycles  = 0;
    std::uint64_t carryNs = 0;
};

// A console's CPU clock as numbers a game can do cycle arithmetic with. OPTIONAL: an original game
// with no CPU model omits it.
//
// `cpuClockHz / cpuClockHzDivisor` is the machine's own rate and is the authority: how many cycles a
// tick is worth is derived from it and the period actually being run (see cyclesFor).
// doubleSpeedCyclesPerFrame is the per-frame budget when the machine runs at double speed; the
// display refresh is unchanged, so double speed is a larger cycle budget, not a faster loop cadence.
// A clock that is not a whole number of hertz is carried as the exact ratio it is: the 60 Hz SNES
// master clock is 236'250'000 / 11 Hz.
struct CpuTiming {
    std::uint32_t cpuClockHz;
    std::uint32_t cyclesPerFrame;            // the machine's OWN frame budget, at its own refresh
    std::uint32_t doubleSpeedCyclesPerFrame;
    std::uint32_t cpuClockHzDivisor = 1;     // the clock is cpuClockHz / cpuClockHzDivisor Hz
    [[nodiscard]] constexpr bool operator==(const CpuTiming&) const noexcept = default;

    // The cycles this machine runs in `span`, carrying the sub-cycle remainder. Exact integer
    // arithmetic — the fraction is kept, not discarded, so the running total never drifts and the
    // instantaneous error never exceeds one cycle.
    //
    //   CycleDraw d{};
    //   for (each tick) { d = cpu.cyclesFor(enginePeriod, d.carryNs); step(d.cycles); }
    //
    // A negative or zero span draws nothing and preserves the carry, and so does a zero divisor,
    // which names no clock.
    [[nodiscard]] constexpr CycleDraw cyclesFor(std::chrono::nanoseconds span,
                                                std::uint64_t carryNs = 0) const noexcept {
        if (span.count() <= 0 || cpuClockHzDivisor == 0) {
            return CycleDraw{.cycles = 0, .carryNs = carryNs};
        }
        const std::uint64_t scale = static_cast<std::uint64_t>(cpuClockHzDivisor) * 1'000'000'000u;
        const std::uint64_t acc =
            carryNs + static_cast<std::uint64_t>(cpuClockHz) * static_cast<std::uint64_t>(span.count());
        return CycleDraw{.cycles = acc / scale, .carryNs = acc % scale};
    }
};

// The timing bundle the host hands the run loop: a render cadence (required) + an optional CPU-
// timing block. RunLoop schedules on tickPeriod(); a game reads cpu for its own cycle arithmetic.
// Defaults to the Game Boy Color cadence, so a default-constructed profile needs no arguments for
// the common case.
//
// The named presets (TimingProfile::GameBoyColor, …) are static members of the type, usable in
// constexpr contexts including the RunLoop default argument. The console presets fill both fields.
struct TimingProfile {
    TickPeriodNs             tickPeriodNs = TickPeriodNs::GameBoyColor;  // identity, first member
    std::optional<CpuTiming> cpu{};

    // The tick period as chrono::nanoseconds — what RunLoop's fixed step schedules on.
    [[nodiscard]] constexpr std::chrono::nanoseconds tickPeriod() const noexcept {
        return std::chrono::nanoseconds{static_cast<std::int64_t>(tickPeriodNs)};
    }

    // The CPU cycles in one render tick — one tick is one frame of this profile's model (e.g. 70'224
    // for the Game Boy), for a game doing its own cycle arithmetic. Zero if the profile carries no CPU
    // model.
    //
    // STORED, not derived, and that is deliberate. The cycle count is the exact hardware fact; the ns
    // period is the rounded one. A Game Boy frame IS 70'224 cycles at 4'194'304 Hz, which is
    // 16'742'706.298828125 ns — and TickPeriodNs::GameBoy has to store an integer, so it is 0.3 ns
    // short. Deriving the count back out of that period yields 70'223.9987, i.e. 70'223. So for a
    // machine running at its OWN cadence the stored count is the accurate one and the derivation is
    // strictly worse.
    //
    // Use cyclesFor instead when the machine is NOT running at its own cadence — there is no frame
    // count to reach for then, and the rate is the only thing that answers.
    [[nodiscard]] constexpr std::uint32_t cpuCyclesPerTick() const noexcept {
        return cpu ? cpu->cyclesPerFrame : 0u;
    }

    // The cycles this profile's model runs in one tick of `enginePeriod`, carrying the sub-cycle
    // remainder, for a game's own cycle arithmetic. Two arms: at this profile's own cadence the stored
    // frame count is exact — a Game Boy frame is 70'224 cycles, which is 16'742'706.3 ns, so deriving
    // the count back out of the rounded 16'742'706 would lose a cycle a frame; at any other cadence the
    // clock rate answers and the remainder is carried.
    //
    // Hand back the carry the previous call returned; zero is the right start. Yields nothing if the
    // profile carries no CPU model.
    [[nodiscard]] constexpr CycleDraw cyclesForTick(std::chrono::nanoseconds enginePeriod,
                                                    std::uint64_t carryNs = 0) const noexcept {
        if (!cpu) {
            return CycleDraw{.cycles = 0, .carryNs = carryNs};
        }
        if (enginePeriod == tickPeriod()) {
            return CycleDraw{.cycles = cpu->cyclesPerFrame, .carryNs = carryNs};
        }
        return cpu->cyclesFor(enginePeriod, carryNs);
    }

    // How many render ticks span a wall-clock duration, rounded to nearest — so a consumer schedules
    // "every 2 seconds" as ticksForDuration(std::chrono::seconds{2}) instead of hardcoding a tick
    // count derived from the cadence. Accepts any std::chrono duration (it converts to nanoseconds).
    // Returns the same std::uint64_t the run loop counts ticks in (RunLoop::tickCount()), so a
    // consumer never casts; a non-positive duration yields 0.
    [[nodiscard]] constexpr std::uint64_t ticksForDuration(std::chrono::nanoseconds d) const noexcept {
        const std::int64_t period = static_cast<std::int64_t>(tickPeriodNs);
        if (period <= 0 || d.count() <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>((d.count() + period / 2) / period);  // round to nearest
    }
    [[nodiscard]] constexpr bool operator==(const TimingProfile&) const noexcept = default;

    static const TimingProfile GameBoy;
    static const TimingProfile GameBoyColor;
    static const TimingProfile Snes;
    static const TimingProfile SnesPal;
};

inline constexpr TimingProfile TimingProfile::GameBoy{
    TickPeriodNs::GameBoy,      CpuTiming{4'194'304, 70'224, 140'448}};
inline constexpr TimingProfile TimingProfile::GameBoyColor{
    TickPeriodNs::GameBoyColor, CpuTiming{4'194'304, 70'224, 140'448}};
// The SNES has no double speed, so that field repeats the frame budget.
inline constexpr TimingProfile TimingProfile::Snes{
    .tickPeriodNs = TickPeriodNs::Snes,
    .cpu          = CpuTiming{.cpuClockHz                = 236'250'000,
                              .cyclesPerFrame            = 357'366,
                              .doubleSpeedCyclesPerFrame = 357'366,
                              .cpuClockHzDivisor         = 11}};
inline constexpr TimingProfile TimingProfile::SnesPal{
    .tickPeriodNs = TickPeriodNs::SnesPal,
    .cpu          = CpuTiming{.cpuClockHz                = 21'281'370,
                              .cyclesPerFrame            = 425'568,
                              .doubleSpeedCyclesPerFrame = 425'568,
                              .cpuClockHzDivisor         = 1}};

}  // namespace retropp
