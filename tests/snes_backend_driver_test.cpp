// A resident driver on the SNES core, exercised through the public surface only — Vm, snes.h and
// driver_binding.h. Each case hosts a driver built from 65816 source assembled here at the address it is
// placed at, drives the resident tick, and observes the result through a declared slot in work RAM.
//
// What hosting has to get right on this console, and the cases below pin one at a time: a flat driver in
// one LoROM bank; a HiROM image, whose header is at a site of its own, reaching a byte in a high bank; a
// placement past the first bank needs a mapper; the stack the binding names is the stack the entries push
// on; two images never share a byte, and none takes the idle loop, the header or an address no ROM is at;
// a tick's cost is the machine's own master cycles, and a tick that never returns is stopped near the
// frame's budget; a mailbox write lands before the tick reads it, a call rides its value in a register,
// the declared init runs once at host; and a routine placed before the driver stays callable beside it.

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "retropp/driver_binding.h"
#include "retropp/snes.h"
#include "retropp/vm.h"

namespace retropp {
namespace {

// One frame of a 60 Hz console, in master cycles — the budget a resident tick pads to.
constexpr std::uint64_t kFrame = 357'366;

// Where the cases keep their slots and mailboxes: work RAM, reached from the entries through the direct
// page, which the engine's frame sets to $0000.
constexpr std::uint32_t kSlot    = 0x7E0010;
constexpr std::uint32_t kMailbox = 0x7E0020;

// 65816 source assembled at the address its ORG names, as the bytes from its first byte to its last.
std::vector<std::uint8_t> code(std::string_view source) {
    const snaggletooth::assembler::Assembly assembly =
        snaggletooth::assembler::assembleCpu65816(source, "driver.asm");
    EXPECT_TRUE(assembly.ok()) << (assembly.errors.empty() ? "" : assembly.errors.front().message);
    const std::uint32_t first = assembly.ranges.front().start;
    const std::uint32_t end =
        assembly.ranges.back().start + static_cast<std::uint32_t>(assembly.ranges.back().bytes.size());
    return *snaggletooth::assembler::image(assembly, first, end - first);
}

DriverImage image(const std::vector<std::uint8_t>& bytes, std::uint32_t base) {
    return DriverImage{.bytes = std::span<const std::uint8_t>(bytes), .base = base};
}

// A binding for this console: the 65816's instruction set, the tick at `tick`.
DriverBinding binding(std::vector<DriverImage> images, std::uint32_t tick) {
    DriverBinding b;
    b.images    = std::move(images);
    b.tickEntry = tick;
    b.isa       = Isa::Wdc65816;
    return b;
}

SlotSpec slotAt(std::uint32_t address, SlotDirection direction = SlotDirection::Read) {
    return SlotSpec{.address = address, .width = 1, .direction = direction};
}

// ── Placement + tick ──────────────────────────────────────────────────────────────────────────────

// A flat driver: one image in the first LoROM bank, a tick that stores a constant to a slot.
TEST(SnesDriverHosting, AFlatDriverPlacesAndTicks) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> tick = code("        ORG $00:8000\n"
                                                "        A8\n"
                                                "        LDA #$37\n"
                                                "        STA $10\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(tick, 0x008000)}, 0x008000);
    b.slots         = {slotAt(kSlot)};

    vm.hostDriver(b);
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x37u);
}

// A HiROM driver: the tick sits at $C0:7FC0 — where a LoROM image keeps its header, and plain code on a
// HiROM one, whose header is at $FFC0 — and reads a byte placed in bank $C1 with a long load.
TEST(SnesDriverHosting, AHiRomDriverReachesAHighBank) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> tick = code("        ORG $C0:7FC0\n"
                                                "        A8\n"
                                                "        LDA $C1:0000\n"
                                                "        STA $10\n"
                                                "        RTS\n");
    const std::vector<std::uint8_t> data{0xAB};
    DriverBinding b = binding({image(tick, 0xC07FC0), image(data, 0xC10000)}, 0xC07FC0);
    b.mapper        = snes::HiRom;
    b.slots         = {slotAt(kSlot)};

    vm.hostDriver(b);
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0xABu);  // the byte bank $C1 holds, read by code in bank $C0
}

// A placement past the first bank with the none mapper is a declaration error: the none mapper is one
// 32 KB LoROM bank.
TEST(SnesDriverHosting, APlacementPastTheFirstBankWithoutAMapperThrows) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> data{0xAB};
    const DriverBinding b = binding({image(data, 0x018000)}, 0x008000);
    EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
}

// The declared stack top is where the entries push: a tick that calls a subroutine works on the relocated
// stack, and the default top ($1FFF) — marked by a queued write before the tick — is left as it was.
TEST(SnesDriverHosting, TheDeclaredStackTopIsUsedAndTheDefaultIsSpared) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> tick = code("        ORG $00:8000\n"
                                                "        A8\n"
                                                "        JSR !sub\n"
                                                "        STA $10\n"
                                                "        RTS\n"
                                                "sub:    LDA #$5A\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(tick, 0x008000)}, 0x008000);
    b.stackTop      = 0x1F00;
    b.slots         = {slotAt(kSlot), slotAt(0x7E1FFF, SlotDirection::ReadWrite)};

    vm.hostDriver(b);
    // Mark the default top; a call pushed there would overwrite it with the landing's high byte.
    const std::array<Instruction, 1> queued{Instruction::write(Location::memory(0x7E1FFF), 1, 0x99)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);

    EXPECT_EQ(vm.readSlot(0), 0x5Au);  // the JSR / RTS on the relocated stack ran
    EXPECT_EQ(vm.readSlot(1), 0x99u);  // and the default top is untouched
}

// ── Placement validation ──────────────────────────────────────────────────────────────────────────

TEST(SnesDriverHosting, OverlappingImagesThrow) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> a(0x100, 0xEA);
    const std::vector<std::uint8_t> b(0x100, 0xEA);
    EXPECT_THROW(vm.hostDriver(binding({image(a, 0x008000), image(b, 0x008080)}, 0x008000)),
                 std::invalid_argument);
}

// The image keeps its idle loop and its header; neither can take a driver's byte, and an address no ROM is
// at — work RAM — places nothing.
TEST(SnesDriverHosting, TheIdleLoopTheHeaderAndWorkRamRefuseAnImage) {
    const std::vector<std::uint8_t> bytes{0xEA, 0xEA, 0xEA, 0x60};
    const auto hostAt = [&](std::uint32_t base, Mapper mapper) {
        Vm            vm{VMPlatform::Snes};
        DriverBinding b = binding({image(bytes, base)}, base);
        b.mapper        = mapper;
        vm.hostDriver(b);
    };
    EXPECT_THROW(hostAt(0x00FFC0, Mapper{}), std::invalid_argument);      // the LoROM header
    EXPECT_THROW(hostAt(0x00FFAE, Mapper{}), std::invalid_argument);      // across the idle loop
    EXPECT_THROW(hostAt(0xC0FFC0, snes::HiRom), std::invalid_argument);   // the HiROM header
    EXPECT_THROW(hostAt(0x7E0000, Mapper{}), std::invalid_argument);      // work RAM
}

// ── Cycle accounting ──────────────────────────────────────────────────────────────────────────────

// The tick answers the master cycles its entry spent, and never more than the frame it pads to; a longer
// entry costs more.
TEST(SnesDriverHosting, ATicksCostIsItsOwnAndStaysUnderTheFrame) {
    const auto tickCost = [](std::string_view source) {
        Vm                              vm{VMPlatform::Snes};
        const std::vector<std::uint8_t> tick = code(source);
        vm.hostDriver(binding({image(tick, 0x008000)}, 0x008000));
        return vm.tickDriver({}, kFrame);
    };
    const std::uint64_t shortCost = tickCost("        ORG $00:8000\n"
                                             "        RTS\n");
    const std::uint64_t longCost  = tickCost("        ORG $00:8000\n"
                                             "        NOP\n        NOP\n        NOP\n        NOP\n"
                                             "        NOP\n        NOP\n        NOP\n        NOP\n"
                                             "        RTS\n");
    EXPECT_GT(shortCost, 0u);
    EXPECT_LT(longCost, kFrame);     // a small tick leaves the rest of the frame to idle
    EXPECT_GT(longCost, shortCost);  // the eight NOPs are counted
}

// A tick that never returns is stopped near the budget it was given rather than spinning on.
TEST(SnesDriverHosting, ATickThatNeverReturnsIsStoppedNearItsBudget) {
    Vm                              vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> spin = code("        ORG $00:8000\n"
                                                "spin:   BRA spin\n");
    vm.hostDriver(binding({image(spin, 0x008000)}, 0x008000));

    const std::uint64_t budget = 1000;
    const std::uint64_t spent  = vm.tickDriver({}, budget);  // returns: the guard stops it
    EXPECT_GE(spent, budget);
    EXPECT_LT(spent, budget * 2);  // near the budget: the guard is its twelfth, in instructions
}

// ── Queued instructions ───────────────────────────────────────────────────────────────────────────

// A queued write lands in a mailbox before the tick, which reads it and acts on it.
TEST(SnesDriverHosting, AQueuedWriteFeedsAMailboxTheTickActsOn) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> tick = code("        ORG $00:8000\n"
                                                "        A8\n"
                                                "        LDA $20\n"
                                                "        ASL A\n"
                                                "        STA $10\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(tick, 0x008000)}, 0x008000);
    b.slots         = {slotAt(kSlot), slotAt(kMailbox, SlotDirection::Write)};
    vm.hostDriver(b);

    const std::array<Instruction, 1> queued{Instruction::write(Location::memory(kMailbox), 1, 5)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);
    EXPECT_EQ(vm.readSlot(0), 10u);  // the tick doubled what the write delivered
}

// A queued call rides its value in a register into an entry the engine calls.
TEST(SnesDriverHosting, AQueuedCallRidesItsValueInARegister) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> entry = code("        ORG $00:8000\n"
                                                 "        A8\n"
                                                 "        STA $10\n"
                                                 "        RTS\n");
    const std::vector<std::uint8_t> tick = code("        ORG $00:8010\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(entry, 0x008000), image(tick, 0x008010)}, 0x008010);
    b.slots         = {slotAt(kSlot)};
    vm.hostDriver(b);

    const std::array<Instruction, 1> queued{Instruction::call(0x008000, snes::A, 0x2A)};
    vm.tickDriver(std::span<const Instruction>(queued), kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x2Au);  // the entry stored the accumulator the call set
}

// The declared init is performed once, at host, before any tick.
TEST(SnesDriverHosting, TheInitRunsOnceAtHost) {
    Vm vm{VMPlatform::Snes};
    const std::vector<std::uint8_t> tick = code("        ORG $00:8000\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(tick, 0x008000)}, 0x008000);
    b.slots         = {slotAt(kSlot)};
    b.init          = Instruction::write(Location::memory(kSlot), 1, 0xC3);

    vm.hostDriver(b);
    EXPECT_EQ(vm.readSlot(0), 0xC3u);  // no tick has run
}

// ── The instruction value rule + host preconditions ───────────────────────────────────────────────

// A fixed value is what the gesture carries whatever the performer passes; an open one carries the
// performer's.
TEST(SnesDriverHosting, AFixedValueWinsAndAnOpenOneCarriesThePerformers) {
    const Instruction fixed = Instruction::write(Location::memory(kMailbox), 1, 7);
    const Instruction open  = Instruction::write(Location::memory(kMailbox), 1);
    EXPECT_EQ(fixed.valueFor(99), 7u);
    EXPECT_EQ(open.valueFor(99), 99u);
}

// Ticking or reading a slot before a driver is hosted is a usage error.
TEST(SnesDriverHosting, TickAndReadSlotThrowWithoutAHostedDriver) {
    Vm vm{VMPlatform::Snes};
    EXPECT_THROW(vm.tickDriver({}, kFrame), std::logic_error);
    EXPECT_THROW((void)vm.readSlot(0), std::logic_error);
}

// A routine placed before the driver stays where it was placed and callable beside it.
TEST(SnesDriverHosting, ARoutinePlacedFirstStaysCallableBesideTheDriver) {
    Vm                                   vm{VMPlatform::Snes};
    static constexpr std::array<std::uint8_t, 2> kIncrement{0x1A, 0x60};  // INC A ; RTS
    auto increment = vm.uploadRoutine<std::uint8_t(std::uint8_t)>(
        std::span<const std::uint8_t>(kIncrement),
        RoutineBinding{.inputs = {snes::A}, .output = snes::A, .isa = Isa::Wdc65816});
    EXPECT_EQ(increment(3), 4);

    const std::vector<std::uint8_t> tick = code("        ORG $00:9000\n"
                                                "        A8\n"
                                                "        LDA #$37\n"
                                                "        STA $10\n"
                                                "        RTS\n");
    DriverBinding b = binding({image(tick, 0x009000)}, 0x009000);
    b.slots         = {slotAt(kSlot)};
    vm.hostDriver(b);

    EXPECT_EQ(increment(5), 6);  // the routine's bytes are in the image the driver was placed in
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x37u);  // and the driver ticks
}

}  // namespace
}  // namespace retropp
