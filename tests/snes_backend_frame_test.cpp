// A hosted machine's picture reaches the engine through the frame sink, at the machine's own
// dimensions, reported as a whole frame or as a field of an interlaced picture, and only when video is
// asked for — a raster costs cycles, so a machine nobody watches draws nothing and its state is the same
// either way (snes.h:543-544).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/raster_content.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

// A frame arrives as the beam wraps; running well past one NTSC frame guarantees at least one.
constexpr std::uint64_t kOneFrame = 357'366u;

std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
    const snaggletooth::assembler::Assembly program =
        snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
    EXPECT_TRUE(program.ok());
    std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
    for (const auto& range : program.ranges) {
        const std::size_t at = range.start - 0x8000u;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return rom;
}

// A machine that just rests: the PPU still advances the beam, so frames are drawn whether or not the
// program does anything.
constexpr std::string_view kIdle =
    "        ORG $00:8000\n"
    "here:   BRA here\n";

// A program that asks the chip to interlace and then rests.
constexpr std::string_view kInterlaces =
    "        ORG $00:8000\n"
    "        SEP #$20\n"
    "        LDA #$01\n"
    "        STA !$2133\n"   // SETINI: interlace
    "here:   BRA here\n";

// What a frame the sink received looked like.
struct FrameReport {
    int               width  = 0;
    int               height = 0;
    RasterPixelFormat format = RasterPixelFormat::Rgba8888;
    int               count  = 0;
    std::vector<vm::FrameField> fields;  // what each frame was reported as, in order
};

TEST(SnesBackendFrame, AFrameArrivesWithTheMachinesOwnDimensions) {
    SnesBackend backend;
    FrameReport report;
    backend.setVideoEnabled(true);
    backend.setFrameSink([&report](std::span<const std::uint8_t> pixels, int width, int height,
                                   RasterPixelFormat format, vm::FrameField field) {
        report.width  = width;
        report.height = height;
        report.format = format;
        ++report.count;
        report.fields.push_back(field);
        EXPECT_EQ(pixels.size(),
                  static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                      bytesPerPixel(format));
    });
    backend.loadRom(cartridgeFrom(kIdle));
    backend.runForCycles(kOneFrame + 20'000);

    ASSERT_GT(report.count, 0);
    EXPECT_EQ(report.format, RasterPixelFormat::Rgba8888);
    EXPECT_EQ(report.width, 256);   // a fixture that never widens draws 256 across
    EXPECT_GT(report.height, 0);
    for (const vm::FrameField field : report.fields) {
        EXPECT_EQ(field, vm::FrameField::Whole);   // a program that never interlaces draws whole frames
    }

    // The height is the machine's own, not a number this test picked: a second run reports the same.
    SnesBackend second;
    FrameReport again;
    second.setVideoEnabled(true);
    second.setFrameSink([&again](std::span<const std::uint8_t>, int width, int height,
                                 RasterPixelFormat, vm::FrameField) {
        again.width  = width;
        again.height = height;
        ++again.count;
    });
    second.loadRom(cartridgeFrom(kIdle));
    second.runForCycles(kOneFrame + 20'000);
    ASSERT_GT(again.count, 0);
    EXPECT_EQ(again.height, report.height);
    EXPECT_EQ(again.width, report.width);
}

TEST(SnesBackendFrame, VideoOffDeliversNoFrameAndDoesNotChangeState) {
    SnesBackend off;
    int frames = 0;
    off.setFrameSink([&frames](std::span<const std::uint8_t>, int, int, RasterPixelFormat, vm::FrameField) { ++frames; });
    off.loadRom(cartridgeFrom(kIdle));  // video off is the default
    off.runForCycles(kOneFrame + 20'000);
    EXPECT_EQ(frames, 0);

    // A machine drawing and a machine not drawing compute the same thing: video changes what is DRAWN,
    // not the program's state.
    SnesBackend on;
    on.setVideoEnabled(true);
    on.setFrameSink([](std::span<const std::uint8_t>, int, int, RasterPixelFormat, vm::FrameField) {});
    on.loadRom(cartridgeFrom(kIdle));
    on.runForCycles(kOneFrame + 20'000);

    const snaggletooth::SnesState& a = SnesBackendTestAccess::state(off);
    const snaggletooth::SnesState& b = SnesBackendTestAccess::state(on);
    EXPECT_EQ(a.master, b.master);
    EXPECT_EQ(a.wram, b.wram);
    EXPECT_EQ(a.ppu.vram, b.ppu.vram);
}

TEST(SnesBackendFrame, VideoCanBeEnabledAfterHosting) {
    SnesBackend backend;
    int frames = 0;
    backend.setFrameSink([&frames](std::span<const std::uint8_t>, int, int, RasterPixelFormat, vm::FrameField) { ++frames; });
    backend.loadRom(cartridgeFrom(kIdle));

    backend.runForCycles(kOneFrame + 20'000);
    EXPECT_EQ(frames, 0);  // video still off

    backend.setVideoEnabled(true);
    backend.runForCycles(kOneFrame + 20'000);
    EXPECT_GT(frames, 0);  // enabling it on the live machine re-attaches the observer
}

// With the interlace bit set, every frame is one field, at alternating parity.
TEST(SnesBackendFrame, AnInterlacedRunHandsOverFieldsOfAlternatingParity) {
    SnesBackend backend;
    std::vector<vm::FrameField> fields;
    backend.setVideoEnabled(true);
    backend.setFrameSink([&fields](std::span<const std::uint8_t>, int, int, RasterPixelFormat,
                                   vm::FrameField field) { fields.push_back(field); });
    backend.loadRom(cartridgeFrom(kInterlaces));
    backend.runForCycles(4 * kOneFrame + 20'000);

    ASSERT_GE(fields.size(), 3u);
    // The first frame may complete under either value of the register; every one after it is a field.
    for (std::size_t i = 1; i < fields.size(); ++i) {
        EXPECT_NE(fields[i], vm::FrameField::Whole) << "frame " << i;
    }
    for (std::size_t i = 2; i < fields.size(); ++i) {
        EXPECT_NE(fields[i], fields[i - 1]) << "frames " << i - 1 << " and " << i;
    }
}

}  // namespace
}  // namespace retropp
