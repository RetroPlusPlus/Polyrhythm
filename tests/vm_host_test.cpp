// The VM host public API, exercised through PUBLIC headers only (retropp/vm.h, retropp/gb.h,
// retropp/gb_routines.h) — no backend header in sight, proving the surface is self-contained. Each
// case drives a synthetic SM83 routine (a hand-assembled `const` byte blob, NO ROM) registered with
// a developer-declared I/O binding, then called as a plain typed C++ function.
//
// Determinism note: SameBoy fills power-on RAM/HRAM with a time-seeded PRNG (faithful — real
// hardware powers on with garbage), so a routine that reads an HRAM seed is only reproducible once
// that seed is written. These tests seed the RNG state explicitly before measuring — exactly as the
// game does (a game seeds its RNG state during init) — using the engine's own memory-
// binding path (pokeByte). rDIV is pure emulation (deterministic across hosts), so a seeded stream
// is reproducible and platform-independent.
#include "retropp/gb.h"
#include "retropp/gb_routines.h"
#include "retropp/vm.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace retropp {
namespace {

// Poke a known byte into a memory address on a VM, so RNG state is deterministic for the test. A RET
// routine whose single input binds to the address: invoke() writes the input to memory before the
// (no-op) run — the engine's memory-input marshalling used as a seed primitive.
void pokeByte(Vm& vm, std::uint16_t addr, std::uint8_t value) {
    static constexpr std::array<std::uint8_t, 1> kRet{0xC9};
    auto poke = vm.uploadRoutine<void(std::uint8_t)>(
        std::span<const std::uint8_t>(kRet),
        RoutineBinding{.inputs = {Location::memory(addr)}});
    poke(value);
}

// ── Case 1: 8-bit register I/O ────────────────────────────────────────────────────────────────
// ADD A,B ; RET  — A,B in, A out. The headline "call it like a function" surface.
TEST(VmHost, EightBitRegisterAddReturnsSum) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};  // ADD A,B ; RET
    auto add = vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
        std::span<const std::uint8_t>(kAdd),
        RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A});
    EXPECT_EQ(add(3, 4), 7);
    EXPECT_EQ(add(200, 55), 255);
}

// ── Case 2: 16-bit register width ─────────────────────────────────────────────────────────────
// ADD HL,HL ; RET — doubles a 16-bit value through a register pair.
TEST(VmHost, SixteenBitRegisterRoundTrips) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kDouble{0x29, 0xC9};  // ADD HL,HL ; RET
    auto dbl = vm.uploadRoutine<std::uint16_t(std::uint16_t)>(
        std::span<const std::uint8_t>(kDouble),
        RoutineBinding{.inputs = {gb::HL}, .output = gb::HL});
    EXPECT_EQ(dbl(0x1234), 0x2468);
    EXPECT_EQ(dbl(0x0001), 0x0002);
}

// ── Case 3: memory-location binding ───────────────────────────────────────────────────────────
// Input read from HRAM, output written to a different HRAM cell — bound by absolute address.
TEST(VmHost, MemoryLocationBinding) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 7> kBytes{
        0xF0, 0x90,  // ldh a,[0xFF90]
        0xC6, 0x10,  // add 0x10
        0xE0, 0x91,  // ldh [0xFF91],a
        0xC9};       // ret
    auto f = vm.uploadRoutine<std::uint8_t(std::uint8_t)>(
        std::span<const std::uint8_t>(kBytes),
        RoutineBinding{.inputs = {Location::memory(0xFF90)}, .output = Location::memory(0xFF91)});
    EXPECT_EQ(f(0x05), 0x15);
    EXPECT_EQ(f(0x20), 0x30);
}

// ── Case 4: rDIV-fidelity (headline) ──────────────────────────────────────────────────────────
// A routine that folds rDIV into an HRAM seed — reads the real free-running DIV register the way an
// original RNG does. Seeded to a known value, then asserts: (a) determinism — two fresh, identically
// seeded VMs produce the identical stream; (b) state persistence — the seed in HRAM carries across
// calls so the stream EVOLVES; (c) the exact byte stream the SameBoy core produces (the golden —
// break a byte → red).
namespace {
constexpr std::array<std::uint8_t, 9> kDivFold{
    0xF0, 0x90,  // ldh a,[0xFF90]   (seed)
    0x47,        // ld b,a
    0xF0, 0x04,  // ldh a,[rDIV]
    0x80,        // add b
    0xE0, 0x90,  // ldh [0xFF90],a
    0xC9};       // ret

std::vector<std::uint8_t> divFoldStream(int n) {
    Vm vm{VMPlatform::GameBoyColor};
    pokeByte(vm, 0xFF90, 0x00);  // known seed
    auto roll = vm.uploadRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(kDivFold), RoutineBinding{.output = gb::A});
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(roll());
    return out;
}
}  // namespace

TEST(VmHost, RDivFoldIsDeterministicAndEvolves) {
    const std::vector<std::uint8_t> a = divFoldStream(8);
    const std::vector<std::uint8_t> b = divFoldStream(8);
    EXPECT_EQ(a, b);  // determinism across fresh, identically seeded VMs

    bool allEqual = true;
    for (std::size_t i = 1; i < a.size(); ++i) {
        if (a[i] != a[0]) allEqual = false;
    }
    EXPECT_FALSE(allEqual);  // state persistence → the stream evolves

    // Golden: the exact stream the compiled SameBoy core produces from seed 0. Break a byte → red.
    const std::vector<std::uint8_t> golden{0, 0, 0, 1, 2, 3, 4, 6};
    EXPECT_EQ(a, golden);
}

// ── Case 5: void return + HRAM side effect persisting across calls ────────────────────────────
TEST(VmHost, VoidRoutineSideEffectPersists) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 3> kWrite{0xE0, 0x92, 0xC9};  // ldh [0xFF92],a ; ret
    static constexpr std::array<std::uint8_t, 3> kRead{0xF0, 0x92, 0xC9};   // ldh a,[0xFF92] ; ret
    auto write = vm.uploadRoutine<void(std::uint8_t)>(
        std::span<const std::uint8_t>(kWrite), RoutineBinding{.inputs = {gb::A}});
    auto read = vm.uploadRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(kRead), RoutineBinding{.output = gb::A});
    write(0x37);
    EXPECT_EQ(read(), 0x37);  // the write's HRAM effect persisted into a later call
    write(0xA5);
    EXPECT_EQ(read(), 0xA5);
}

// ── Case 6: no-idiom call surface (compile-level proof) ───────────────────────────────────────
// The whole point: once registered, a routine is called with C++ values only — no Reg / address /
// register idiom anywhere at the call site. This compiles and runs as plain arithmetic.
TEST(VmHost, CallSiteHasNoMachineIdiom) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};
    const auto add = vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
        std::span<const std::uint8_t>(kAdd),
        RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A});
    std::uint8_t total = 0;
    for (std::uint8_t i = 0; i < 10; ++i) total = add(total, i);  // pure C++ at the call site
    EXPECT_EQ(total, 45);
}

// ── Case 7: HardwareSpeed is realized ───────────────────────────────────────────────
// Registering a HardwareSpeed routine no longer throws — the throttle is realized as the audio-driver
// path (the AudioSystem drives such routines via startDriver / stepDriver). The seam that throws here
// shrank to instances > 1 (still the multi-instance seam), checked below.
TEST(VmHost, HardwareSpeedThrottleIsRealized) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};
    EXPECT_NO_THROW(
        (vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
            std::span<const std::uint8_t>(kAdd),
            RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A,
                           .throttle = Throttle::HardwareSpeed})));
}

// ── Case 8: the multi-instance seam still throws ─────────────────────────────────────
TEST(VmHost, MultiInstanceThrowsEng4Seam) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};
    EXPECT_THROW(
        (vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
            std::span<const std::uint8_t>(kAdd),
            RoutineBinding{.inputs = {gb::A, gb::B}, .output = gb::A}, /*instances=*/2)),
        std::logic_error);
}

// ── Case 9: a platform with no backend built throws at construction ────────────────────────────
TEST(VmHost, PlatformWithNoBackendThrowsAtConstruction) {
    EXPECT_THROW(Vm{VMPlatform::Genesis}, std::runtime_error);
}

// ── Case 10: binding validation ───────────────────────────────────────────────────────────────
TEST(VmHost, ArityMismatchThrows) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};
    // signature has 2 args, binding declares 1 input.
    EXPECT_THROW(
        (vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(
            std::span<const std::uint8_t>(kAdd),
            RoutineBinding{.inputs = {gb::A}, .output = gb::A})),
        std::invalid_argument);
}

TEST(VmHost, WidthLocationMismatchThrows) {
    Vm vm{VMPlatform::GameBoyColor};
    static constexpr std::array<std::uint8_t, 1> kRet{0xC9};
    // a 16-bit argument bound to the 8-bit register A.
    EXPECT_THROW(
        (vm.uploadRoutine<void(std::uint16_t)>(
            std::span<const std::uint8_t>(kRet), RoutineBinding{.inputs = {gb::A}})),
        std::invalid_argument);
}

// ── Case 11: preset divRng == its hand-written expansion ──────────────────────────────────────
// The engine-embedded preset and the equivalent generic registration produce the identical stream
// (proves the preset is exactly its documented expansion). divRng reads only rDIV (no HRAM seed), so
// two fresh VMs are deterministic without seeding — pure rDIV evolution.
TEST(VmHost, DivRngPresetEqualsGenericExpansion) {
    constexpr int kN = 6;

    std::vector<std::uint8_t> presetStream;
    {
        Vm vm{VMPlatform::GameBoyColor};
        auto rng = sameboy::divRng(vm);
        for (int i = 0; i < kN; ++i) presetStream.push_back(rng());
    }

    std::vector<std::uint8_t> genericStream;
    {
        Vm vm{VMPlatform::GameBoyColor};
        static constexpr std::array<std::uint8_t, 3> kBytes{0xF0, 0x04, 0xC9};  // ldh a,[rDIV] ; ret
        auto rng = vm.uploadRoutine<std::uint8_t()>(
            std::span<const std::uint8_t>(kBytes), RoutineBinding{.output = gb::A});
        for (int i = 0; i < kN; ++i) genericStream.push_back(rng());
    }

    EXPECT_EQ(presetStream, genericStream);
}

// advanceClock ticks the free-running divider between calls, so a routine reading it distributes
// instead of degenerating into a counter (the frozen-divider failure). Deterministic given the same
// clock advancement; well-spread across the byte range. Uses the timing profile's per-tick budget —
// no hardcoded cycle count.
TEST(VmHost, AdvanceClockTicksDividerForDistributedRng) {
    const std::uint64_t perTick = TimingProfile::GameBoyColor.cpuCyclesPerTick();
    ASSERT_GT(perTick, 0u);

    auto streamWithClock = [perTick](int n) {
        Vm vm{VMPlatform::GameBoyColor};
        auto rng = sameboy::divRng(vm);
        std::vector<std::uint8_t> out;
        for (int i = 0; i < n; ++i) {
            vm.advanceClock(perTick);  // one tick (frame) of divider time between rolls
            out.push_back(rng());
        }
        return out;
    };

    const std::vector<std::uint8_t> a = streamWithClock(32);
    const std::vector<std::uint8_t> b = streamWithClock(32);
    EXPECT_EQ(a, b);  // deterministic given identical clock advancement

    std::array<bool, 256> seen{};
    int distinct = 0;
    for (std::uint8_t v : a) {
        if (!seen[v]) { seen[v] = true; ++distinct; }
    }
    // With the divider ticking the read spreads across many values; a frozen divider would collapse
    // to a near-constant handful.
    EXPECT_GE(distinct, 16);
}

// Reset returns the machine to a baseline, so a routine whose output depends on machine state
// reproduces its stream when re-seeded and run again. Also confirms the routine bytes in the code
// space survive reset — the same handles are called on both sides of it.
//
// The seed cell is re-established explicitly on each side rather than read back after reset: what
// reset leaves in high RAM is not a fixed value (SameBoy does not zero it), and it is not what this
// case is about.
TEST(VmHost, ResetClearsPersistentState) {
    Vm vm{VMPlatform::GameBoyColor};
    // A folding routine of the test's own: seed ^= rDIV, kept in one high-RAM cell, returned in A.
    // Its stream therefore depends on the divider, which is the machine state reset restores.
    static constexpr std::array<std::uint8_t, 10> kFold{
        0xF0, 0x04,  // ldh a, [rDIV]
        0x47,        // ld  b, a
        0xF0, 0x93,  // ldh a, [$FF93]
        0xA8,        // xor b
        0xE0, 0x93,  // ldh [$FF93], a
        0xC9,        // ret
        0x00,        // (pad)
    };
    static constexpr std::array<std::uint8_t, 3> kSeed{0xE0, 0x93, 0xC9};  // ldh [$FF93],a ; ret

    auto fold = vm.uploadRoutine<std::uint8_t()>(
        std::span<const std::uint8_t>(kFold), RoutineBinding{.output = gb::A});
    auto seed = vm.uploadRoutine<void(std::uint8_t)>(
        std::span<const std::uint8_t>(kSeed), RoutineBinding{.inputs = {gb::A}});

    auto streamFromSeed = [&] {
        seed(0x00);
        std::vector<std::uint8_t> out;
        for (int i = 0; i < 5; ++i) out.push_back(fold());
        return out;
    };

    const std::vector<std::uint8_t> first = streamFromSeed();
    vm.reset();
    const std::vector<std::uint8_t> afterReset = streamFromSeed();

    EXPECT_EQ(first, afterReset);
}


}  // namespace
}  // namespace retropp
