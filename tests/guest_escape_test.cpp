// The escape surface, on a machine with no CPU.
//
// Every case here runs against MockVmBackend — a flat byte array and a scripted walk. What is being
// pinned is that declaring escapes, validating a batch, routing a fire to the right handler and
// arming/disarming are the HOST layer's behavior: they work with no console core underneath, so no
// part of them can have been designed around one core's capabilities.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::EscapeMap;
using retropp::GuestEscape;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
using retropp::vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for the walk.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoyColor};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

void walk(MockVmBackend& mock, std::vector<std::uint32_t> addresses) {
    mock.walk(addresses);
}

TEST(GuestEscapes, ADeclaredEscapeFiresItsHandlerWithItsOwnAddress) {
    MockedVm    m;
    int         fired  = 0;
    std::uint32_t seen = 0;
    Vm*         handed = nullptr;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm& machine, std::uint32_t at) {
            ++fired;
            seen   = at;
            handed = &machine;
        }}));

    walk(*m.mock, {0xC300, 0xC310, 0xC320});

    EXPECT_EQ(fired, 1);
    EXPECT_EQ(seen, 0xC310u);
    EXPECT_EQ(handed, &m.machine);
    EXPECT_EQ(m.mock->executed(), 3u);
}

TEST(GuestEscapes, OneHandlerServingTwoEscapesTellsThemApartByAddress) {
    MockedVm                   m;
    std::vector<std::uint32_t> order;
    const auto note = [&](Vm&, std::uint32_t at) { order.push_back(at); };

    m.machine.registerEscapes(retropp::escapes(
        GuestEscape{.key = "first", .at = 0x4000, .handler = note},
        GuestEscape{.key = "second", .at = 0x4100, .handler = note}));

    walk(*m.mock, {0x4100, 0x4000, 0x4100});

    EXPECT_EQ(order, (std::vector<std::uint32_t>{0x4100, 0x4000, 0x4100}));
}

TEST(GuestEscapes, AnEscapeSwitchedOffKeepsItsDeclarationAndStopsFiring) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    m.machine.escapes()["rng draw"].armed(false);
    walk(*m.mock, {0xC310, 0xC310});

    EXPECT_EQ(fired, 0);
    EXPECT_TRUE(m.machine.escapes().contains("rng draw"));
    EXPECT_FALSE(m.machine.escapes()["rng draw"].armed());
    EXPECT_EQ(m.machine.escapes().size(), 1u);
}

TEST(GuestEscapes, SwitchingOneBackOnResumesFiringWithoutRedeclaring) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    m.machine.escapes()["rng draw"].armed(false);
    walk(*m.mock, {0xC310});
    m.machine.escapes()["rng draw"].armed(true);
    walk(*m.mock, {0xC310});

    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(m.machine.escapes()["rng draw"].armed());
}

TEST(GuestEscapes, EscapesAreLiveByDefault) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    walk(*m.mock, {0xC310});

    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(m.machine.escapes()["rng draw"].armed());
}

TEST(GuestEscapes, AMachineWhoseEscapesAreAllOffWatchesNothing) {
    MockedVm m;
    const auto nothing = [](Vm&, std::uint32_t) {};

    m.machine.registerEscapes(retropp::escapes(
        GuestEscape{.key = "a", .at = 0x4000, .handler = nothing},
        GuestEscape{.key = "b", .at = 0x4100, .handler = nothing}));
    EXPECT_EQ(m.mock->armedCount(), 2u);

    m.machine.escapes()["a"].armed(false);
    m.machine.escapes()["b"].armed(false);

    // The declarations are all still there; what the machine is asked to watch is nothing at all.
    EXPECT_EQ(m.machine.escapes().size(), 2u);
    EXPECT_EQ(m.mock->armedCount(), 0u);
}

// Whether an escape is live is the host layer's answer, not the machine's. A machine is asked to stop
// watching a switched-off address as well, but that is an economy — if one reports the address anyway,
// the escape is still off and still does not run.
TEST(GuestEscapes, ASwitchedOffEscapeDoesNotFireEvenIfTheMachineReportsItAnyway) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    m.machine.escapes()["rng draw"].armed(false);
    m.mock->armEscape(0xC310, false);  // an over-reporting machine: watching what nobody asked for
    walk(*m.mock, {0xC310, 0xC310});

    EXPECT_EQ(fired, 0);
}

TEST(GuestEscapes, RemovingAnEscapeDropsItsDeclarationAndStopsWatching) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rng draw", .at = 0xC310, .handler = [&](Vm&, std::uint32_t) { ++fired; }}));

    m.machine.escapes()["rng draw"].remove();
    walk(*m.mock, {0xC310});

    EXPECT_EQ(fired, 0);
    EXPECT_EQ(m.machine.escapes().size(), 0u);
    EXPECT_FALSE(m.machine.escapes().contains("rng draw"));
    EXPECT_EQ(m.mock->armedCount(), 0u);
    EXPECT_THROW((void)m.machine.escapes()["rng draw"].armed(), std::out_of_range);
}

TEST(GuestEscapes, NamingAnEscapeThatWasNeverDeclaredThrows) {
    MockedVm m;
    m.machine.registerEscapes(retropp::escapes(
        GuestEscape{.key = "rng draw", .at = 0xC310, .handler = [](Vm&, std::uint32_t) {}}));

    EXPECT_THROW((void)m.machine.escapes()["rng drawe"].armed(), std::out_of_range);
    EXPECT_FALSE(m.machine.escapes().contains("rng drawe"));
}

TEST(GuestEscapes, AnEmptyBatchIsRefused) {
    MockedVm m;
    EXPECT_THROW(m.machine.registerEscapes(EscapeMap{}), std::invalid_argument);
}

TEST(GuestEscapes, AnEscapeWithNothingToRunIsRefused) {
    MockedVm m;
    EXPECT_THROW(m.machine.registerEscapes(
                     retropp::escapes(GuestEscape{.key = "silent", .at = 0xC310})),
                 std::invalid_argument);
    EXPECT_EQ(m.machine.escapes().size(), 0u);
}

TEST(GuestEscapes, ABatchReportsEveryFailureAtOnceEachByItsKey) {
    MockedVm   m;
    const auto nothing = [](Vm&, std::uint32_t) {};

    // Four distinct failures in one batch: no handler, an address past this machine's memory, and a
    // key and an address each declared twice within the batch.
    try {
        m.machine.registerEscapes(retropp::escapes(
            GuestEscape{.key = "no handler", .at = 0x4000},
            GuestEscape{.key = "unreachable", .at = 0x1FFFFF, .handler = nothing},
            GuestEscape{.key = "twice", .at = 0x5000, .handler = nothing},
            GuestEscape{.key = "twice", .at = 0x6000, .handler = nothing},
            GuestEscape{.key = "same address", .at = 0x5000, .handler = nothing}));
        FAIL() << "the batch should not have registered";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("no handler"), std::string::npos);
        EXPECT_NE(what.find("unreachable"), std::string::npos);
        EXPECT_NE(what.find("twice"), std::string::npos);
        EXPECT_NE(what.find("same address"), std::string::npos);
    }
    EXPECT_EQ(m.machine.escapes().size(), 0u);
}

TEST(GuestEscapes, AKeyAlreadyDeclaredOnThisMachineIsRefused) {
    MockedVm   m;
    const auto nothing = [](Vm&, std::uint32_t) {};

    m.machine.registerEscapes(
        retropp::escapes(GuestEscape{.key = "rng draw", .at = 0xC310, .handler = nothing}));
    EXPECT_THROW(m.machine.registerEscapes(retropp::escapes(GuestEscape{
                     .key = "rng draw", .at = 0xC400, .handler = nothing})),
                 std::invalid_argument);
    EXPECT_EQ(m.machine.escapes().size(), 1u);
}

TEST(GuestEscapes, AnAddressAlreadyEscapingOnThisMachineIsRefused) {
    MockedVm   m;
    const auto nothing = [](Vm&, std::uint32_t) {};

    m.machine.registerEscapes(
        retropp::escapes(GuestEscape{.key = "first", .at = 0xC310, .handler = nothing}));
    EXPECT_THROW(m.machine.registerEscapes(
                     retropp::escapes(GuestEscape{.key = "second", .at = 0xC310, .handler = nothing})),
                 std::invalid_argument);
    EXPECT_EQ(m.machine.escapes().size(), 1u);
}

// D-U2.7: a handler may declare another escape while it is running, and the surface carries no notion
// of how deep anything is. Nothing here asks, and nothing refuses.
TEST(GuestEscapes, AHandlerMayDeclareAnotherEscapeWhileItRuns) {
    MockedVm m;
    int      outer = 0;
    int      inner = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "outer", .at = 0x4000, .handler = [&](Vm& machine, std::uint32_t) {
            ++outer;
            if (!machine.escapes().contains("inner")) {
                machine.registerEscapes(retropp::escapes(GuestEscape{
                    .key = "inner", .at = 0x4100, .handler = [&](Vm&, std::uint32_t) { ++inner; }}));
            }
        }}));

    walk(*m.mock, {0x4000, 0x4100});

    EXPECT_EQ(outer, 1);
    EXPECT_EQ(inner, 1);
    EXPECT_EQ(m.machine.escapes().size(), 2u);
}

TEST(GuestEscapes, AHandlerMaySwitchItsOwnEscapeOff) {
    MockedVm m;
    int      fired = 0;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "once", .at = 0x4000, .handler = [&](Vm& machine, std::uint32_t) {
            ++fired;
            machine.escapes()["once"].armed(false);
        }}));

    walk(*m.mock, {0x4000, 0x4000, 0x4000});

    EXPECT_EQ(fired, 1);
}

TEST(GuestEscapes, AMachineThatCannotWatchAddressesIsLeftExactlyAsItWas) {
    MockedVm m;
    m.mock->refuseEscapes(true);

    EXPECT_THROW(m.machine.registerEscapes(retropp::escapes(
                     GuestEscape{.key = "a", .at = 0x4000, .handler = [](Vm&, std::uint32_t) {}},
                     GuestEscape{.key = "b", .at = 0x4100, .handler = [](Vm&, std::uint32_t) {}})),
                 std::logic_error);

    EXPECT_EQ(m.machine.escapes().size(), 0u);
    EXPECT_EQ(m.mock->armedCount(), 0u);
}

// ── The answering kind, on a machine with no CPU ────────────────────────────────────────────────
// A replacement's declaration, validation, arming and marshaling are the host layer's; the mock
// proves them with memory-convention bindings (this machine has no registers to bind).

TEST(GuestEscapes, AReplacementMarshalsItsArgumentsInAndItsAnswerOut) {
    MockedVm m;
    m.mock->bytes()[0xC000] = 5;   // what the guest's caller would have left for the routine
    m.mock->bytes()[0xC001] = 99;  // where its callers read the answer; anything but 6

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rule", .at = 0x4000,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::Location::memory(0xC000)},
                                    .output = retropp::Location::memory(0xC001)},
            [](std::uint8_t seed) -> std::uint8_t { return static_cast<std::uint8_t>(seed + 1); })}));

    walk(*m.mock, {0x4000});

    EXPECT_EQ(m.mock->bytes()[0xC001], 6);  // read 5 from the bound input, wrote 6 to the bound output
    EXPECT_TRUE(m.mock->isReplaced(0x4000));
}

TEST(GuestEscapes, SwitchingAReplacementOffReleasesTheRoutine) {
    MockedVm m;
    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rule", .at = 0x4000,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::Location::memory(0xC000)},
                                    .output = retropp::Location::memory(0xC001)},
            [](std::uint8_t seed) -> std::uint8_t { return seed; })}));
    EXPECT_TRUE(m.mock->isReplaced(0x4000));

    m.machine.escapes()["rule"].armed(false);
    EXPECT_FALSE(m.mock->isReplaced(0x4000));  // the routine is the guest's own again

    m.machine.escapes()["rule"].armed(true);
    EXPECT_TRUE(m.mock->isReplaced(0x4000));

    m.machine.escapes()["rule"].remove();
    EXPECT_FALSE(m.mock->isReplaced(0x4000));
}

TEST(GuestEscapes, AnEscapeDeclaringBothKindsIsRefused) {
    MockedVm   m;
    const auto nothing = [](Vm&, std::uint32_t) {};
    EXPECT_THROW(
        m.machine.registerEscapes(retropp::escapes(GuestEscape{
            .key = "both", .at = 0x4000, .handler = nothing,
            .replaces = retropp::routine(
                retropp::RoutineBinding{.output = retropp::Location::memory(0xC000)},
                []() -> std::uint8_t { return 1; })})),
        std::invalid_argument);
    EXPECT_EQ(m.machine.escapes().size(), 0u);
}

TEST(GuestEscapes, AReplacementBindingIsCheckedWithTheBatch) {
    MockedVm m;
    // Three distinct binding failures in one batch: an input count that does not match the function,
    // a register on a machine that has none, and a pacing declaration that means nothing here.
    try {
        m.machine.registerEscapes(retropp::escapes(
            GuestEscape{.key = "arity", .at = 0x4000,
                        .replaces = retropp::routine(
                            retropp::RoutineBinding{.inputs = {retropp::Location::memory(0xC000),
                                                               retropp::Location::memory(0xC001)},
                                                    .output = retropp::Location::memory(0xC002)},
                            [](std::uint8_t a) -> std::uint8_t { return a; })},
            GuestEscape{.key = "no such register", .at = 0x4100,
                        .replaces = retropp::routine(
                            retropp::RoutineBinding{.inputs = {retropp::Location::reg(7)},
                                                    .output = retropp::Location::memory(0xC002)},
                            [](std::uint8_t a) -> std::uint8_t { return a; })},
            GuestEscape{.key = "paced", .at = 0x4200,
                        .replaces = retropp::routine(
                            retropp::RoutineBinding{.output   = retropp::Location::memory(0xC003),
                                                    .throttle = retropp::Throttle::HardwareSpeed},
                            []() -> std::uint8_t { return 1; })}));
        FAIL() << "the batch should not have registered";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("arity"), std::string::npos);
        EXPECT_NE(what.find("no such register"), std::string::npos);
        EXPECT_NE(what.find("paced"), std::string::npos);
    }
    EXPECT_EQ(m.machine.escapes().size(), 0u);
}

// A machine that gives out PARTWAY through a batch is the case that makes the undo observable: the
// first address is being watched when the second is refused, and what the caller must not do is leave
// it watched for a batch that never registered.
TEST(GuestEscapes, AMachineThatGivesOutPartwayThroughABatchIsLeftWatchingNothing) {
    MockedVm   m;
    const auto nothing = [](Vm&, std::uint32_t) {};
    m.mock->acceptAtMost(1);

    EXPECT_THROW(m.machine.registerEscapes(retropp::escapes(
                     GuestEscape{.key = "a", .at = 0x4000, .handler = nothing},
                     GuestEscape{.key = "b", .at = 0x4100, .handler = nothing})),
                 std::logic_error);

    EXPECT_EQ(m.machine.escapes().size(), 0u);
    EXPECT_EQ(m.mock->armedCount(), 0u);
}

// A handler is handed the machine it belongs to, so a machine that moves hands over the address it
// now occupies. The game stores no handle of its own, so there is none to go stale.
TEST(GuestEscapes, MovingTheMachineHandsHandlersTheMachineWhereItNowLives) {
    MockedVm m;
    Vm*      handed = nullptr;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key     = "rng draw",
        .at      = 0xC310,
        .handler = [&](Vm& machine, std::uint32_t) { handed = &machine; }}));

    Vm moved = std::move(m.machine);
    walk(*m.mock, {0xC310});

    EXPECT_EQ(handed, &moved);
    EXPECT_EQ(moved.escapes().size(), 1u);
}

}  // namespace
