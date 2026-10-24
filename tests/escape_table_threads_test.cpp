// Changing the escape table while the machine runs on a thread of its own.
//
// Arming an escape changes the machine's own code — the backend watches an armed address, and an
// answering escape holds a return at it — so a switch issued from the game thread cannot be applied
// where it is called. It crosses to the thread that owns the machine and lands at the next step
// boundary, in issue order, exactly as a write to a declared place does; issued from inside a
// handler it applies at once, because that code already runs on the machine's own thread.
//
// These cases pin both halves and the consequence the contract states: a change is not visible to
// the read that follows it. The machine is PAUSED (speed {0,1}) wherever a case needs to observe a
// change that has not landed yet — a paused machine takes no step, so nothing drains, and the
// observation is stable rather than a race with the runner.
//
// Every cartridge here is authored by these tests, byte for byte, with its code assembled by the
// engine's own SM83 assembler.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/guest_escape.h"
#include "retropp/memory_region.h"
#include "retropp/vm.h"
#include "tests/authored_cartridge.h"

namespace retropp {
namespace {

constexpr std::uint32_t kProgram = 0x0150;
constexpr std::uint32_t kFirst   = 0x0400;  // the loop calls this one first, every iteration
constexpr std::uint32_t kSecond  = 0x0420;  // and this one straight after
constexpr std::uint32_t kCounter = 0xFF80;  // what the guest advances every iteration

// A loop that calls two routines of its own every time round, and counts its own iterations. It
// runs flat out — no interrupt, no halt — so progress is quick and needs no wall-clock reasoning.
constexpr std::string_view kLoopSource = R"(
    xor a
    ldh [$FF80], a
loop:
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a
    call $0400
    call $0420
    jr loop
)";

constexpr std::string_view kReturnSource = R"(
    ret
)";

struct Places {
    MemoryRegion counter;
};

std::vector<std::uint8_t> authorLoopCartridge(Vm& assembler) {
    std::vector<std::uint8_t> rom = testing::authorCartridge(testing::kSmallestCartridge);
    rom[0x0143] = 0x80;  // a CGB-flagged image
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;  // jp $0150

    const auto place = [&](std::uint32_t at, std::string_view source) {
        const std::vector<std::uint8_t> code = assembler.assemble(std::string(source));
        std::copy(code.begin(), code.end(), rom.begin() + at);
    };
    place(kProgram, kLoopSource);
    place(kFirst, kReturnSource);
    place(kSecond, kReturnSource);
    return rom;
}

// Wait for something the running machine is expected to do. Answers whether it happened, so a case
// asserts on the answer rather than hanging when it does not.
template <class Fn>
[[nodiscard]] bool waitFor(Fn&& done, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// The machine, its declared counter, and the fire counts of the two escapes — the fixture every
// case here starts from. The escapes are declared before run(), which is when that is settled.
struct Running {
    Vm::GBC                    machine;
    RegionMapId<Places>        places;
    std::atomic<std::uint64_t> firstFires{0};
    std::atomic<std::uint64_t> secondFires{0};

    explicit Running(bool firstArmed = true, bool secondArmed = true) {
        machine.hostRom(authorLoopCartridge(machine));
        places = machine.registerRegions(regions(
            region(&Places::counter, MemoryRegion{.at = kCounter, .size = 1}, "counter")));
        machine.registerEscapes(escapes(
            GuestEscape{.key     = "first",
                        .at      = kFirst,
                        .handler = [this](Vm&, std::uint32_t) { firstFires.fetch_add(1); },
                        .armed   = firstArmed},
            GuestEscape{.key     = "second",
                        .at      = kSecond,
                        .handler = [this](Vm&, std::uint32_t) { secondFires.fetch_add(1); },
                        .armed   = secondArmed}));
    }

    ~Running() { machine.stop(); }

    [[nodiscard]] std::uint8_t counter() { return machine.read(places, &Places::counter).at(0); }

    // Let the guest get somewhere: its own counter is one byte, so what is waited for is a change,
    // not an amount.
    [[nodiscard]] bool guestMovesOn() {
        const std::uint8_t was = counter();
        return waitFor([&] { return counter() != was; });
    }
};

// ── The game thread, while the machine runs ─────────────────────────────────────────────────────

TEST(EscapeTableThreads, ADisarmFromTheGameThreadStopsTheEscapeFiring) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    r.machine.escapes()["first"].armed(false);
    ASSERT_TRUE(waitFor([&] {
        const std::uint64_t settled = r.firstFires.load();
        return r.guestMovesOn() && r.firstFires.load() == settled;
    }));

    const std::uint64_t after = r.firstFires.load();
    ASSERT_TRUE(r.guestMovesOn());  // the world carries on
    EXPECT_EQ(r.firstFires.load(), after);
}

TEST(EscapeTableThreads, AnArmFromTheGameThreadStartsItFiring) {
    Running r{/*firstArmed=*/false};
    r.machine.run();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_EQ(r.firstFires.load(), 0u);  // declared, and switched off, so it never ran

    r.machine.escapes()["first"].armed(true);
    EXPECT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));
}

TEST(EscapeTableThreads, ARemoveFromTheGameThreadDropsTheDeclarationAndStopsItFiring) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    r.machine.escapes()["first"].remove();
    EXPECT_TRUE(waitFor([&] { return !r.machine.escapes().contains("first"); }));

    const std::uint64_t after = r.firstFires.load();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_EQ(r.firstFires.load(), after);
    EXPECT_EQ(r.machine.escapes().size(), 1u);  // the other one is untouched
}

// ── What the contract says a change is NOT ──────────────────────────────────────────────────────

TEST(EscapeTableThreads, AChangeIsNotVisibleToTheReadThatFollowsIt) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    r.machine.speed(0, 1);  // paused: no step, so nothing drains and the observation is stable
    ASSERT_TRUE(waitFor([&] {
        const std::uint8_t was = r.counter();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return r.counter() == was;
    }));

    r.machine.escapes()["first"].armed(false);
    EXPECT_TRUE(r.machine.escapes()["first"].armed());  // issued, and not yet landed

    r.machine.speed(1, 1);
    EXPECT_TRUE(waitFor([&] { return !r.machine.escapes()["first"].armed(); }));
}

TEST(EscapeTableThreads, ChangesLandInTheOrderTheyWereIssued) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    r.machine.speed(0, 1);
    ASSERT_TRUE(waitFor([&] {
        const std::uint8_t was = r.counter();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return r.counter() == was;
    }));

    // Two switches with nothing running to apply them, and they disagree: the machine ends on the
    // LAST one, which is only true of a queue that keeps its order. Read the other way round it
    // would end disarmed, so the pair is what tells the two apart — a sequence that reads the same
    // backward would pass either way.
    r.machine.escapes()["first"].armed(false);
    r.machine.escapes()["first"].armed(true);

    r.machine.speed(1, 1);
    EXPECT_TRUE(waitFor([&] { return r.machine.escapes()["first"].armed(); }));

    const std::uint64_t settled = r.firstFires.load();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_GT(r.firstFires.load(), settled);  // and it really is firing again
}

TEST(EscapeTableThreads, AQueuedChangeStillLandsWhenTheMachineStops) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    r.machine.speed(0, 1);
    ASSERT_TRUE(waitFor([&] {
        const std::uint8_t was = r.counter();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return r.counter() == was;
    }));

    r.machine.escapes()["first"].armed(false);  // issued against a machine that will not step again
    r.machine.stop();

    EXPECT_FALSE(r.machine.escapes()["first"].armed());  // parking is a boundary too
}

// ── The machine's own thread ────────────────────────────────────────────────────────────────────

TEST(EscapeTableThreads, AChangeFromInsideAHandlerAppliesAtOnce) {
    Vm::GBC machine;
    machine.hostRom(authorLoopCartridge(machine));

    std::atomic<std::uint64_t> firstFires{0}, secondFires{0};
    constexpr std::uint64_t    kSwitchOnFire = 5;

    // The loop calls `first` and then `second` every time round. The fifth time `first` fires it
    // switches `second` off — from the machine's own thread, so it takes effect before the guest
    // reaches `second` in that same iteration. `second` therefore fires on the four iterations
    // before it and never again: a change that waited for a step boundary would let it fire many
    // more times, because a step is thousands of cycles and this loop is a few dozen.
    machine.registerEscapes(escapes(
        GuestEscape{.key     = "first",
                    .at      = kFirst,
                    .handler = [&](Vm& m, std::uint32_t) {
                        if (firstFires.fetch_add(1) + 1 == kSwitchOnFire) {
                            m.escapes()["second"].armed(false);
                        }
                    }},
        GuestEscape{.key     = "second",
                    .at      = kSecond,
                    .handler = [&](Vm&, std::uint32_t) { secondFires.fetch_add(1); }}));

    machine.run();
    ASSERT_TRUE(waitFor([&] { return firstFires.load() > kSwitchOnFire * 4; }));
    machine.stop();

    EXPECT_EQ(secondFires.load(), kSwitchOnFire - 1);
    EXPECT_FALSE(machine.escapes()["second"].armed());
}

// ── The stopped machine, and the keys ───────────────────────────────────────────────────────────

TEST(EscapeTableThreads, SwitchingAStoppedMachineAppliesAtOnce) {
    Running r;  // never run: the caller's thread is the only one there is
    r.machine.escapes()["first"].armed(false);
    EXPECT_FALSE(r.machine.escapes()["first"].armed());

    r.machine.escapes()["first"].remove();
    EXPECT_FALSE(r.machine.escapes().contains("first"));
    EXPECT_EQ(r.machine.escapes().size(), 1u);
}

TEST(EscapeTableThreads, AKeyThisMachineDoesNotDeclareThrowsWhileItRuns) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(r.guestMovesOn());

    EXPECT_THROW(static_cast<void>(r.machine.escapes()["no such escape"]), std::out_of_range);
    EXPECT_FALSE(r.machine.escapes().contains("no such escape"));
}

// ── Under a game thread that will not leave it alone ────────────────────────────────────────────

TEST(EscapeTableThreads, TheTableHoldsUpUnderAGameThreadSwitchingItConstantly) {
    Running r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));

    // Every switch here is a change to the code the other thread is executing. What this asserts is
    // that the machine survives it and ends where the last call left it; what it is really for is
    // the sanitizers, which see the race this crossing exists to remove.
    constexpr int kSwitches = 400;
    for (int i = 0; i < kSwitches; ++i) {
        r.machine.escapes()["first"].armed(i % 2 == 0);
        r.machine.escapes()["second"].armed(i % 3 == 0);
    }
    r.machine.escapes()["first"].armed(true);

    EXPECT_TRUE(r.guestMovesOn());  // still alive
    EXPECT_TRUE(waitFor([&] { return r.machine.escapes()["first"].armed(); }));
    EXPECT_TRUE(waitFor([&] { return r.firstFires.load() > 0; }));
}

}  // namespace
}  // namespace retropp
