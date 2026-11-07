// A Game Boy machine's routines and the addresses they are assembled for, through public headers. Every
// routine here calls a routine of its own by address, so it answers 42 only when its bytes hold the
// address they landed at. Device-free: a placed routine's effect on a register is the same on any host.
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal paths
#include "retropp/driver_binding.h"
#include "retropp/gb.h"
#include "retropp/routine_registry.h"
#include "retropp/vm.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#ifndef RETROPP_FIXTURES_DIR
#define RETROPP_FIXTURES_DIR "."
#endif

namespace retropp {
namespace {

// One CGB frame's CPU-cycle budget — the per-tick unit the resident tick pads to.
const std::uint64_t kFrame = TimingProfile::GameBoyColor.cpuCyclesPerTick();

constexpr const char* kBody = "  call sub\n  ret\nsub:\n  ld a, $2A\n  ret\n";

constexpr std::array<std::uint8_t, 2> kAdd{0x80, 0xC9};  // add a,b ; ret

const RoutineBinding kAnswer{.output = gb::A};
const RoutineBinding kAddBinding{.inputs = {gb::A, gb::B}, .output = gb::A};

// The literal paths below are project-root-relative, so the asset root is the project's.
void useProjectRoot() {
    setAssetRoot(std::filesystem::path(RETROPP_FIXTURES_DIR).parent_path().parent_path());
}

auto uploadAdd(Vm& vm) {
    return vm.uploadRoutine<std::uint8_t(std::uint8_t, std::uint8_t)>(std::span<const std::uint8_t>(kAdd),
                                                                      kAddBinding);
}

auto upload(Vm& vm, const std::vector<std::uint8_t>& bytes) {
    return vm.uploadRoutine<std::uint8_t()>(std::span<const std::uint8_t>(bytes), kAnswer);
}

// A source that names no address is assembled for the first gap of the arena, which moves up as
// routines are placed: `sub` is four bytes in.
TEST(GbRoutineOrigin, AssembleAnswersBytesForTheArenasFirstGap) {
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_EQ(vm.assemble(kBody), (std::vector<std::uint8_t>{0xCD, 0x64, 0x01, 0xC9, 0x3E, 0x2A, 0xC9}));

    (void)uploadAdd(vm);
    EXPECT_EQ(vm.assemble(kBody), (std::vector<std::uint8_t>{0xCD, 0x66, 0x01, 0xC9, 0x3E, 0x2A, 0xC9}));
}

// Each assemble-then-upload pair lands at the gap its bytes were assembled for.
TEST(GbRoutineOrigin, BytesAssembledThenUploadedLandWhereTheyWereAssembledFor) {
    Vm vm{VMPlatform::GameBoyColor};
    auto first = upload(vm, vm.assemble(kBody));
    EXPECT_EQ(first(), 42);
    auto second = upload(vm, vm.assemble(kBody));
    EXPECT_EQ(second(), 42);
    EXPECT_EQ(first(), 42);
}

// A source's `ORG` is where it lands, and the arena below it stays free for the next routine.
TEST(GbRoutineOrigin, AnAsmThatNamesItsAddressLandsThere) {
    useProjectRoot();
    Vm vm{VMPlatform::GameBoyColor};
    auto routine = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0.asm",
                                                      {.output = gb::A}, AssetPolicy::LoadFromPath);
    EXPECT_EQ(routine(), 42);
    auto add = uploadAdd(vm);
    EXPECT_EQ(add(3, 4), 7);
    EXPECT_EQ(routine(), 42);
}

// A source that names no address lands in the first gap and resolves for it, whatever is placed already.
TEST(GbRoutineOrigin, AnAsmThatNamesNoAddressLandsWhereItFits) {
    useProjectRoot();
    Vm vm{VMPlatform::GameBoyColor};
    auto before = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_where_it_lands.asm",
                                                     {.output = gb::A}, AssetPolicy::LoadFromPath);
    EXPECT_EQ(before(), 42);
    auto add = uploadAdd(vm);
    EXPECT_EQ(add(3, 4), 7);
    auto after = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_where_it_lands.asm",
                                                    {.output = gb::A}, AssetPolicy::LoadFromPath);
    EXPECT_EQ(after(), 42);
    EXPECT_EQ(before(), 42);
}

// An `ORG` below the arena, or one whose bytes run past its end, is refused with nothing placed.
TEST(GbRoutineOrigin, AnOrgOutsideTheArenaIsRefused) {
    useProjectRoot();
    Vm vm{VMPlatform::GameBoyColor};
    EXPECT_THROW((void)vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/org_outside_the_arena.asm",
                                                          {.output = gb::A}, AssetPolicy::LoadFromPath),
                 std::invalid_argument);
    auto add = uploadAdd(vm);
    EXPECT_EQ(add(3, 4), 7);
    EXPECT_THROW((void)vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/org_past_the_arenas_end.asm",
                                                          {.output = gb::A}, AssetPolicy::LoadFromPath),
                 std::invalid_argument);
    auto second = uploadAdd(vm);
    EXPECT_EQ(second(3, 4), 7);
    EXPECT_EQ(add(5, 6), 11);
}

// Two routines never share a byte: the second at the same `ORG` is refused and the first stands.
TEST(GbRoutineOrigin, AnOrgOverAPlacedRoutineIsRefused) {
    useProjectRoot();
    Vm vm{VMPlatform::GameBoyColor};
    auto routine = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0.asm",
                                                      {.output = gb::A}, AssetPolicy::LoadFromPath);
    EXPECT_THROW((void)vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0.asm",
                                                          {.output = gb::A}, AssetPolicy::LoadFromPath),
                 std::invalid_argument);
    EXPECT_EQ(routine(), 42);
}

// A baked routine carries its `ORG` to placement; one whose source names none carries nothing.
TEST(GbRoutineOrigin, TheEmbedBakeCarriesTheOrg) {
    Vm vm{VMPlatform::GameBoyColor};
    auto routine = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0_embedded.asm",
                                                      {.output = gb::A});
    EXPECT_EQ(routine(), 42);

    const std::optional<detail::EmbeddedPlacement> placement =
        detail::findEmbeddedRoutinePlacement("tests/fixtures/routines/call_at_01a0_embedded.asm");
    ASSERT_TRUE(placement.has_value());
    EXPECT_EQ(placement->isa, Isa::Sm83);
    EXPECT_EQ(placement->origin, 0x01A0u);

    EXPECT_FALSE(detail::findEmbeddedRoutinePlacement("tests/fixtures/routines/add_ab.asm").has_value());
}

// Hosting a driver rebuilds the cartridge image, and a routine at its `ORG` keeps its bytes through it.
// A driver image cannot be placed over that routine.
TEST(GbRoutineOrigin, ARoutinePlacedAtAnOrgSurvivesAHostedDriver) {
    useProjectRoot();
    static constexpr std::array<std::uint8_t, 6> kTick{0x3E, 0x37, 0xEA, 0x00, 0xC0, 0xC9};
    {
        Vm vm{VMPlatform::GameBoyColor};
        auto routine = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0.asm",
                                                          {.output = gb::A}, AssetPolicy::LoadFromPath);
        EXPECT_EQ(routine(), 42);

        DriverBinding b;
        b.images = {DriverImage{.bytes = std::span<const std::uint8_t>(kTick), .base = 0x6000}};
        b.tickEntry = 0x6000;
        b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};
        vm.hostDriver(b);

        EXPECT_EQ(routine(), 42);
        vm.tickDriver({}, kFrame);
        EXPECT_EQ(vm.readSlot(0), 0x37u);
    }
    {
        Vm vm{VMPlatform::GameBoyColor};
        auto routine = vm.registerRoutine<std::uint8_t()>("tests/fixtures/routines/call_at_01a0.asm",
                                                          {.output = gb::A}, AssetPolicy::LoadFromPath);
        DriverBinding b;
        b.images = {DriverImage{.bytes = std::span<const std::uint8_t>(kTick), .base = 0x01A0}};
        b.tickEntry = 0x01A0;
        EXPECT_THROW(vm.hostDriver(b), std::invalid_argument);
        EXPECT_EQ(routine(), 42);
    }
}

// A banked image's source names the window address the CPU sees it at, and the bank is the placement's:
// the fixed-bank stub maps bank 2 and calls into it, and the image's own `call` lands inside the window.
TEST(GbRoutineOrigin, ABankedImagesSourceNamesTheWindowAddress) {
    Vm vm{VMPlatform::GameBoyColor};
    // ld a,$02 ; ld [$2000],a ; call $4000 ; ret
    static constexpr std::array<std::uint8_t, 9> kStub{0x3E, 0x02, 0xEA, 0x00, 0x20, 0xCD, 0x00, 0x40, 0xC9};
    const std::vector<std::uint8_t> banked =
        vm.assemble("  ORG $4000\n  call sub\n  ld [$C000], a\n  ret\nsub:\n  ld a, $5A\n  ret\n");

    DriverBinding b;
    b.mapper = gb::Mbc3;
    b.images = {DriverImage{.bytes = std::span<const std::uint8_t>(kStub), .base = 0x0A00},
                DriverImage{.bytes = std::span<const std::uint8_t>(banked), .base = gb::banked(2, 0x4000)}};
    b.tickEntry = 0x0A00;
    b.slots = {SlotSpec{.address = 0xC000, .width = 1, .direction = SlotDirection::Read}};

    vm.hostDriver(b);
    vm.tickDriver({}, kFrame);
    EXPECT_EQ(vm.readSlot(0), 0x5Au);
}

}  // namespace
}  // namespace retropp
