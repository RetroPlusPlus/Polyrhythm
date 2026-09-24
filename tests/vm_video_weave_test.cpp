// Weaving an interlaced picture, on a machine with no PPU.
//
// Every case here runs against MockVmBackend, whose finishFrame stands where a real core's vblank is and
// hands over a whole frame or a field of either parity. What is being pinned is that weaving is the HOST
// layer's: which field goes where, what the first field fills, what a whole frame does to the held fields,
// how the two weaves differ, and that an unset option keeps the held value — all of it with no console
// core underneath. Where the settings land on a RUNNING machine is pinned on a real one, in
// snes_vm_test.cpp, because a machine has to be hosting a cartridge to run.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/vm.h"
#include "src/vm/vm_backend.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::RasterContent;
using retropp::VideoOptions;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::Weave;
using retropp::testing::MockVmBackend;
using retropp::vm::FrameField;
using retropp::vm::VmTestAccess;

// A Vm driven by the mock, with the mock still reachable for finishing frames.
struct MockedVm {
    Vm             machine{VMPlatform::GameBoy};
    MockVmBackend* mock = nullptr;

    MockedVm() {
        auto owned = std::make_unique<MockVmBackend>();
        mock       = owned.get();
        VmTestAccess::substituteBackend(machine, std::move(owned));
    }
};

constexpr int kWidth  = 4;
constexpr int kHeight = 2;   // a field's height; a woven picture is 4 lines

// A field whose line y is every byte `base + y`, so a line landing at the wrong parity is caught and
// two lines read backwards are not the two lines read forwards.
std::vector<std::uint8_t> fieldOf(std::uint8_t base) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int i = 0; i < kWidth * 4; ++i) {
            pixels[static_cast<std::size_t>(y) * kWidth * 4 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(base + y);
        }
    }
    return pixels;
}

// The first byte of line `line` of the picture the machine answers with.
std::uint8_t lineByte(const RasterContent& picture, int line) {
    return picture.pixels[static_cast<std::size_t>(line) * picture.width * 4];
}

constexpr std::uint8_t kEven = 10;   // the even field's lines are 10, 11
constexpr std::uint8_t kOdd  = 20;   // the odd field's lines are 20, 21

TEST(VmVideoWeave, InterlacingOffPassesAFieldThroughAsItComes) {
    MockedVm m;
    m.machine.video(true);
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    EXPECT_EQ(picture.height, kHeight);
    EXPECT_EQ(lineByte(picture, 0), kEven);
    EXPECT_EQ(lineByte(picture, 1), kEven + 1);
}

// Line y of the field of parity p is line 2y + p of the picture, and the picture holds the newest field
// of each parity.
TEST(VmVideoWeave, AStraightWeaveLandsEachFieldsLinesAtItsParity) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true}});
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    ASSERT_EQ(picture.height, 2 * kHeight);
    EXPECT_EQ(picture.width, kWidth);
    EXPECT_EQ(lineByte(picture, 0), kEven);
    EXPECT_EQ(lineByte(picture, 1), kOdd);
    EXPECT_EQ(lineByte(picture, 2), kEven + 1);
    EXPECT_EQ(lineByte(picture, 3), kOdd + 1);
}

// Before the other field has arrived, the first fills both parities, so no line shows nothing.
TEST(VmVideoWeave, TheFirstFieldFillsBothParities) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true}});
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    ASSERT_EQ(picture.height, 2 * kHeight);
    EXPECT_EQ(lineByte(picture, 0), kOdd);
    EXPECT_EQ(lineByte(picture, 1), kOdd);
    EXPECT_EQ(lineByte(picture, 2), kOdd + 1);
    EXPECT_EQ(lineByte(picture, 3), kOdd + 1);
}

// Blend: each line of the straight weave averaged with the line below it, the last with itself.
TEST(VmVideoWeave, ABlendAveragesEachLineWithTheOneBelow) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true, .type = Weave::Blend}});
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    ASSERT_EQ(picture.height, 2 * kHeight);
    EXPECT_EQ(lineByte(picture, 0), (kEven + kOdd) / 2);            // 10, 20 → 15
    EXPECT_EQ(lineByte(picture, 1), (kOdd + kEven + 1) / 2);        // 20, 11 → 15
    EXPECT_EQ(lineByte(picture, 2), (kEven + 1 + kOdd + 1) / 2);    // 11, 21 → 16
    EXPECT_EQ(lineByte(picture, 3), kOdd + 1);                      // 21 with itself
}

// A whole frame passes through as it is, whatever the settings say, and drops the fields the host was
// holding: the next field starts the picture over and fills both parities.
TEST(VmVideoWeave, AWholeFrameUnderInterlacingPassesThroughAndDropsTheHeldFields) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true}});
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    m.mock->finishFrame(fieldOf(30), kWidth, kHeight, FrameField::Whole);
    m.machine.advanceTick();
    const RasterContent whole = m.machine.video();
    EXPECT_EQ(whole.height, kHeight);
    EXPECT_EQ(lineByte(whole, 0), 30);
    EXPECT_EQ(lineByte(whole, 1), 31);

    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    const RasterContent again = m.machine.video();
    ASSERT_EQ(again.height, 2 * kHeight);
    EXPECT_EQ(lineByte(again, 1), kEven);   // the odd line is the even field's, not the odd field held before
}

// A field of another size rebuilds the picture at that size; the field fills both parities.
TEST(VmVideoWeave, ASizeChangeRebuildsTheWovenPicture) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true}});
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();

    constexpr int kNarrow = 2;
    std::vector<std::uint8_t> narrow(static_cast<std::size_t>(kNarrow) * kHeight * 4, kOdd);
    m.mock->finishFrame(narrow, kNarrow, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    EXPECT_EQ(picture.width, kNarrow);
    ASSERT_EQ(picture.height, 2 * kHeight);
    EXPECT_EQ(picture.pixels.size(), static_cast<std::size_t>(kNarrow) * 2 * kHeight * 4);
    EXPECT_EQ(lineByte(picture, 0), kOdd);   // both parities from the field that rebuilt it
    EXPECT_EQ(lineByte(picture, 1), kOdd);
}

// A field is a frame the machine finished: each advances the generation by one.
TEST(VmVideoWeave, EachFieldAdvancesTheGeneration) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true}});
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    EXPECT_EQ(m.machine.video().generation, 1u);
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();
    EXPECT_EQ(m.machine.video().generation, 2u);
}

// An option left unset keeps the value the machine holds: the weave chosen once survives interlacing
// being switched off and on, and video(true) alone changes nothing.
TEST(VmVideoWeave, AnUnsetOptionKeepsTheHeldValue) {
    MockedVm m;
    m.machine.video(true, {.interlacing = {.on = true, .type = Weave::Blend}});
    m.machine.video(true, {.interlacing = {.on = false}});
    m.machine.video(true, {.interlacing = {.on = true}});
    m.machine.video(true);

    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    m.mock->finishFrame(fieldOf(kOdd), kWidth, kHeight, FrameField::Odd);
    m.machine.advanceTick();

    const RasterContent picture = m.machine.video();
    ASSERT_EQ(picture.height, 2 * kHeight);              // still interlacing
    EXPECT_EQ(lineByte(picture, 0), (kEven + kOdd) / 2);  // still Blend
}

// A setting given with the switch off is held for when the machine next draws.
TEST(VmVideoWeave, OptionsGivenWithVideoOffAreHeldForWhenItIsOn) {
    MockedVm m;
    m.machine.video(false, {.interlacing = {.on = true}});
    EXPECT_FALSE(m.mock->videoEnabled());
    EXPECT_THROW((void)m.machine.video(), std::logic_error);

    m.machine.video(true);
    m.mock->finishFrame(fieldOf(kEven), kWidth, kHeight, FrameField::Even);
    m.machine.advanceTick();
    EXPECT_EQ(m.machine.video().height, 2 * kHeight);
}

}  // namespace
