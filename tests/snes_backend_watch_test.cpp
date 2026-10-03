// Watches on the SNES core: what an unwatched machine carries, what each verdict does to a cartridge that
// is running, and what this console's memory path shows that a mock cannot — an opcode fetch is a read,
// a 16-bit access is two byte accesses, a transfer engine's access is watched like the CPU's, a register
// is a place a watch may name, and a routine cannot be called while the machine is inside an access.
//
// The device-free suite (guest_watch_test.cpp) pins the surface on a machine with no CPU. These are the
// claims only the 65816 can answer. Every cartridge here is authored by these tests as 65816 source.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/snes.h"
#include "retropp/vm.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"
#include "src/vm/vm_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;
using vm::VmBackend;
using vm::VmTestAccess;

// A one-bank LoROM cartridge whose reset code is `source`.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    for (const snaggletooth::assembler::Diagnostic& d : program.errors) {
        ADD_FAILURE() << "line " << d.line << ": " << d.message;
    }
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(range.start - 0x8000u));
    }
    return rom;
}

// Native mode and 8-bit registers, then `body` at $00:8100.
std::string program(std::string_view body) {
    return "        ORG $00:8000\n"
           "        EMULATION\n"
           "        CLC\n"
           "        XCE\n"
           "        SEP #$30\n"
           "        A8\n"
           "        X8\n"
           "        JMP !main\n"
           "        ORG $00:8100\n"
           "        A8\n"
           "        X8\n"
           "main:\n" +
           std::string(body);
}

// Reads a cell, adds one, writes it back, forever: one read and one write of the same byte a time round.
// A routine at $00:8180 counts its own calls in $31 and one at $00:81A0 stores A in $50.
constexpr std::uint32_t kCell = 0x7E0030;
const std::string kCellSource = program(
    "loop:   LDA $30\n"
    "        INC A\n"
    "        STA $30\n"
    "        BRA loop\n"
    "        ORG $00:8180\n"
    "count:  INC $31\n"
    "        RTS\n"
    "        ORG $00:81A0\n"
    "stash:  STA $50\n"
    "        RTS\n");

MemoryRegion one(std::uint32_t at) { return MemoryRegion{.at = at, .size = 1}; }

std::uint8_t byteAt(Vm& machine, std::uint32_t at) { return machine.read(one(at)).at(0); }

void runOneStep(Vm& machine) {
    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    machine.stop();
}

SnesBackend& booted(SnesBackend& backend, std::string_view source) {
    backend.loadRom(cartridgeFrom(source));
    backend.bootHostedRom();
    return backend;
}

// ── What an unwatched machine carries ───────────────────────────────────────────────────────────

TEST(SnesWatches, AMachineWatchingNothingCarriesNoWatcher) {
    SnesBackend backend;
    booted(backend, kCellSource);
    backend.setWatchSink([](std::uint32_t, std::uint32_t, VmBackend::AccessKind, std::uint8_t) {
        return AccessVerdict::proceed();
    });

    EXPECT_FALSE(SnesBackendTestAccess::accessWatcherInstalled(backend));
    backend.runForCycles(4'000);
    EXPECT_FALSE(SnesBackendTestAccess::accessWatcherInstalled(backend));
    backend.armWatch(one(kCell), false, true);
    EXPECT_TRUE(SnesBackendTestAccess::accessWatcherInstalled(backend));
    backend.disarmWatch(one(kCell), false, true);
    EXPECT_FALSE(SnesBackendTestAccess::accessWatcherInstalled(backend));
}

// A game that wants writes does not make the machine pay for reads: the direction not armed is never asked.
TEST(SnesWatches, ArmingOneDirectionAsksOnlyAboutThatDirection) {
    SnesBackend backend;
    booted(backend, kCellSource);
    int reads = 0, writes = 0;
    backend.setWatchSink([&](std::uint32_t, std::uint32_t, VmBackend::AccessKind kind, std::uint8_t) {
        ++(kind == VmBackend::AccessKind::Read ? reads : writes);
        return AccessVerdict::proceed();
    });

    backend.armWatch(one(kCell), false, true);
    backend.runForCycles(4'000);
    EXPECT_EQ(reads, 0);
    EXPECT_GT(writes, 0);
}

TEST(SnesWatches, AnArmedPlaceWithNowhereToAskInstallsNothing) {
    SnesBackend backend;
    booted(backend, kCellSource);
    backend.armWatch(one(kCell), true, true);  // armed, but no sink: nothing could be asked about it
    EXPECT_FALSE(SnesBackendTestAccess::accessWatcherInstalled(backend));
}

TEST(SnesWatches, AWatchOnAMemoryTheBusCannotNameIsRefused) {
    SnesBackend backend;
    booted(backend, kCellSource);
    EXPECT_THROW(backend.armWatch(one(snes::videoRam(0)), true, true), std::invalid_argument);
    EXPECT_THROW(backend.armWatch(one(snes::palette(0)), true, true), std::invalid_argument);
    EXPECT_THROW(backend.armWatch(one(0x006000), true, true), std::invalid_argument);  // open bus

    // And through the public surface, where the batch names it.
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    EXPECT_THROW(machine.registerWatches(watches(GuestWatch{
                     .key = "tiles", .at = one(snes::videoRam(0)), .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                         return AccessVerdict::proceed();
                     }})),
                 std::invalid_argument);
}

// ── The verdicts, on a cartridge that is running ────────────────────────────────────────────────

TEST(SnesWatches, AVetoedGuestWriteLeavesTheOldValueIntact) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));

    int          fired = 0, reads = 0;
    std::uint8_t first        = 0;
    bool         everDiffered = false;
    machine.registerWatches(watches(GuestWatch{
        .key = "cell",
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
    EXPECT_GT(fired, 1);
    EXPECT_GT(reads, 1);
    EXPECT_FALSE(everDiffered);  // no store of the guest's ever landed
    EXPECT_EQ(byteAt(machine, kCell), first);
}

// Every read answers 100 whatever work RAM holds; the cartridge adds one and stores it.
TEST(SnesWatches, AGuestReadIsAnsweredWithAValueTheCartridgeDoesNotContain) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    machine.registerWatches(watches(GuestWatch{
        .key = "cell", .at = one(kCell), .onRead = [](Vm&, std::uint32_t, std::uint8_t) {
            return AccessVerdict::instead(100);
        }}));

    runOneStep(machine);
    EXPECT_EQ(byteAt(machine, kCell), 101);
}

TEST(SnesWatches, InsteadOnAGuestWriteLandsTheEnginesValueWithoutRecursing) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    int fired = 0;
    machine.registerWatches(watches(GuestWatch{
        .key = "cell", .at = one(kCell), .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
            ++fired;
            return AccessVerdict::instead(0x5A);
        }}));

    runOneStep(machine);
    EXPECT_EQ(byteAt(machine, kCell), 0x5A);
    EXPECT_GT(fired, 0);
    EXPECT_LT(fired, 100'000);  // one fire per store the guest made, never the substitute's own
}

TEST(SnesWatches, ADroppedWatchLeavesTheCartridgeToItsOwnBehavior) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    machine.registerWatches(watches(GuestWatch{
        .key = "cell", .at = one(kCell), .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
            return AccessVerdict::instead(0x5A);
        }}));
    machine.watches()["cell"].remove();

    runOneStep(machine);
    EXPECT_NE(byteAt(machine, kCell), 0x5A);
}

// ── The engine's own stores are not accesses anyone declared ────────────────────────────────────

// A call into the parked cartridge pushes its landing onto the guest's own stack through the machine's
// host face, spending no cycle. That is the engine acting on the machine, and a watch on the stack does
// not see it; the routine's own RTS reading the landing back is the guest's, and is not a write.
TEST(SnesWatches, TheLandingACallPushesDoesNotFireAWatch) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    auto count = machine.bindRoutine<void()>(0x008180, RoutineBinding{.isa = Isa::Wdc65816});
    int  fired = 0;
    machine.registerWatches(watches(GuestWatch{
        .key = "stack", .at = MemoryRegion{.at = 0x7E0100, .size = 0x100}, .onWrite =
            [&](Vm&, std::uint32_t, std::uint8_t) {
                ++fired;
                return AccessVerdict::proceed();
            }}));

    runOneStep(machine);
    const std::uint8_t before = byteAt(machine, 0x7E0031);
    count();
    EXPECT_EQ(byteAt(machine, 0x7E0031), static_cast<std::uint8_t>(before + 1));  // the routine ran
    EXPECT_EQ(fired, 0);
}

// ── What this console's memory path shows ───────────────────────────────────────────────────────

// An opcode fetch is a read like any other: answering the fetch of INC A with NOP ($EA) changes the
// instruction that runs, and A stays where it started.
TEST(SnesWatches, AnOpcodeFetchIsAReadAndAnsweringItChangesWhatExecutes) {
    const auto count = [](bool answerNop) {
        SnesBackend backend;
        booted(backend, program("        LDA #$00\n"
                                "loop:   INC A\n"
                                "        BRA loop\n"));
        if (answerNop) {
            backend.setWatchSink([](std::uint32_t, std::uint32_t, VmBackend::AccessKind, std::uint8_t) {
                return AccessVerdict::instead(0xEA);
            });
            backend.armWatch(one(0x008102), true, false);
        }
        backend.runForCycles(4'000);
        return SnesBackendTestAccess::state(backend).cpu.a & 0xFF;
    };
    EXPECT_GT(count(false), 0);
    EXPECT_EQ(count(true), 0);
}

// One 16-bit store is two byte writes on two cycles, told one at a time, each at the address it drove —
// here the direct page, $00:0040 and $00:0041, for a watch declared through bank $7E.
TEST(SnesWatches, ASixteenBitStoreFiresTheWatchOncePerByte) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(program("        REP #$20\n"
                                          "        A16\n"
                                          "        LDA #$1234\n"
                                          "loop:   STA $40\n"
                                          "        BRA loop\n")));
    std::vector<std::uint32_t> firstTwo;
    std::vector<std::uint8_t>  values;
    machine.registerWatches(watches(GuestWatch{
        .key = "the pair", .at = MemoryRegion{.at = 0x7E0040, .size = 2}, .onWrite =
            [&](Vm&, std::uint32_t at, std::uint8_t value) {
                if (firstTwo.size() < 2) {
                    firstTwo.push_back(at);
                    values.push_back(value);
                }
                return AccessVerdict::proceed();
            }}));

    runOneStep(machine);
    EXPECT_EQ(firstTwo, (std::vector<std::uint32_t>{0x000040, 0x000041}));
    EXPECT_EQ(values, (std::vector<std::uint8_t>{0x34, 0x12}));
}

// A transfer engine's access is watched like the CPU's. The program points the work-RAM port at $7E:0050
// and has channel 0 copy two bytes of the image into it; the CPU never stores there.
TEST(SnesWatches, ATransferEnginesWriteFiresTheWatch) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(program("        LDA #$50\n"
                                          "        STA !$2181\n"   // WMADDL
                                          "        STZ !$2182\n"   // WMADDM
                                          "        STZ !$2183\n"   // WMADDH: bank $7E
                                          "        STZ !$4300\n"   // DMAP0: A to B, one register, stepping
                                          "        LDA #$80\n"
                                          "        STA !$4301\n"   // BBAD0: $2180, the work-RAM port
                                          "        STZ !$4302\n"
                                          "        LDA #$82\n"
                                          "        STA !$4303\n"   // from $00:8200
                                          "        STZ !$4304\n"
                                          "        LDA #$02\n"
                                          "        STA !$4305\n"   // two bytes
                                          "        STZ !$4306\n"
                                          "        LDA #$01\n"
                                          "        STA !$420B\n"   // MDMAEN: channel 0
                                          "done:   BRA done\n"
                                          "        ORG $00:8200\n"
                                          "        DB $11,$22\n")));
    std::vector<std::uint8_t> landed;
    machine.registerWatches(watches(GuestWatch{
        .key = "target", .at = MemoryRegion{.at = 0x7E0050, .size = 2}, .onWrite =
            [&](Vm&, std::uint32_t, std::uint8_t value) {
                landed.push_back(value);
                return AccessVerdict::proceed();
            }}));

    runOneStep(machine);
    EXPECT_EQ(landed, (std::vector<std::uint8_t>{0x11, 0x22}));
    EXPECT_EQ(byteAt(machine, 0x7E0050), 0x11);
    EXPECT_EQ(byteAt(machine, 0x7E0051), 0x22);
}

// A register is a place a watch may name. Its read is told after the read's own effect; the answer
// changes what the program receives.
TEST(SnesWatches, AWatchOnARegisterFires) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(program("loop:   LDA !$4212\n"  // HVBJOY
                                          "        STA $60\n"
                                          "        BRA loop\n")));
    int fired = 0;
    machine.registerWatches(watches(GuestWatch{
        .key = "hvbjoy", .at = one(0x004212), .onRead = [&](Vm&, std::uint32_t, std::uint8_t) {
            ++fired;
            return AccessVerdict::instead(0xA5);
        }}));

    runOneStep(machine);
    EXPECT_GT(fired, 0);
    EXPECT_EQ(byteAt(machine, 0x7E0060), 0xA5);
}

// ── A handler holds a machine it may reach back into — through an escape, not a watch ───────────

// The machine is part-way through the access a watch is deciding, so a routine cannot run there. The call
// throws, nothing moves, and the guest carries on under the handler's verdict.
TEST(SnesWatches, ACallFromInsideAWatchHandlerThrows) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    auto count   = machine.bindRoutine<void()>(0x008180, RoutineBinding{.isa = Isa::Wdc65816});
    int  refused = 0;
    machine.registerWatches(watches(GuestWatch{
        .key = "cell", .at = one(kCell), .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
            try {
                count();
            } catch (const std::logic_error&) {
                ++refused;
            }
            return AccessVerdict::instead(0x77);
        }}));

    runOneStep(machine);
    EXPECT_GT(refused, 0);
    EXPECT_EQ(byteAt(machine, 0x7E0031), 0x00);  // the routine never ran
    EXPECT_EQ(byteAt(machine, kCell), 0x77);      // and the verdict still landed
}

// An escape is told between instructions, so its handler may call the cartridge's own code — whose
// store is watched in turn. The watch therefore fires from inside the escape's handler, one level in.
TEST(SnesWatches, AWatchFiresFromInsideAnEscapesRoutineOnTheGuestsOwnStore) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    auto stash = machine.bindRoutine<void(std::uint8_t)>(
        0x0081A0, RoutineBinding{.inputs = {snes::A}, .isa = Isa::Wdc65816});

    std::vector<std::string> order;
    machine.registerEscapes(escapes(GuestEscape{
        .key = "loop", .at = 0x008100, .handler = [&](Vm&, std::uint32_t) {
            if (order.size() < 3) {
                order.emplace_back("escape in");
                stash(0x3C);  // the cartridge's own code, storing to $50
                order.emplace_back("escape out");
            }
        }}));
    machine.registerWatches(watches(GuestWatch{
        .key = "stash", .at = one(0x7E0050), .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
            if (order.size() < 3) {
                order.emplace_back("watch");
            }
            return AccessVerdict::proceed();
        }}));

    runOneStep(machine);
    ASSERT_GE(order.size(), 3u);
    EXPECT_EQ(order[0], "escape in");
    EXPECT_EQ(order[1], "watch");
    EXPECT_EQ(order[2], "escape out");
    EXPECT_EQ(byteAt(machine, 0x7E0050), 0x3C);
}

// ── Switching one on a machine that is running ──────────────────────────────────────────────────

struct Cell {
    MemoryRegion cell;
};

TEST(SnesWatches, AWatchSwitchedOffOnTheRunningCartridgeStopsDecidingIt) {
    Vm::SNES machine;
    machine.hostRom(cartridgeFrom(kCellSource));
    const auto places = machine.registerRegions(regions(region(&Cell::cell, one(kCell), "cell")));
    int fired = 0;
    machine.registerWatches(watches(GuestWatch{
        .key = "cell", .at = one(kCell), .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
            ++fired;
            return AccessVerdict::veto();
        }}));

    VmTestAccess::runInline(machine);
    VmTestAccess::stepOnce(machine);
    const int          afterFirst = fired;
    const std::uint8_t held       = machine.read(places, &Cell::cell).at(0);
    ASSERT_GT(afterFirst, 0);

    machine.watches()["cell"].armed(false);
    VmTestAccess::stepOnce(machine);
    machine.stop();
    EXPECT_EQ(fired, afterFirst);             // it decided nothing more
    EXPECT_NE(byteAt(machine, kCell), held);  // and the guest's own stores landed again
}

}  // namespace
}  // namespace retropp
