// Reaching the SNES pad: the two-port word a game hands over, the twelve buttons in the pad's shift
// order, an empty socket told from an idle pad, and a press the guest's own code reads — the core
// answering `takesButtons()`/`setButtons()` and the machine sampling what it was given (snes.h:172-176,
// :381-399).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "retropp/guest_buttons.h"
#include "retropp/raster_content.h"
#include "retropp/input.h"
#include "retropp/snes.h"
#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"

namespace retropp {
namespace {

using vm::SnesBackend;
using vm::SnesBackendTestAccess;

constexpr std::uint64_t kOneFrame = 357'366u;

// ── The packing: what a pad state IS (snes.h's conversions, static_assert-testable) ────────────────

constexpr std::uint64_t bits(snes::Buttons b) noexcept { return GuestButtons(b).held; }
constexpr std::uint64_t bits(snes::Ports p) noexcept { return GuestButtons(p).held; }

// The pad's own shift order in the low twelve bits, with bit 15 marking port one occupied; ports pack
// port one in the low half and port two shifted up by sixteen.
TEST(SnesButtons, EachButtonIsItsOwnBit) {
    static_assert(bits(snes::Buttons{}) == (1ull << 15), "an idle pad is an occupied port, no buttons");
    static_assert(bits(snes::Buttons{.b = true}) == ((1ull << 0) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.y = true}) == ((1ull << 1) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.select = true}) == ((1ull << 2) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.start = true}) == ((1ull << 3) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.up = true}) == ((1ull << 4) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.down = true}) == ((1ull << 5) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.left = true}) == ((1ull << 6) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.right = true}) == ((1ull << 7) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.a = true}) == ((1ull << 8) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.x = true}) == ((1ull << 9) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.l = true}) == ((1ull << 10) | (1ull << 15)));
    static_assert(bits(snes::Buttons{.r = true}) == ((1ull << 11) | (1ull << 15)));

    static_assert(bits(snes::Buttons{.b = true, .a = true}) ==
                  ((1ull << 8) | (1ull << 0) | (1ull << 15)));

    static_assert(bits(snes::Ports{}) == 0, "nothing plugged in");
    static_assert(bits(snes::Ports{.one = snes::Buttons{}}) == (1ull << 15), "port one only");
    static_assert(bits(snes::Ports{.one = snes::Buttons{.a = true},
                                   .two = snes::Buttons{.b = true}}) ==
                  (((1ull << 8) | (1ull << 15)) | (((1ull << 0) | (1ull << 15)) << 16)));

    // One run-time mirror so a mutation of the layout reddens a RUN, not only the build.
    EXPECT_EQ(bits(snes::Buttons{.a = true}), (1ull << 8) | (1ull << 15));
}

// ── Reading the vocabulary back through snes::held ─────────────────────────────────────────────────

// Hold these Button actions this tick, the way the run loop hands a sampled level to the game.
void hold(InputState& in, std::initializer_list<snes::Button> down) {
    ActionSet level;
    for (const snes::Button b : down) {
        level.set(actionId(b), true);
    }
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[0] = level;
    InputSample sample;
    sample.players[0].held = level;
    in.sampleTick(sample, pressed);
}

TEST(SnesButtons, HeldReadsTheButtonActionsHeldThisTick) {
    InputState in;
    hold(in, {snes::Button::A, snes::Button::Left});

    const snes::Buttons pad = snes::held(in);
    EXPECT_TRUE(pad.a);
    EXPECT_TRUE(pad.left);
    EXPECT_FALSE(pad.b);
    EXPECT_FALSE(pad.right);
    EXPECT_EQ(GuestButtons(pad).held, (1ull << 8) | (1ull << 6) | (1ull << 15));

    hold(in, {});
    EXPECT_EQ(GuestButtons(snes::held(in)).held, (1ull << 15)) << "released is an idle occupied pad";
}

TEST(SnesButtons, ATapShorterThanATickStillReaches) {
    InputState in;

    // The level at tick time is nothing; the union since the last tick carries the press.
    std::array<ActionSet, kMaxPlayers> pressed{};
    pressed[0].set(actionId(snes::Button::A), true);
    InputSample sample;  // held is empty
    in.sampleTick(sample, pressed);

    EXPECT_TRUE(snes::held(in).a) << "a tap shorter than a tick still reaches the guest";
}

// ── What the machine sampled, through state().pads and state().joy ──────────────────────────────────

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

constexpr std::string_view kIdle =
    "        ORG $00:8000\n"
    "here:   BRA here\n";

TEST(SnesGuestInput, AHeldWordReaches4218) {
    // A fixture that turns the auto-joypad read on, so the machine latches the pad into $4218-$421F.
    constexpr std::string_view kAutoRead =
        "        ORG $00:8000\n"
        "        A8\n"
        "        X8\n"
        "        LDA #$01\n"
        "        STA !$4200\n"  // NMITIMEN: auto-joypad read on (bit 0)
        "here:   BRA here\n";

    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kAutoRead));
    backend.setButtons(GuestButtons(snes::Buttons{.a = true}).held);
    backend.runForCycles(2u * kOneFrame);  // past a vblank, so the auto-read has latched

    // A sits at bit 7 of the 16-bit pad word (snes.h Joypad::bits), so it lands in the low byte $4218.
    EXPECT_EQ(SnesBackendTestAccess::state(backend).joy[0], 0x80u);  // $4218: A
    EXPECT_EQ(SnesBackendTestAccess::state(backend).joy[1], 0x00u);  // $4219: the high byte is zero
}

TEST(SnesGuestInput, AnEmptySocketReadsDifferentlyFromAnIdlePad) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdle));

    // Port two an empty socket: no controller plugged in.
    backend.setButtons(GuestButtons(snes::Ports{.one = snes::Buttons{}}).held);
    EXPECT_TRUE(SnesBackendTestAccess::state(backend).pads[0].has_value());
    EXPECT_FALSE(SnesBackendTestAccess::state(backend).pads[1].has_value());

    // Port two an idle pad: a controller is in the socket, holding nothing. The auto-read word is zero
    // for BOTH, so this is asserted only at state().pads (snes.h:174-176).
    backend.setButtons(GuestButtons(snes::Ports{.one = snes::Buttons{}, .two = snes::Buttons{}}).held);
    ASSERT_TRUE(SnesBackendTestAccess::state(backend).pads[1].has_value());
    EXPECT_EQ(*SnesBackendTestAccess::state(backend).pads[1], snaggletooth::Joypad{});
}

TEST(SnesGuestInput, PortTwoIsIndependentOfPortOne) {
    SnesBackend backend;
    backend.loadRom(cartridgeFrom(kIdle));

    backend.setButtons(GuestButtons(snes::Ports{.one = snes::Buttons{.a = true}, .two = snes::Buttons{}}).held);
    ASSERT_TRUE(SnesBackendTestAccess::state(backend).pads[0].has_value());
    ASSERT_TRUE(SnesBackendTestAccess::state(backend).pads[1].has_value());
    EXPECT_TRUE(SnesBackendTestAccess::state(backend).pads[0]->a);
    EXPECT_EQ(*SnesBackendTestAccess::state(backend).pads[1], snaggletooth::Joypad{});

    // Swapping the halves swaps the reads.
    backend.setButtons(GuestButtons(snes::Ports{.one = snes::Buttons{}, .two = snes::Buttons{.a = true}}).held);
    EXPECT_EQ(*SnesBackendTestAccess::state(backend).pads[0], snaggletooth::Joypad{});
    ASSERT_TRUE(SnesBackendTestAccess::state(backend).pads[1].has_value());
    EXPECT_TRUE(SnesBackendTestAccess::state(backend).pads[1]->a);
}

// A cartridge that draws the JOY1 low byte as the backdrop colour: reset goes native, turns the screen
// on and enables the vblank NMI + the auto-joypad read; the native NMI acknowledges the NMI, waits for
// the auto-read, reads $4218 and writes it to CGRAM entry 0. With no layers enabled the whole screen is
// that backdrop, so the picture IS the pad word.
constexpr std::string_view kDrawsPad =
    "        ORG $00:8000\n"
    "        EMULATION\n"
    "        CLC\n"
    "        XCE\n"          // native mode
    "        SEP #$30\n"     // A8, X8
    "        LDA #$0F\n"
    "        STA !$2100\n"   // INIDISP: screen on, full brightness
    "        LDA #$81\n"
    "        STA !$4200\n"   // NMITIMEN: vblank NMI (bit7) + auto-joypad read (bit0)
    "idle:   BRA idle\n"
    "        ORG $00:8100\n"
    "        A8\n"
    "        X8\n"
    "        LDA !$4210\n"   // RDNMI: acknowledge the vblank NMI
    "wait:   LDA !$4212\n"   // HVBJOY
    "        AND #$01\n"     // auto-read busy
    "        BNE wait\n"     // spin until the auto-read has finished
    "        LDA #$00\n"
    "        STA !$2121\n"   // CGADD: palette word 0 (the backdrop)
    "        LDA !$4218\n"   // JOY1 low byte
    "        STA !$2122\n"   // CGDATA: the colour's low byte
    "        STA !$2122\n"   // CGDATA: its high byte
    "        RTI\n";

std::vector<std::uint8_t> drawsPadCartridge() {
    std::vector<std::uint8_t> rom = cartridgeFrom(kDrawsPad);
    rom[0x7FC0u + 0x2Au] = 0x00u;  // native NMI vector -> $8100
    rom[0x7FC0u + 0x2Bu] = 0x81u;
    return rom;
}

TEST(SnesGuestInput, APressReachesThePictureInAFixedNumberOfFrames) {
    SnesBackend backend;
    std::vector<bool> lit;  // whether each delivered frame's backdrop is anything but black
    backend.setVideoEnabled(true);
    backend.setFrameSink([&lit](std::span<const std::uint8_t> pixels, int, int, RasterPixelFormat, vm::FrameField) {
        lit.push_back(!pixels.empty() && (pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0));
    });
    backend.loadRom(drawsPadCartridge());

    // Advance the machine by exactly one delivered frame: a chunk shorter than a frame crosses at most
    // one frame boundary, so looping until the sink fires lands precisely one frame.
    const auto stepOneFrame = [&] {
        const std::size_t before = lit.size();
        while (lit.size() == before) {
            backend.runForCycles(60'000);
        }
    };

    for (int i = 0; i < 4; ++i) stepOneFrame();  // warm up with nothing pressed
    for (const bool frameLit : lit) {
        EXPECT_FALSE(frameLit) << "an empty port draws a black backdrop";
    }

    const std::size_t submit = lit.size();  // frames delivered before the press is set
    backend.setButtons(GuestButtons(snes::Buttons{.a = true}).held);

    bool found = false;
    std::size_t firstPressed = 0;
    for (int i = 0; i < 8 && !found; ++i) {
        stepOneFrame();
        if (lit.back()) {
            firstPressed = lit.size() - 1;  // 0-based index (== generation) of the first pressed frame
            found = true;
        }
    }
    ASSERT_TRUE(found) << "the press never reached the picture";
    // Derived, not measured (Item 10a): the auto-read the NMI reads is this frame's; the CGRAM write it
    // makes takes effect for the NEXT frame's raster; and a finished frame is delivered as the beam
    // reaches the following frame's first line. So the first delivered frame whose backdrop shows the
    // press is two generations after the submit boundary.
    EXPECT_EQ(firstPressed - submit, 2u);
}

}  // namespace
}  // namespace retropp
