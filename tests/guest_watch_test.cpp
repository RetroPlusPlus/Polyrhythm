// The watch surface, on a machine with no CPU.
//
// Every case here runs against MockVmBackend — a flat byte array whose reads and writes ask
// whatever is watching, exactly as a real core's memory path does. What is being pinned is that
// declaring watches, validating a batch, routing an access to the right handler, realizing each
// verdict, and arming/disarming are the HOST layer's behavior: they work with no console core
// underneath, so no part of them can have been designed around one core's capabilities.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::AccessSource;
using retropp::AccessVerdict;
using retropp::GuestWatch;
using retropp::MemoryRegion;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
using retropp::vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for the accesses.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoyColor};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

MemoryRegion one(std::uint32_t at) { return MemoryRegion{.at = at, .size = 1}; }

// ── The three verdicts, in both directions ──────────────────────────────────────────────────────

TEST(GuestWatches, AWriteVerdictOfProceedLandsTheGuestsOwnByte) {
    MockedVm m;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::proceed();
                   }}));

    m.mock->guestWrite(0xC0A2, 7);

    EXPECT_EQ(m.mock->bytes()[0xC0A2], 7);
}

TEST(GuestWatches, AVetoedWriteLeavesTheOldValueIntact) {
    MockedVm m;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::veto();
                   }}));

    m.mock->guestWrite(0xC0A2, 0);

    EXPECT_EQ(m.mock->bytes()[0xC0A2], 40);
}

TEST(GuestWatches, InsteadOnAWriteLandsTheEnginesValueRatherThanTheGuests) {
    MockedVm m;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [](Vm&, std::uint32_t, std::uint8_t value) {
                       return value < 10 ? AccessVerdict::instead(10) : AccessVerdict::proceed();
                   }}));

    m.mock->guestWrite(0xC0A2, 3);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 10);

    m.mock->guestWrite(0xC0A2, 55);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 55);
}

TEST(GuestWatches, AReadIsAnsweredWithAValueTheMemoryDoesNotHold) {
    MockedVm m;
    m.mock->bytes()[0x4321] = 1;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key    = "coins",
                   .at     = one(0x4321),
                   .onRead = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::instead(99);
                   }}));

    EXPECT_EQ(m.mock->guestRead(0x4321), 99);
    EXPECT_EQ(m.mock->bytes()[0x4321], 1);  // what memory holds never moved
}

TEST(GuestWatches, AReadCannotBePreventedSoAVetoAnswersWithTheByteMemoryHolds) {
    MockedVm m;
    m.mock->bytes()[0x4321] = 77;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key    = "coins",
                   .at     = one(0x4321),
                   .onRead = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::veto();
                   }}));

    EXPECT_EQ(m.mock->guestRead(0x4321), 77);
}

TEST(GuestWatches, AHandlerIsHandedTheByteTheAccessCarriesAndItsOwnMachine) {
    MockedVm      m;
    Vm*           handed   = nullptr;
    std::uint32_t seenAt   = 0;
    std::uint8_t  seenRead = 0;
    std::uint8_t  seenWrit = 0;
    m.mock->bytes()[0xD000] = 12;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key = "slot",
                   .at  = one(0xD000),
                   .onRead =
                       [&](Vm& machine, std::uint32_t at, std::uint8_t value) {
                           handed   = &machine;
                           seenAt   = at;
                           seenRead = value;
                           return AccessVerdict::proceed();
                       },
                   .onWrite =
                       [&](Vm&, std::uint32_t, std::uint8_t value) {
                           seenWrit = value;
                           return AccessVerdict::proceed();
                       }}));

    (void)m.mock->guestRead(0xD000);
    m.mock->guestWrite(0xD000, 34);

    EXPECT_EQ(handed, &m.machine);
    EXPECT_EQ(seenAt, 0xD000u);
    EXPECT_EQ(seenRead, 12);   // a read carries what the machine was about to answer with
    EXPECT_EQ(seenWrit, 34);   // a write carries what the guest is storing
}

// ── Direction is declared, and only the declared one fires ──────────────────────────────────────

TEST(GuestWatches, AWriteOnlyWatchDoesNotFireOnReads) {
    MockedVm m;
    int      reads = 0;
    m.mock->bytes()[0xC100] = 5;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "write only",
                   .at      = one(0xC100),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++reads;
                       return AccessVerdict::veto();
                   }}));

    EXPECT_EQ(m.mock->guestRead(0xC100), 5);  // untouched by a write-only watch
    EXPECT_EQ(reads, 0);

    m.mock->guestWrite(0xC100, 9);
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(m.mock->bytes()[0xC100], 5);
}

// ── A watch is declared over a PLACE, not one address ───────────────────────────────────────────

TEST(GuestWatches, EveryByteOfADeclaredSpanIsWatchedAndTheHandlerKnowsWhichMoved) {
    MockedVm                   m;
    std::vector<std::uint32_t> touched;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "party",
                   .at      = MemoryRegion{.at = 0xC200, .size = 2, .count = 3},  // six bytes
                   .onWrite = [&](Vm&, std::uint32_t at, std::uint8_t) {
                       touched.push_back(at);
                       return AccessVerdict::proceed();
                   }}));

    for (std::uint32_t a = 0xC1FF; a <= 0xC206; ++a) {
        m.mock->guestWrite(a, 1);
    }

    // The six declared bytes fired, in address order; the two either side did not.
    EXPECT_EQ(touched, (std::vector<std::uint32_t>{0xC200, 0xC201, 0xC202, 0xC203, 0xC204,
                                                   0xC205}));
}

// ── Declaration validation ──────────────────────────────────────────────────────────────────────

TEST(GuestWatches, AWatchWithNoHandlerAtAllIsRefusedByItsKey) {
    MockedVm m;
    try {
        m.machine.registerWatches(
            retropp::watches(GuestWatch{.key = "does nothing", .at = one(0xC000)}));
        FAIL() << "a watch with neither handler should not register";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("does nothing"), std::string::npos);
    }
}

TEST(GuestWatches, AnUnreachablePlaceIsRefusedByItsKey) {
    MockedVm m;
    try {
        m.machine.registerWatches(retropp::watches(
            GuestWatch{.key     = "off the end",
                       .at      = MemoryRegion{.at = 0xFFFF, .size = 4},
                       .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                           return AccessVerdict::proceed();
                       }}));
        FAIL() << "a place running off the machine should not register";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("off the end"), std::string::npos);
    }
}

TEST(GuestWatches, TwoWatchesOverOverlappingPlacesAreRefused) {
    MockedVm   m;
    const auto proceed = [](Vm&, std::uint32_t, std::uint8_t) { return AccessVerdict::proceed(); };

    m.machine.registerWatches(retropp::watches(GuestWatch{
        .key = "first", .at = MemoryRegion{.at = 0xC300, .size = 4}, .onWrite = proceed}));

    try {
        m.machine.registerWatches(retropp::watches(GuestWatch{
            .key = "second", .at = MemoryRegion{.at = 0xC302, .size = 4}, .onRead = proceed}));
        FAIL() << "a place overlapping one already watched should not register";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("second"), std::string::npos);
        EXPECT_NE(what.find("first"), std::string::npos);
    }
}

TEST(GuestWatches, ABatchReportsEveryFailingEntryByItsOwnKey) {
    MockedVm m;
    try {
        m.machine.registerWatches(retropp::watches(
            GuestWatch{.key = "no handler", .at = one(0xC000)},
            GuestWatch{.key     = "unreachable",
                       .at      = MemoryRegion{.at = 0xFFFE, .size = 8},
                       .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                           return AccessVerdict::proceed();
                       }}));
        FAIL() << "a batch with two bad entries should not register";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("no handler"), std::string::npos);
        EXPECT_NE(what.find("unreachable"), std::string::npos);
        EXPECT_NE(what.find("2 of 2"), std::string::npos);
    }
}

TEST(GuestWatches, AMachineThatCannotWatchAccessesIsLeftExactlyAsItWas) {
    MockedVm m;
    m.mock->refuseWatches(true);

    EXPECT_THROW(m.machine.registerWatches(retropp::watches(GuestWatch{
                     .key     = "hp",
                     .at      = one(0xC0A2),
                     .onWrite = [](Vm&, std::uint32_t,
                                   std::uint8_t) { return AccessVerdict::proceed(); }})),
                 std::logic_error);

    EXPECT_EQ(m.mock->armedWatchCount(), 0u);
    EXPECT_EQ(m.machine.watches().size(), 0u);
}

// ── Arming, switching and removing ──────────────────────────────────────────────────────────────

TEST(GuestWatches, AWatchSwitchedOffKeepsItsDeclarationAndStopsFiring) {
    MockedVm m;
    int      fired = 0;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::veto();
                   }}));

    m.mock->guestWrite(0xC0A2, 1);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 40);

    m.machine.watches()["hp"].armed(false);
    EXPECT_FALSE(m.machine.watches()["hp"].armed());
    EXPECT_TRUE(m.machine.watches().contains("hp"));  // still declared
    // The machine is released, not merely ignored: a switched-off watch costs the machine what a
    // machine with no watches costs, which it cannot do while its place is still armed below.
    EXPECT_EQ(m.mock->armedWatchCount(), 0u);

    m.mock->guestWrite(0xC0A2, 2);
    EXPECT_EQ(fired, 1);          // it did not run
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 2);  // and the guest's write landed

    m.machine.watches()["hp"].armed(true);
    EXPECT_EQ(m.mock->armedWatchCount(), 1u);
    m.mock->guestWrite(0xC0A2, 3);
    EXPECT_EQ(fired, 2);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 2);  // vetoed again
}

TEST(GuestWatches, AWatchDeclaredUnarmedDoesNotFireUntilItIsSwitchedOn) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerWatches(retropp::watches(GuestWatch{.key     = "hp",
                                                          .at      = one(0xC0A2),
                                                          .onWrite =
                                                              [&](Vm&, std::uint32_t,
                                                                  std::uint8_t) {
                                                                  ++fired;
                                                                  return AccessVerdict::proceed();
                                                              },
                                                          .armed = false}));

    EXPECT_EQ(m.mock->armedWatchCount(), 0u);
    m.mock->guestWrite(0xC0A2, 1);
    EXPECT_EQ(fired, 0);

    m.machine.watches()["hp"].armed(true);
    m.mock->guestWrite(0xC0A2, 2);
    EXPECT_EQ(fired, 1);
}

TEST(GuestWatches, ARemovedWatchIsGoneAndNamingItThrows) {
    MockedVm m;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::veto();
                   }}));
    EXPECT_EQ(m.machine.watches().size(), 1u);

    m.machine.watches()["hp"].remove();

    EXPECT_EQ(m.machine.watches().size(), 0u);
    EXPECT_FALSE(m.machine.watches().contains("hp"));
    EXPECT_THROW((void)m.machine.watches()["hp"], std::out_of_range);

    m.mock->bytes()[0xC0A2] = 40;
    m.mock->guestWrite(0xC0A2, 5);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 5);  // nothing decides it any more
}

TEST(GuestWatches, NamingAWatchThisMachineNeverDeclaredThrows) {
    MockedVm m;
    EXPECT_THROW((void)m.machine.watches()["nothing here"], std::out_of_range);
}

// ── Non-foreclosure: a handler may reach back into the machine, to any depth ─────────────────────

TEST(GuestWatches, AHandlerMayDeclareAnotherWatchWhileItRuns) {
    MockedVm m;
    int      inner = 0;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "outer",
                   .at      = one(0xC400),
                   .onWrite = [&](Vm& machine, std::uint32_t, std::uint8_t) {
                       if (!machine.watches().contains("inner")) {
                           machine.registerWatches(retropp::watches(GuestWatch{
                               .key     = "inner",
                               .at      = one(0xC500),
                               .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                                   ++inner;
                                   return AccessVerdict::veto();
                               }}));
                       }
                       return AccessVerdict::proceed();
                   }}));

    m.mock->guestWrite(0xC400, 1);
    EXPECT_TRUE(m.machine.watches().contains("inner"));

    m.mock->bytes()[0xC500] = 8;
    m.mock->guestWrite(0xC500, 9);
    EXPECT_EQ(inner, 1);
    EXPECT_EQ(m.mock->bytes()[0xC500], 8);
}

TEST(GuestWatches, AWatchFiredFromInsideAnotherWatchsHandlerIsJustAnotherWatch) {
    MockedVm                 m;
    std::vector<std::string> order;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "outer",
                   .at      = one(0xC400),
                   .onWrite =
                       [&](Vm& machine, std::uint32_t, std::uint8_t) {
                           order.emplace_back("outer in");
                           // Reaching into the machine from inside a handler: this write is itself
                           // watched, and its handler runs here, one level in.
                           machine.write(MemoryRegion{.at = 0xC500, .size = 1},
                                         std::vector<std::uint8_t>{1});
                           order.emplace_back("outer out");
                           return AccessVerdict::proceed();
                       }},
        GuestWatch{.key     = "inner",
                   .at      = one(0xC500),
                   .from    = AccessSource::GuestAndGame,
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       order.emplace_back("inner");
                       return AccessVerdict::proceed();
                   }}));

    m.mock->guestWrite(0xC400, 1);

    EXPECT_EQ(order, (std::vector<std::string>{"outer in", "inner", "outer out"}));
}

// ── Whose accesses fire it ──────────────────────────────────────────────────────────────────────

TEST(GuestWatches, TheGamesOwnWriteDoesNotFireAWatchOnTheGuestAlone) {
    MockedVm m;
    int      fired = 0;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::veto();
                   }}));

    m.machine.write(MemoryRegion{.at = 0xC0A2, .size = 1}, std::vector<std::uint8_t>{7});

    EXPECT_EQ(fired, 0);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 7);  // the game's own write landed, undecided
}

TEST(GuestWatches, AWatchThatAskedForTheGamesOwnWritesSeesThemAndCanVetoOne) {
    MockedVm m;
    int      fired = 0;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "hp",
                   .at      = one(0xC0A2),
                   .from    = AccessSource::GuestAndGame,
                   .onWrite = [&](Vm&, std::uint32_t, std::uint8_t) {
                       ++fired;
                       return AccessVerdict::veto();
                   }}));

    m.machine.write(MemoryRegion{.at = 0xC0A2, .size = 1}, std::vector<std::uint8_t>{7});

    EXPECT_EQ(fired, 1);
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 40);  // vetoed, so the byte kept what it had
}

TEST(GuestWatches, AGamesOwnReadThatAskedForItIsAnsweredByItsHandler) {
    MockedVm m;
    m.mock->bytes()[0xC0A2] = 40;

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key    = "hp",
                   .at     = one(0xC0A2),
                   .from   = AccessSource::GuestAndGame,
                   .onRead = [](Vm&, std::uint32_t, std::uint8_t) {
                       return AccessVerdict::instead(3);
                   }}));

    const std::vector<std::uint8_t> got =
        m.machine.read(MemoryRegion{.at = 0xC0A2, .size = 1});

    EXPECT_EQ(got, (std::vector<std::uint8_t>{3}));
    EXPECT_EQ(m.mock->bytes()[0xC0A2], 40);  // memory itself never moved
}

TEST(GuestWatches, OnlyTheVetoedBytesOfAGamesWriteKeepTheirOldValues) {
    MockedVm m;
    for (std::uint32_t i = 0; i < 4; ++i) {
        m.mock->bytes()[0xC600 + i] = static_cast<std::uint8_t>(0xA0 + i);
    }

    m.machine.registerWatches(retropp::watches(
        GuestWatch{.key     = "block",
                   .at      = MemoryRegion{.at = 0xC600, .size = 4},
                   .from    = AccessSource::GuestAndGame,
                   .onWrite = [](Vm&, std::uint32_t at, std::uint8_t) {
                       // The two middle bytes are held; the outer two are the game's to set.
                       return (at == 0xC601 || at == 0xC602) ? AccessVerdict::veto()
                                                             : AccessVerdict::proceed();
                   }}));

    m.machine.write(MemoryRegion{.at = 0xC600, .size = 4},
                    std::vector<std::uint8_t>{1, 2, 3, 4});

    EXPECT_EQ(m.mock->bytes()[0xC600], 1);
    EXPECT_EQ(m.mock->bytes()[0xC601], 0xA1);
    EXPECT_EQ(m.mock->bytes()[0xC602], 0xA2);
    EXPECT_EQ(m.mock->bytes()[0xC603], 4);
}

TEST(GuestWatches, AWatchOnTheGamesOwnVerbsCannotNameABankQualifiedPlace) {
    MockedVm m;
    try {
        m.machine.registerWatches(retropp::watches(
            GuestWatch{.key     = "banked",
                       .at      = MemoryRegion{.at = (1u << 16) | 0x4000u, .size = 1},
                       .from    = AccessSource::GuestAndGame,
                       .onWrite = [](Vm&, std::uint32_t, std::uint8_t) {
                           return AccessVerdict::proceed();
                       }}));
        FAIL() << "a GuestAndGame watch on a banked place should not register";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("banked"), std::string::npos);
    }
}

TEST(GuestWatches, AnEmptyBatchIsRefused) {
    MockedVm m;
    EXPECT_THROW(m.machine.registerWatches(retropp::WatchMap{}), std::invalid_argument);
}

}  // namespace
