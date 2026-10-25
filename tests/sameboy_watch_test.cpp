// Watches on the real core: what an unwatched machine carries, what each verdict actually does to a
// cartridge that is running, and the two things about a real memory path that a mock cannot show —
// that an opcode fetch is a read like any other, and that a 16-bit access is two byte accesses.
//
// The device-free suite (guest_watch_test.cpp) pins the surface on a machine with no CPU. These are
// the claims only a CPU can answer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/gb.h"  // gb::A — the register a bound routine's answer comes back in
#include "retropp/vm.h"
#include "src/vm/gameboy/sameboy_machine.h"
#include "src/vm/vm_testing.h"

namespace {

using retropp::AccessVerdict;
using retropp::GuestWatch;
using retropp::MemoryRegion;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::vm::ConsoleModel;
using retropp::vm::Registers;
using retropp::vm::SameBoyMachine;
using retropp::vm::VmTestAccess;

constexpr std::uint16_t kLoop  = 0x0150;
constexpr std::uint32_t kCell  = 0xC0A2;
constexpr std::uint32_t kPair  = 0xC300;
constexpr std::uint16_t kBgp   = 0xFF47;  // a register the boot seeding writes through the bus

// The 32 KiB ROM-only header every image here shares, entering at 0x0150 — a place no boot ROM
// overlays, so a booted machine reaches it and stepping just works.
std::vector<std::uint8_t> romShell() {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0147] = 0x00;  // ROM ONLY
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0149] = 0x00;  // no RAM
    rom[0x0100] = 0xC3;
    rom[0x0101] = 0x50;
    rom[0x0102] = 0x01;  // jp $0150
    return rom;
}

// Reads a cell, adds one, writes it back, forever. One read and one write of the same byte per
// iteration, which is what a watch declaring both handlers answers for.
//
//   0x0150:  ld a,[$C0A2]
//   0x0153:  inc a
//   0x0154:  ld [$C0A2],a
//   0x0157:  jr -9            (back to 0x0150)
std::vector<std::uint8_t> makeCellRom() {
    std::vector<std::uint8_t> rom = romShell();
    rom[0x0150] = 0xFA;
    rom[0x0151] = 0xA2;
    rom[0x0152] = 0xC0;
    rom[0x0153] = 0x3C;
    rom[0x0154] = 0xEA;
    rom[0x0155] = 0xA2;
    rom[0x0156] = 0xC0;
    rom[0x0157] = 0x18;
    rom[0x0158] = 0xF7;
    return rom;
}

// Counts in A forever — two instructions, nothing but the fetch and the increment.
//
//   0x0150:  inc a
//   0x0151:  jr -3
std::vector<std::uint8_t> makeCountRom() {
    std::vector<std::uint8_t> rom = romShell();
    rom[0x0150] = 0x3C;
    rom[0x0151] = 0x18;
    rom[0x0152] = 0xFD;
    return rom;
}

// Stores the stack pointer — ONE instruction making a 16-bit write, which the hardware performs as
// two byte writes at two addresses.
//
//   0x0150:  ld [$C300],sp
//   0x0153:  jr -5
std::vector<std::uint8_t> makeWordRom() {
    std::vector<std::uint8_t> rom = romShell();
    rom[0x0150] = 0x08;
    rom[0x0151] = 0x00;
    rom[0x0152] = 0xC3;
    rom[0x0153] = 0x18;
    rom[0x0154] = 0xFB;
    return rom;
}

MemoryRegion one(std::uint32_t at) { return MemoryRegion{.at = at, .size = 1}; }

// Host an image, declare `map`, run it inline for one step, then park. Reading a parked machine
// gives the bytes as they are, which is what every case below checks.
void runOneStep(Vm& machine) {
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
}

// ── What an unwatched machine carries ───────────────────────────────────────────────────────────

TEST(SameBoyWatches, AMachineWatchingNothingCarriesNeitherHook) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeCellRom());
    m.reset();

    EXPECT_FALSE(m.readWatchInstalled());
    EXPECT_FALSE(m.writeWatchInstalled());
    m.setRegisters(Registers{.pc = kLoop});
    m.runForCycles(400);
    EXPECT_FALSE(m.readWatchInstalled());
    EXPECT_FALSE(m.writeWatchInstalled());
}

// A game that wants writes must not make the machine pay for reads: each direction is installed on
// its own, which is what makes declaring only what you need worth declaring.
TEST(SameBoyWatches, ArmingOneDirectionInstallsOnlyThatDirectionsHook) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeCellRom());
    m.reset();
    m.setAccessSinks([](std::uint16_t, std::uint8_t data) { return data; },
                     [](std::uint16_t, std::uint8_t, std::uint8_t&) {
                         return SameBoyMachine::WriteAction::Allow;
                     });

    m.armWriteWatch(0xC0A2);
    EXPECT_FALSE(m.readWatchInstalled());
    EXPECT_TRUE(m.writeWatchInstalled());

    m.armReadWatch(0xC0A2);
    EXPECT_TRUE(m.readWatchInstalled());

    m.disarmWriteWatch(0xC0A2);
    EXPECT_TRUE(m.readWatchInstalled());
    EXPECT_FALSE(m.writeWatchInstalled());

    m.disarmReadWatch(0xC0A2);
    EXPECT_FALSE(m.readWatchInstalled());
}

TEST(SameBoyWatches, AWatchedAddressWithNowhereToReportInstallsNothing) {
    SameBoyMachine m(ConsoleModel::GameBoyColor);
    m.loadRom(makeCellRom());
    m.reset();

    m.armWriteWatch(0xC0A2);  // watched, but no sink: nothing could be asked about it
    EXPECT_FALSE(m.writeWatchInstalled());
}

// ── The verdicts, on a cartridge that is running ─────────────────────────────────────────────────

TEST(SameBoyWatches, AVetoedGuestWriteLeavesTheOldValueIntact) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCellRom());

    // Power-on work RAM is whatever the machine came up with — SameBoy seeds it from a PRNG, as
    // hardware does — so the claim is not a particular value but that the byte NEVER MOVED. The
    // read handler observes what the cartridge sees each time round the loop; with every store
    // vetoed, that is the same byte forever, and it is still there when the machine parks.
    int          fired = 0;
    int          reads = 0;
    std::uint8_t first = 0;
    bool         everDiffered = false;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key = "cell",
                   .at  = one(kCell),
                   .onRead =
                       [&](Vm&, std::uint32_t, std::uint8_t value) {
                           if (reads++ == 0) {
                               first = value;
                           } else if (value != first) {
                               everDiffered = true;
                           }
                           return AccessVerdict::proceed();
                       },
                   .onWrite =
                       [&](Vm&, std::uint32_t, std::uint8_t) {
                           ++fired;
                           return AccessVerdict::veto();
                       }}));

    runOneStep(machine);

    EXPECT_GT(fired, 1);  // the guest tried, many times
    EXPECT_GT(reads, 1);
    EXPECT_FALSE(everDiffered);  // no store of the guest's ever landed
    EXPECT_EQ(machine.read(one(kCell)).at(0), first);
}

TEST(SameBoyWatches, AGuestReadIsAnsweredWithAValueTheCartridgeDoesNotContain) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCellRom());

    // Every read of the cell answers 100, whatever memory holds; the cartridge adds one and stores
    // the result, so the byte that lands is 101 no matter how many times the loop ran.
    machine.registerWatches(retropp::watches(
        GuestWatch{.key    = "cell",
                   .at     = one(kCell),
                   .onRead = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::instead(100);
                   }}));

    runOneStep(machine);

    EXPECT_EQ(machine.read(one(kCell)).at(0), 101);
}

TEST(SameBoyWatches, InsteadOnAGuestWriteLandsTheEnginesValueWithoutRecursing) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCellRom());

    int fired = 0;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "cell",
                   .at      = one(kCell),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::instead(0x5A);
                   }}));

    runOneStep(machine);

    // The engine's own store lands the substituted byte...
    EXPECT_EQ(machine.read(one(kCell)).at(0), 0x5A);
    // ...and does not itself count as a watched access, or this would not terminate at all. One
    // fire per store the guest made is the whole claim: a re-entering store would double it, and an
    // endlessly re-entering one would never have returned here.
    EXPECT_GT(fired, 0);
    EXPECT_LT(fired, 100'000);
}

TEST(SameBoyWatches, ADroppedWatchLeavesTheCartridgeToItsOwnBehaviour) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCellRom());

    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "cell",
                   .at      = one(kCell),
                   .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::instead(0x5A);
                   }}));
    machine.watches()["cell"].remove();

    runOneStep(machine);

    EXPECT_NE(machine.read(one(kCell)).at(0), 0x5A);
}

// ── The engine's own stores are not accesses anyone declared ────────────────────────────────────

// Bringing a hosted image to its post-boot state writes hardware registers through the machine's own
// bus. Those are the engine acting on the machine, and a watch declared over one before the machine
// boots does not see them.
TEST(SameBoyWatches, TheEnginesOwnBootSeedingDoesNotFireAWatch) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCountRom());

    int fired = 0;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "palette",
                   .at      = one(kBgp),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::proceed();
                   }}));

    runOneStep(machine);  // this is where the image boots

    EXPECT_EQ(fired, 0);
}

// ── Two things about a real memory path a mock cannot show ──────────────────────────────────────

// A watch is on the ADDRESS BUS, and an instruction fetch is a read like any other. Answering a code
// byte with a different one therefore changes the instruction that executes — here INC A becomes NOP
// and the cartridge stops counting. This is the Game Boy backend's own property.
TEST(SameBoyWatches, AnOpcodeFetchIsAReadAndAnsweringItChangesWhatExecutes) {
    // Made directly on the machine, where A can be read: the same cycle budget with the fetch
    // answered as NOP leaves A where it started, and un-answered advances it.
    const auto runCount = [](bool substituteNop) {
        SameBoyMachine m(ConsoleModel::GameBoyColor);
        m.loadRom(makeCountRom());
        m.reset();
        if (substituteNop) {
            m.setAccessSinks(
                [](std::uint16_t address, std::uint8_t data) -> std::uint8_t {
                    return address == kLoop ? 0x00 : data;
                },
                {});
            m.armReadWatch(kLoop);
        }
        m.setRegisters(Registers{.af = 0x0000, .pc = kLoop});
        m.runForCycles(400);
        return static_cast<int>(m.registers().af >> 8);
    };

    EXPECT_GT(runCount(false), 0);   // counting, as the cartridge wrote it
    EXPECT_EQ(runCount(true), 0);    // the fetch was answered, so A stayed where it started
}

// One 16-bit store is two byte writes at two addresses. A watch sees both, separately.
TEST(SameBoyWatches, ASixteenBitWriteFiresTheWatchOncePerByte) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeWordRom());

    std::vector<std::uint32_t> firstTwo;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "the pair",
                   .at      = MemoryRegion{.at = kPair, .size = 2},
                   .onWrite = [&](Vm&, std::uint32_t at, std::uint8_t) {
                       if (firstTwo.size() < 2) {
                           firstTwo.push_back(at);
                       }
                       return AccessVerdict::proceed();
                   }}));

    runOneStep(machine);

    EXPECT_EQ(firstTwo, (std::vector<std::uint32_t>{kPair, kPair + 1}));
}

// ── Non-foreclosure: a handler holds a machine it may reach back into ───────────────────────────

TEST(SameBoyWatches, AHandlerMayCallTheCartridgesOwnCodeAndTheGuestCarriesOn) {
    // The cartridge's own doubling routine, bound where it sits — called from inside the watch that
    // is deciding a write. The guest is parked mid-instruction while it runs, and its own context is
    // given back, so the interrupted store completes exactly as the verdict says.
    std::vector<std::uint8_t> rom = makeCellRom();
    rom[0x0180]                   = 0x87;  // add a,a
    rom[0x0181]                   = 0xC9;  // ret
    Vm second{VMPlatform::GameBoyColor};
    second.hostRom(rom);

    auto twice = second.bindRoutine<std::uint8_t(std::uint8_t)>(
        0x0180, retropp::RoutineBinding{.inputs = {retropp::gb::A}, .output = retropp::gb::A});

    int          calls = 0;
    std::uint8_t last  = 0;
    second.registerWatches(retropp::watches(
        GuestWatch{.key     = "cell",
                   .at      = one(kCell),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t value) {
                       ++calls;
                       last = twice(value);
                       return AccessVerdict::instead(last);
                   }}));

    runOneStep(second);

    EXPECT_GT(calls, 0);
    EXPECT_EQ(second.read(one(kCell)).at(0), last);
}

// A watch fires while the guest is mid-instruction, and its handler may run the cartridge's own code
// — which makes its own accesses, which are watched in turn. The inner watch therefore fires from
// inside the outer one's handler, on the guest's own store, one level in.
TEST(SameBoyWatches, AWatchFiresFromInsideAnotherWatchsHandlerOnTheGuestsOwnStore) {
    std::vector<std::uint8_t> rom = makeCellRom();
    rom[0x0180]                   = 0xEA;
    rom[0x0181]                   = 0x00;
    rom[0x0182]                   = 0xC5;  // ld [$C500],a
    rom[0x0183]                   = 0xC9;  // ret

    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(rom);

    auto stash = machine.bindRoutine<void(std::uint8_t)>(
        0x0180, retropp::RoutineBinding{.inputs = {retropp::gb::A}});

    std::vector<std::string> order;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key = "outer",
                   .at  = one(kCell),
                   .onWrite =
                       [&](Vm&, std::uint32_t, std::uint8_t value) {
                           if (order.size() < 3) {
                               order.emplace_back("outer in");
                               stash(value);  // the cartridge's own code, storing to $C500
                               order.emplace_back("outer out");
                           }
                           return AccessVerdict::proceed();
                       }},
        GuestWatch{.key     = "inner",
                   .at      = one(0xC500),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       if (order.size() < 3) {
                           order.emplace_back("inner");
                       }
                       return AccessVerdict::proceed();
                   }}));

    runOneStep(machine);

    ASSERT_GE(order.size(), 3u);
    EXPECT_EQ(order[0], "outer in");
    EXPECT_EQ(order[1], "inner");
    EXPECT_EQ(order[2], "outer out");
}

// ── Switching one on a machine that is running ──────────────────────────────────────────────────

TEST(SameBoyWatches, AWatchSwitchedOffOnTheRunningCartridgeStopsDecidingIt) {
    Vm machine{VMPlatform::GameBoyColor};
    machine.hostRom(makeCellRom());

    int fired = 0;
    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "cell",
                   .at      = one(kCell),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::veto();
                   }}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    const int afterFirst = fired;
    ASSERT_GT(afterFirst, 0);

    machine.watches()["cell"].armed(false);
    VmTestAccess::stepOnce(machine);
    machine.stop();

    EXPECT_EQ(fired, afterFirst);                     // it decided nothing more
    EXPECT_NE(machine.read(one(kCell)).at(0), 0);     // and the guest's own stores landed again
}

// ── The machine running on a thread of its own ──────────────────────────────────────────────────
//
// Switching a watch changes what the machine's own memory path tests on every access, so a switch
// issued from the game thread cannot be applied where it is called. It crosses to the thread that
// owns the machine and lands at the next step boundary, in issue order, exactly as a write to a
// declared place does. The machine is PAUSED (speed {0,1}) wherever a case needs to observe a change
// that has not landed yet — a paused machine takes no step, so nothing drains and the observation is
// stable rather than a race with the runner.

constexpr std::uint32_t kTicker = 0xC0B0;  // the loop advances this one with nothing watching it

// The cell loop with a second counter beside it, so a case can tell that the guest is getting on
// with its work even while every store to the watched cell is being refused.
//
//   0x0150:  ld a,[$C0A2]   /  inc a  /  ld [$C0A2],a      (the watched cell)
//   0x0157:  ld a,[$C0B0]   /  inc a  /  ld [$C0B0],a      (the ticker)
//   0x015E:  jr -16
std::vector<std::uint8_t> makeTickerRom() {
    std::vector<std::uint8_t> rom = romShell();
    rom[0x0143]                   = 0x80;  // a CGB-flagged image
    rom[0x0150] = 0xFA; rom[0x0151] = 0xA2; rom[0x0152] = 0xC0;
    rom[0x0153] = 0x3C;
    rom[0x0154] = 0xEA; rom[0x0155] = 0xA2; rom[0x0156] = 0xC0;
    rom[0x0157] = 0xFA; rom[0x0158] = 0xB0; rom[0x0159] = 0xC0;
    rom[0x015A] = 0x3C;
    rom[0x015B] = 0xEA; rom[0x015C] = 0xB0; rom[0x015D] = 0xC0;
    rom[0x015E] = 0x18; rom[0x015F] = 0xF0;
    return rom;
}

struct Places {
    MemoryRegion ticker;
};

// Wait for something the running machine is expected to do. Answers whether it happened, so a case
// asserts on the answer rather than hanging when it does not.
template <class Fn>
[[nodiscard]] bool waitFor(Fn&& done,
                           std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// The machine, its declared ticker, and how often the watch decided a store — the fixture every
// threaded case starts from.
struct RunningWatched {
    Vm::GBC                    machine;
    retropp::RegionMapId<Places> places;
    std::atomic<std::uint64_t> decided{0};

    explicit RunningWatched(bool armed = true) {
        machine.hostRom(makeTickerRom());
        places = machine.registerRegions(retropp::regions(retropp::region(
            &Places::ticker, MemoryRegion{.at = kTicker, .size = 1}, "ticker")));
        machine.registerWatches(retropp::watches(
            GuestWatch{.key = "cell",
                       .at  = one(kCell),
                       .onWrite =
                           [this](Vm&, std::uint32_t, std::uint8_t) {
                               decided.fetch_add(1);
                               return AccessVerdict::veto();
                           },
                       .armed = armed}));
    }

    ~RunningWatched() { machine.stop(); }

    [[nodiscard]] std::uint8_t ticker() { return machine.read(places, &Places::ticker).at(0); }

    // Let the guest get somewhere: its ticker is one byte, so what is waited for is a change, not
    // an amount.
    [[nodiscard]] bool guestMovesOn() {
        const std::uint8_t was = ticker();
        return waitFor([&] { return ticker() != was; });
    }

    // Park the runner so a change issued now has no step boundary to land at.
    [[nodiscard]] bool pause() {
        machine.speed(0, 1);
        return waitFor([&] {
            const std::uint8_t was = ticker();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return ticker() == was;
        });
    }
};

TEST(WatchTableThreads, ADisarmFromTheGameThreadStopsTheWatchDecidingStores) {
    RunningWatched r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.decided.load() > 0; }));

    r.machine.watches()["cell"].armed(false);
    ASSERT_TRUE(waitFor([&] {
        const std::uint64_t settled = r.decided.load();
        return r.guestMovesOn() && r.decided.load() == settled;
    }));

    const std::uint64_t after = r.decided.load();
    ASSERT_TRUE(r.guestMovesOn());  // the world carries on
    EXPECT_EQ(r.decided.load(), after);
}

TEST(WatchTableThreads, AnArmFromTheGameThreadStartsItDeciding) {
    RunningWatched r{/*armed=*/false};
    r.machine.run();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_EQ(r.decided.load(), 0u);  // declared, and switched off, so it never ran

    r.machine.watches()["cell"].armed(true);
    EXPECT_TRUE(waitFor([&] { return r.decided.load() > 0; }));
}

TEST(WatchTableThreads, ARemoveFromTheGameThreadDropsTheDeclaration) {
    RunningWatched r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.decided.load() > 0; }));

    r.machine.watches()["cell"].remove();
    EXPECT_TRUE(waitFor([&] { return !r.machine.watches().contains("cell"); }));

    const std::uint64_t after = r.decided.load();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_EQ(r.decided.load(), after);
}

TEST(WatchTableThreads, AChangeIsNotVisibleToTheReadThatFollowsIt) {
    RunningWatched r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.decided.load() > 0; }));
    ASSERT_TRUE(r.pause());

    r.machine.watches()["cell"].armed(false);
    EXPECT_TRUE(r.machine.watches()["cell"].armed());  // issued, and still waiting for a boundary

    r.machine.speed(1, 1);
    EXPECT_TRUE(waitFor([&] { return !r.machine.watches()["cell"].armed(); }));
}

// Three switches with nothing running to apply them, ending on a value the reversed sequence would
// not reach: off, on, off ends OFF only for a queue that kept its order, and read backward would
// end ON. A sequence that reads the same both ways would pass either way and pin nothing.
TEST(WatchTableThreads, ChangesLandInTheOrderTheyWereIssued) {
    RunningWatched r;
    r.machine.run();
    ASSERT_TRUE(waitFor([&] { return r.decided.load() > 0; }));
    ASSERT_TRUE(r.pause());

    r.machine.watches()["cell"].armed(false);
    r.machine.watches()["cell"].armed(true);
    r.machine.watches()["cell"].armed(false);

    r.machine.speed(1, 1);
    EXPECT_TRUE(waitFor([&] { return !r.machine.watches()["cell"].armed(); }));

    const std::uint64_t after = r.decided.load();
    ASSERT_TRUE(r.guestMovesOn());
    EXPECT_EQ(r.decided.load(), after);  // it really is off, not merely reported off
}

// A switch issued from inside a handler applies at once: that code already runs on the machine's own
// thread, so there is nothing for it to cross.
TEST(WatchTableThreads, ASwitchIssuedFromInsideAHandlerAppliesAtOnce) {
    Vm::GBC machine;
    machine.hostRom(makeTickerRom());

    std::atomic<std::uint64_t> decided{0};
    std::atomic<bool>          sawItselfOff{false};
    machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "cell",
                   .at      = one(kCell),
                   .onWrite = [&](Vm& m, std::uint32_t, std::uint8_t) {
                       decided.fetch_add(1);
                       m.watches()["cell"].armed(false);
                       sawItselfOff.store(!m.watches()["cell"].armed());
                       return AccessVerdict::proceed();
                   }}));

    machine.run();
    EXPECT_TRUE(waitFor([&] { return decided.load() > 0; }));
    EXPECT_TRUE(waitFor([&] { return sawItselfOff.load(); }));
    EXPECT_EQ(decided.load(), 1u);  // it switched itself off, so it decided exactly one store
    machine.stop();
}

}  // namespace
