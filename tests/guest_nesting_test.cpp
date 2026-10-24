// Calling the machine's own routines, on a machine with no CPU.
//
// Every case here runs against MockVmBackend — a flat byte array, a scripted walk, and a call that
// records the ask it was made with and runs whatever the test says the routine does. What is being
// pinned is that binding a routine, validating it, marshaling a call in each direction, choosing
// whose stack the frame belongs on and tolerating any depth at all are the HOST layer's behavior:
// they work with no console core underneath, so no part of them can have been designed around one
// core's capabilities. What only a CPU can answer is in sameboy_nesting_test.cpp.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/location.h"
#include "retropp/vm.h"
#include "src/vm/vm_backend.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::GuestEscape;
using retropp::Location;
using retropp::RoutineBinding;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
using retropp::vm::CallStack;
using retropp::vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for the walk and for saying what a called
// routine does.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoyColor};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

// ── The call, in both directions ────────────────────────────────────────────────────────────────

// The arguments reach the homes the binding names before the routine runs, and the answer is read
// out of the home it names after — which is the whole of what a binding promises.
TEST(GuestNesting, ABoundRoutineMarshalsItsArgumentsInAndItsAnswerOut) {
    MockedVm m;
    m.mock->bytes()[0xC001] = 99;  // anything but the answer

    // What the routine at $4000 does: read its input home, leave three times it in its output home.
    m.mock->onContextCall([&](std::uint32_t) {
        const std::uint8_t in = m.mock->bytes()[0xC000];
        m.mock->bytes()[0xC001] = static_cast<std::uint8_t>(in * 3);
    });

    auto triple = m.machine.bindRoutine<std::uint8_t(std::uint8_t)>(
        0x4000, RoutineBinding{.inputs = {Location::memory(0xC000)},
                               .output = Location::memory(0xC001)});

    EXPECT_EQ(triple(7), 21);
    EXPECT_EQ(m.mock->bytes()[0xC000], 7);  // the argument was written where the routine looks
    ASSERT_EQ(m.mock->contextCalls().size(), 1u);
    EXPECT_EQ(m.mock->contextCalls().at(0).entry, 0x4000u);
}

// A routine with nothing to say still runs, and the machine is asked for it exactly once.
TEST(GuestNesting, ARoutineThatReturnsNothingStillRuns) {
    MockedVm m;
    int ran = 0;
    m.mock->onContextCall([&](std::uint32_t) { ++ran; });

    auto tick = m.machine.bindRoutine<void()>(0x4400, RoutineBinding{});
    tick();
    tick();

    EXPECT_EQ(ran, 2);
    EXPECT_EQ(m.mock->contextCalls().size(), 2u);
}

// ── What is refused, and when ───────────────────────────────────────────────────────────────────

TEST(GuestNesting, ARegisterBindingOnAMachineWithNoRegistersIsRefused) {
    MockedVm m;
    EXPECT_THROW((void)m.machine.bindRoutine<std::uint8_t()>(
                     0x4000, RoutineBinding{.output = Location::reg(7)}),
                 std::invalid_argument);
}

TEST(GuestNesting, PacingAndAnEntryOffsetAreRefused) {
    MockedVm m;
    EXPECT_THROW((void)m.machine.bindRoutine<void()>(
                     0x4000, RoutineBinding{.throttle = retropp::Throttle::HardwareSpeed}),
                 std::invalid_argument);
    EXPECT_THROW((void)m.machine.bindRoutine<void()>(0x4000, RoutineBinding{.entryOffset = 4}),
                 std::invalid_argument);
}

TEST(GuestNesting, AnAddressThisMachineCannotReachIsRefused) {
    MockedVm m;
    EXPECT_THROW((void)m.machine.bindRoutine<void()>(0x1FFFFF, RoutineBinding{}),
                 std::invalid_argument);
}

TEST(GuestNesting, ABindingThatDoesNotMatchTheSignatureIsRefused) {
    MockedVm m;
    // Two inputs declared for a function taking one.
    EXPECT_THROW((void)m.machine.bindRoutine<std::uint8_t(std::uint8_t)>(
                     0x4000, RoutineBinding{.inputs = {Location::memory(0xC000),
                                                       Location::memory(0xC001)},
                                            .output = Location::memory(0xC002)}),
                 std::invalid_argument);
    // A value-returning signature with nowhere to put the value.
    EXPECT_THROW((void)m.machine.bindRoutine<std::uint8_t()>(0x4000, RoutineBinding{}),
                 std::invalid_argument);
    // A void signature with somewhere to put one.
    EXPECT_THROW((void)m.machine.bindRoutine<void()>(
                     0x4000, RoutineBinding{.output = Location::memory(0xC000)}),
                 std::invalid_argument);
}

// ── Whose stack the call goes on ────────────────────────────────────────────────────────────────

// A machine at rest has no guest to give anything back to: the call goes on the engine's own stack.
TEST(GuestNesting, ACallOnAMachineAtRestGoesOnTheScratchStack) {
    MockedVm m;
    auto     tick = m.machine.bindRoutine<void()>(0x4400, RoutineBinding{});

    tick();

    ASSERT_EQ(m.mock->contextCalls().size(), 1u);
    EXPECT_EQ(m.mock->contextCalls().at(0).stack, CallStack::Scratch);
}

// An escape holds a guest mid-instruction, with a live stack the routine's frame belongs on.
TEST(GuestNesting, ACallFromInsideAnEscapeGoesOnTheGuestsOwnStack) {
    MockedVm m;
    auto     tick = m.machine.bindRoutine<void()>(0x4400, RoutineBinding{});

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "here", .at = 0x5000, .handler = [&](Vm&, std::uint32_t) { tick(); }}));

    m.mock->walk(std::vector<std::uint32_t>{0x5000});

    ASSERT_EQ(m.mock->contextCalls().size(), 1u);
    EXPECT_EQ(m.mock->contextCalls().at(0).stack, CallStack::Guest);
}

// And the machine goes back to having no guest context once the escape is done with it.
TEST(GuestNesting, TheStackChoiceGoesBackToScratchAfterTheEscapeIsDone) {
    MockedVm m;
    auto     tick = m.machine.bindRoutine<void()>(0x4400, RoutineBinding{});

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "here", .at = 0x5000, .handler = [&](Vm&, std::uint32_t) { tick(); }}));

    m.mock->walk(std::vector<std::uint32_t>{0x5000});
    tick();

    ASSERT_EQ(m.mock->contextCalls().size(), 2u);
    EXPECT_EQ(m.mock->contextCalls().at(0).stack, CallStack::Guest);
    EXPECT_EQ(m.mock->contextCalls().at(1).stack, CallStack::Scratch);
}

// ── Depth ───────────────────────────────────────────────────────────────────────────────────────

// A handler calls a routine; that routine's own code escapes, and THAT handler calls another
// routine. Three calls deep, each answering its own caller, with nothing anywhere counting how far
// in it is. Every frame is on the guest's stack, because at every one of them there is a guest.
TEST(GuestNesting, AHandlersRoutineMayEscapeIntoAHandlerThatCallsAnotherRoutine) {
    MockedVm m;

    auto inner = m.machine.bindRoutine<std::uint8_t()>(
        0x4300, RoutineBinding{.output = Location::memory(0xC003)});
    auto outer = m.machine.bindRoutine<std::uint8_t()>(
        0x4200, RoutineBinding{.output = Location::memory(0xC002)});

    std::uint8_t innerAnswer = 0;
    std::uint8_t outerAnswer = 0;
    std::vector<std::uint32_t> entries;

    // What each routine's own code does. The outer one runs an instruction that escapes; the inner
    // one simply answers.
    m.mock->onContextCall([&](std::uint32_t entry) {
        entries.push_back(entry);
        if (entry == 0x4200) {
            m.mock->walk(std::vector<std::uint32_t>{0x6000});  // the routine's body, escaping
            m.mock->bytes()[0xC002] = static_cast<std::uint8_t>(innerAnswer * 2);
        } else if (entry == 0x4300) {
            m.mock->bytes()[0xC003] = 7;
        }
    });

    m.machine.registerEscapes(retropp::escapes(
        GuestEscape{.key = "outermost", .at = 0x5000,
                    .handler = [&](Vm&, std::uint32_t) { outerAnswer = outer(); }},
        GuestEscape{.key = "inside the routine", .at = 0x6000,
                    .handler = [&](Vm&, std::uint32_t) { innerAnswer = inner(); }}));

    m.mock->walk(std::vector<std::uint32_t>{0x5000});

    EXPECT_EQ(innerAnswer, 7);
    EXPECT_EQ(outerAnswer, 14);
    EXPECT_EQ(entries, (std::vector<std::uint32_t>{0x4200, 0x4300}));
    ASSERT_EQ(m.mock->contextCalls().size(), 2u);
    EXPECT_EQ(m.mock->contextCalls().at(0).stack, CallStack::Guest);
    EXPECT_EQ(m.mock->contextCalls().at(1).stack, CallStack::Guest);
}

// ── A declaration survives the code declared in it ──────────────────────────────────────────────

// A replacement's function may declare escapes of its own while it runs — and the declaration it
// itself lives in is still there to receive its answer when it returns. Thirty-two declarations is
// far past what any growth step would absorb, so a table that relocated its entries would be
// relocating this one out from under the call in flight.
TEST(GuestNesting, AReplacementThatDeclaresEscapesStillDeliversItsAnswer) {
    MockedVm m;
    m.mock->bytes()[0xC000] = 5;
    m.mock->bytes()[0xC001] = 99;

    m.machine.registerEscapes(retropp::escapes(GuestEscape{
        .key = "rule", .at = 0x4000,
        .replaces = retropp::routine(
            RoutineBinding{.inputs = {Location::memory(0xC000)},
                           .output = Location::memory(0xC001)},
            [&](std::uint8_t seed) -> std::uint8_t {
                for (int i = 0; i < 32; ++i) {
                    m.machine.registerEscapes(retropp::escapes(GuestEscape{
                        .key     = "extra " + std::to_string(i),
                        .at      = 0x7000u + static_cast<std::uint32_t>(i),
                        .handler = [](Vm&, std::uint32_t) {}}));
                }
                return static_cast<std::uint8_t>(seed + 1);
            })}));

    m.mock->walk(std::vector<std::uint32_t>{0x4000});

    EXPECT_EQ(m.mock->bytes()[0xC001], 6);
    EXPECT_EQ(m.machine.escapes().size(), 33u);
}

}  // namespace
