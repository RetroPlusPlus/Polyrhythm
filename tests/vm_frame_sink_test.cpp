// The picture seam, on a machine with no PPU.
//
// Every case here runs against MockVmBackend, whose finishFrame stands where a real core's vblank is.
// What is being pinned is that turning drawing on, holding a completed frame to the tick boundary,
// answering with the last complete one, and refusing the clock that cannot carry a picture are all the
// HOST layer's behaviour: they work with no console core underneath, so no part of them can have been
// designed around one core's picture.

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/vm.h"
#include "src/vm/vm_testing.h"
#include "tests/mock_vm_backend.h"

namespace {

using retropp::GuestFrameContent;
using retropp::GuestPixelFormat;
using retropp::Vm;
using retropp::VMPlatform;
using retropp::testing::MockVmBackend;
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
constexpr int kHeight = 2;

// A picture whose every pixel is a distinct byte pattern derived from `mark`, so one frame is never
// mistaken for another and a row read backwards is not the row read forwards.
std::vector<std::uint8_t> pictureOf(std::uint8_t mark) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint8_t>(mark + i);
    }
    return pixels;
}

// ── Drawing is opt-in ───────────────────────────────────────────────────────────────────────────

TEST(VmFrameSink, AMachineWithOutputDisabledNeverFiresTheSink) {
    MockedVm m;
    const std::vector<std::uint8_t> frame = pictureOf(1);

    // Nothing asked this machine to draw, so the core never turned its picture on and the frame the
    // test finishes goes nowhere.
    m.mock->finishFrame(frame, kWidth, kHeight);
    m.machine.advanceTick();

    EXPECT_FALSE(m.mock->pictureEnabled());
    EXPECT_THROW((void)m.machine.picture(), std::logic_error);
}

TEST(VmFrameSink, AskingAMachineThatDrawsNothingToDrawThrows) {
    MockedVm m;
    m.mock->refusePicture(true);

    EXPECT_THROW(m.machine.picture(true), std::logic_error);
}

TEST(VmFrameSink, AskingForThePictureTwiceIsIdempotent) {
    MockedVm m;
    m.machine.picture(true);
    EXPECT_NO_THROW(m.machine.picture(true));
    EXPECT_TRUE(m.mock->pictureEnabled());
}

// Drawing goes off the way it came on, and a machine that draws nothing has no picture to answer with.
TEST(VmFrameSink, TurningThePictureOffStopsTheMachineDrawing) {
    MockedVm m;
    m.machine.picture(true);
    m.mock->finishFrame(pictureOf(1), kWidth, kHeight);
    m.machine.advanceTick();
    EXPECT_NO_THROW((void)m.machine.picture());

    m.machine.picture(false);
    EXPECT_FALSE(m.mock->pictureEnabled());
    EXPECT_THROW((void)m.machine.picture(), std::logic_error);
}

// ── The tick boundary ───────────────────────────────────────────────────────────────────────────

// The user's ruling, pinned: a frame that completes part-way through a tick is not on screen until the
// boundary. Every other verb on a hosted machine lands there, and a picture does not get a second
// timing rule of its own.
TEST(VmFrameSink, AFrameCompletingBetweenTicksIsNotSeenUntilTheBoundary) {
    MockedVm m;
    m.machine.picture(true);
    const std::vector<std::uint8_t> frame = pictureOf(1);

    m.mock->finishFrame(frame, kWidth, kHeight);

    // Mid-tick: the machine has finished a frame, and the game cannot see it yet.
    const GuestFrameContent before = m.machine.picture();
    EXPECT_EQ(before.generation, 0u);
    EXPECT_TRUE(before.pixels.empty());
    EXPECT_EQ(before.width, 0);

    m.machine.advanceTick();

    const GuestFrameContent after = m.machine.picture();
    EXPECT_EQ(after.generation, 1u);
    EXPECT_EQ(after.width, kWidth);
    EXPECT_EQ(after.height, kHeight);
    ASSERT_EQ(after.pixels.size(), frame.size());
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), after.pixels.begin()));
}

// The generation counts FRAMES, not steps: three ticks with one frame finished among them leave it at
// one. A generation that tracked stepping would climb on every tick and every layer would re-upload.
TEST(VmFrameSink, TheGenerationAdvancesOncePerCompletedFrameNotPerStep) {
    MockedVm m;
    m.machine.picture(true);

    m.mock->finishFrame(pictureOf(1), kWidth, kHeight);
    m.machine.advanceTick();
    EXPECT_EQ(m.machine.picture().generation, 1u);

    m.machine.advanceTick();
    EXPECT_EQ(m.machine.picture().generation, 1u);

    m.machine.advanceTick();
    EXPECT_EQ(m.machine.picture().generation, 1u);

    m.mock->finishFrame(pictureOf(2), kWidth, kHeight);
    m.machine.advanceTick();
    EXPECT_EQ(m.machine.picture().generation, 2u);
}

// A machine that has finished nothing reports zero — the sentinel the renderer's per-layer slot starts
// at, so a first submission is never mistaken for one already resident.
TEST(VmFrameSink, AMachineThatFinishedNothingReportsGenerationZero) {
    MockedVm m;
    m.machine.picture(true);
    m.machine.advanceTick();

    EXPECT_EQ(m.machine.picture().generation, 0u);
}

TEST(VmFrameSink, TheLastCompleteFrameIsHeldWhenNoNewOneArrives) {
    MockedVm m;
    m.machine.picture(true);
    const std::vector<std::uint8_t> frame = pictureOf(7);

    m.mock->finishFrame(frame, kWidth, kHeight);
    m.machine.advanceTick();
    m.machine.advanceTick();  // a tick the machine finished nothing in

    // The picture is still on screen — a display that refreshes faster than the machine draws shows
    // the frame again rather than going blank.
    const GuestFrameContent held = m.machine.picture();
    EXPECT_EQ(held.generation, 1u);
    ASSERT_EQ(held.pixels.size(), frame.size());
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), held.pixels.begin()));
}

// Several frames finishing within one tick leave the last one, which is the only one that would still
// be on the machine's screen when the tick ends — and the generation jumps by as many as landed, so
// nothing downstream has to detect the case.
TEST(VmFrameSink, SeveralFramesInOneTickLeaveTheLastOne) {
    MockedVm m;
    m.machine.picture(true);
    const std::vector<std::uint8_t> first  = pictureOf(1);
    const std::vector<std::uint8_t> second = pictureOf(100);

    m.mock->finishFrame(first, kWidth, kHeight);
    m.mock->finishFrame(second, kWidth, kHeight);
    m.machine.advanceTick();

    const GuestFrameContent shown = m.machine.picture();
    EXPECT_EQ(shown.generation, 2u);
    ASSERT_EQ(shown.pixels.size(), second.size());
    EXPECT_TRUE(std::equal(second.begin(), second.end(), shown.pixels.begin()));
}

// The answer is the same however many times it is asked within one frame: a game reading picture()
// once to size a layer and again to submit it gets one picture, because reading a generation leaves
// it where it is.
TEST(VmFrameSink, ReadingThePictureTwiceInOneFrameAnswersTheSame) {
    MockedVm m;
    m.machine.picture(true);
    m.mock->finishFrame(pictureOf(1), kWidth, kHeight);
    m.machine.advanceTick();

    EXPECT_EQ(m.machine.picture().generation, 1u);
    EXPECT_EQ(m.machine.picture().generation, 1u);
}

// ── What a core owes, and what it is never asked ────────────────────────────────────────────────

TEST(VmFrameSink, ThePictureCarriesTheMachinesOwnDimensionsAndLayout) {
    MockedVm m;
    m.machine.picture(true);
    m.mock->finishFrame(pictureOf(3), kWidth, kHeight);
    m.machine.advanceTick();

    const GuestFrameContent shown = m.machine.picture();
    EXPECT_EQ(shown.width, kWidth);
    EXPECT_EQ(shown.height, kHeight);
    EXPECT_EQ(shown.format, GuestPixelFormat::Rgba8888);
    EXPECT_EQ(shown.pixels.size(),
              static_cast<std::size_t>(shown.width) * static_cast<std::size_t>(shown.height) *
                  retropp::bytesPerPixel(shown.format));
}

// ── The clock a picture needs ───────────────────────────────────────────────────────────────────

// A machine on a clock of its own draws like any other. It steps on its own thread, so its finished
// frames are handed to the game thread rather than written where that thread reads — which is a
// mechanism, not a restriction, and asking it to draw is accepted in either order.
//
// Asked while it runs, the change lands at the machine's next step boundary, where every change issued
// to a running machine lands. Reading the picture in the meantime answers the empty one rather than
// throwing: the game asked, so it is early rather than mistaken.
TEST(VmFrameSink, AContinuouslyAdvancingMachineDraws) {
    Vm machine{VMPlatform::GameBoy};
    machine.hostRom(std::vector<std::uint8_t>(0x8000, 0x00));
    machine.run(Vm::Advance::Continuously);

    EXPECT_NO_THROW(machine.picture(true));
    EXPECT_NO_THROW((void)machine.picture());
    EXPECT_EQ(machine.picture().generation, 0u);  // asked, not yet drawn

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (machine.picture().generation == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_GT(machine.picture().generation, 0u) << "the machine never drew on its own clock";
    machine.stop();
}

// The hand-off itself, under a real thread. The machine draws on its own thread for a stretch while
// this one reads its picture continuously — the exact overlap the publish exists for. Every frame read
// is whole: a picture is either the dimensions the machine drew at with the bytes to match, or the
// empty one from before it had finished any. A torn read shows up here as a size that fits neither.
//
// Under ThreadSanitizer this case is the detector; in the ordinary suite it is a liveness check that
// the picture advances at all when nothing calls advanceTick.
TEST(VmFrameSink, AMachineDrawingOnItsOwnThreadHandsWholeFramesToTheGameThread) {
    Vm machine{VMPlatform::GameBoy};
    machine.hostRom(std::vector<std::uint8_t>(0x8000, 0x00));
    machine.picture(true);
    machine.run(Vm::Advance::Continuously);

    // Bounded by WALL TIME, not iterations: the machine draws at its own hardware cadence, so a fixed
    // count of reads finishes long before it has completed a frame and would prove nothing.
    const auto     deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    std::uint64_t  highest  = 0;
    std::uint64_t  reads    = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const GuestFrameContent shown = machine.picture();
        ASSERT_EQ(shown.pixels.size(), static_cast<std::size_t>(shown.width) *
                                           static_cast<std::size_t>(shown.height) *
                                           retropp::bytesPerPixel(shown.format))
            << "a picture whose bytes do not match its own dimensions is a torn read";
        ASSERT_GE(shown.generation, highest) << "the generation went backwards";
        highest = shown.generation;
        ++reads;
    }
    machine.stop();
    EXPECT_GT(reads, 0u);

    // The machine ran on its own clock throughout, so it drew — nothing here ever called advanceTick.
    EXPECT_GT(highest, 0u);
}

TEST(VmFrameSink, ADrawingMachineTakesEitherClock) {
    Vm machine{VMPlatform::GameBoy};
    machine.picture(true);
    machine.hostRom(std::vector<std::uint8_t>(0x8000, 0x00));

    EXPECT_NO_THROW(machine.run(Vm::Advance::Continuously));
    machine.stop();
    EXPECT_NO_THROW(machine.run(Vm::Advance::OnTick));
    machine.stop();
}

}  // namespace
