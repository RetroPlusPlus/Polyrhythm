// A hosted SNES cartridge's sound: the chip's 32'000 Hz frames converted to the sink's rate by the exact
// ratio of the two, the count exact to the frame, a constant and an equal rate passing through untouched,
// images rejected below the floor, and the machine draining its DSP whether or not anyone listens — plus
// the frames one step of an SNES voice machine is worth to the audio system.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "spc700/spc700_asm.h"
#include "examples/common.h"
#include "retropp/audio.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "snaggletooth/snes/snes.h"
#include "src/audio/audio_system_testing.h"
#include "src/vm/snes/resampler.h"
#include "src/vm/snes/snes_backend.h"
#include "src/vm/snes/snes_backend_testing.h"
#include "mock_platform.h"

namespace retropp {
namespace {

using vm::RationalResampler;
using vm::SnesBackend;
using vm::SnesBackendTestAccess;

constexpr std::uint64_t kNtscFrame = 357'366u;  // master cycles in one 60 Hz frame
constexpr std::uint64_t kPalFrame  = 425'568u;  // master cycles in one 50 Hz frame
constexpr int           kFrames    = 600;       // every machine case runs this many frames

// A one-bank LoROM cartridge whose reset code is `source`, laid over the canonical header.
// `source` places its first byte at $00:8000, where reset points.
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

// The same, with an audio-unit program beside it: each SPC700 range lands at ROM offset
// $1E00 + its address, so audio-unit $0200 is read from $00:A000 and $0300 from $00:A100.
std::vector<std::uint8_t> soundCartridgeFrom(std::string_view program65816, std::string_view sound) {
    std::vector<std::uint8_t>                  rom = cartridgeFrom(program65816);
    const snaggletooth::assembler::Assembly audio =
        snaggletooth::assembler::assembleSpc700(sound, "fixture_sound.asm");
    EXPECT_TRUE(audio.ok());
    for (const auto& range : audio.ranges) {
        const std::size_t at = 0x1E00u + range.start;
        std::copy(range.bytes.begin(), range.bytes.end(),
                  rom.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return rom;
}

// A PAL cartridge: the same image with the header's country byte naming a 50 Hz region.
std::vector<std::uint8_t> palCartridge(std::vector<std::uint8_t> rom) {
    rom[0x7FC0u + 0x19u] = 0x02u;
    return rom;
}

// Power at frequency `f` over `n` samples of `x` from `from`, Hann-windowed — the same reading the
// planner's probe took, so the floors below are its numbers.
double power(const std::vector<std::int16_t>& x, std::size_t from, std::size_t n, double f, double rate) {
    constexpr double kPi = 3.14159265358979323846;
    const double     w   = 2.0 * kPi * f / rate;
    double           re  = 0.0;
    double           im  = 0.0;
    for (std::size_t i = 0; i < n && from + i < x.size(); ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double v    = hann * x[from + i];
        re += v * std::cos(w * static_cast<double>(i));
        im -= v * std::sin(w * static_cast<double>(i));
    }
    return (re * re + im * im) / (static_cast<double>(n) * static_cast<double>(n));
}

// Push every frame of `in` (the same value on both channels) and return the left outputs.
std::vector<std::int16_t> collect(RationalResampler& rs, const std::vector<std::int16_t>& in) {
    std::vector<std::int16_t> out;
    for (const std::int16_t sample : in) {
        rs.push(sample, sample, [&out](std::int16_t left, std::int16_t) { out.push_back(left); });
    }
    return out;
}

// 256 frames of silence with a full-scale impulse at the first.
std::vector<std::int16_t> impulse() {
    std::vector<std::int16_t> in(256, 0);
    in[0] = 32'767;
    return in;
}

// What a sink collects: the left and right channels in order.
struct Captured {
    std::vector<std::int16_t> left;
    std::vector<std::int16_t> right;
};

vm::VmBackend::AudioSampleSink captureInto(Captured& captured) {
    return [&captured](std::int16_t left, std::int16_t right) {
        captured.left.push_back(left);
        captured.right.push_back(right);
    };
}

vm::VmBackend::AudioSampleSink countInto(std::uint64_t& count) {
    return [&count](std::int16_t, std::int16_t) { ++count; };
}

// Run `frames` frames of `frameCycles` master cycles through the backend, one step a frame.
void runFrames(SnesBackend& backend, int frames, std::uint64_t frameCycles) {
    for (int i = 0; i < frames; ++i) {
        backend.runForCycles(frameCycles);
    }
}

// Run `frames` frames of `frameCycles` on a bare machine, taking its DSP frames after each.
std::vector<snaggletooth::StereoFrame> bareFrames(snaggletooth::Snes& snes, int frames,
                                                  std::uint64_t frameCycles) {
    std::vector<snaggletooth::StereoFrame> taken;
    for (int i = 0; i < frames; ++i) {
        snes.run(frameCycles);
        const std::vector<snaggletooth::StereoFrame> step = snes.takeFrames();
        taken.insert(taken.end(), step.begin(), step.end());
    }
    return taken;
}

// The idle fixture: the program rests from its first instruction.
constexpr std::string_view kIdle = "        ORG $00:8000\nhere:   BRA here\n";

// The tone fixture: the main program speaks the audio unit's upload protocol — waits for the stub's ready
// bytes, sends the driver to $0200 and its directory and sample to $0300 one acknowledged byte at a time,
// starts it — and rests; the driver keys one voice on a looping square wave at 1'000 Hz and rests.
// Probed: the first non-zero frame lands 12 ms after power-on and the wave swings −10'674..10'672.
constexpr std::string_view kToneCartridge = R"asm(
        ORG $00:8000
        EMULATION
        CLC
        XCE
        SEP #$30
ready:  LDA !$2140
        CMP #$AA
        BNE ready
        LDA !$2141
        CMP #$BB
        BNE ready
        LDA #$00
        STA !$2142
        LDA #$02
        STA !$2143
        LDA #$01
        STA !$2141
        LDA #$CC
        STA !$2140
kick:   CMP !$2140
        BNE kick
        LDX #$00
prog:   LDA !$A000,X
        STA !$2141
        TXA
        STA !$2140
ackp:   CMP !$2140
        BNE ackp
        INX
        CPX #$80
        BNE prog
        LDA #$00
        STA !$2142
        LDA #$03
        STA !$2143
        LDA #$01
        STA !$2141
        LDA #$81
        STA !$2140
ackd:   CMP !$2140
        BNE ackd
        LDX #$00
data:   LDA !$A100,X
        STA !$2141
        TXA
        STA !$2140
ackq:   CMP !$2140
        BNE ackq
        INX
        CPX #$10
        BNE data
        LDA #$00
        STA !$2142
        LDA #$02
        STA !$2143
        LDA #$00
        STA !$2141
        LDA #$11
        STA !$2140
acks:   CMP !$2140
        BNE acks
idle:   BRA idle
)asm";

constexpr std::string_view kToneSound = R"asm(
        ORG $0200
        MOV $F2,#$5D
        MOV $F3,#$03            ; DIR: the sample directory is page $03
        MOV $F2,#$04
        MOV $F3,#$00            ; V0SRCN: voice 0 plays source 0
        MOV $F2,#$00
        MOV $F3,#$40            ; V0VOLL
        MOV $F2,#$01
        MOV $F3,#$40            ; V0VOLR
        MOV $F2,#$02
        MOV $F3,#$00            ; V0PITCHL
        MOV $F2,#$03
        MOV $F3,#$08            ; V0PITCHH: pitch $0800, half the chip's rate, so the sixteen-sample cycle is 1'000 Hz
        MOV $F2,#$07
        MOV $F3,#$7F            ; V0GAIN: direct, full
        MOV $F2,#$0C
        MOV $F3,#$60            ; MVOLL
        MOV $F2,#$1C
        MOV $F3,#$60            ; MVOLR
        MOV $F2,#$6C
        MOV $F3,#$20            ; FLG: unmuted, echo writes off
        MOV $F2,#$4C
        MOV $F3,#$01            ; KON: voice 0 starts
here:   BRA here
        ORG $0300
        DW $0304,$0304          ; directory entry 0: the block right after it, looping on itself
        DB $C3                  ; shift 12, filter 0, loop and end: one block is the whole wave
        DB $77,$77,$77,$77,$99,$99,$99,$99   ; eight samples at +7, eight at -7: a square, sixteen samples a cycle
)asm";

// ── RationalResampler ───────────────────────────────────────────────────────────────────────────────

TEST(RationalResampler, TheRatioIsTheExactReducedFraction) {
    const RationalResampler to48{32'000, 48'000};
    EXPECT_EQ(to48.upFactor(), 3u);
    EXPECT_EQ(to48.downFactor(), 2u);
    const RationalResampler to441{32'000, 44'100};
    EXPECT_EQ(to441.upFactor(), 441u);
    EXPECT_EQ(to441.downFactor(), 320u);
    const RationalResampler same{32'000, 32'000};
    EXPECT_EQ(same.upFactor(), 1u);
    EXPECT_EQ(same.downFactor(), 1u);
    const RationalResampler to2205{32'000, 22'050};
    EXPECT_EQ(to2205.upFactor(), 441u);
    EXPECT_EQ(to2205.downFactor(), 640u);
}

TEST(RationalResampler, TheOutputCountIsCeilOfInputTimesTheRatio) {
    struct Row {
        unsigned      rate;
        std::uint64_t in;
        std::uint64_t out;
    };
    // Every row is ceil(in x up / down).
    constexpr Row kRows[] = {
        {.rate = 48'000, .in = 1, .out = 2},
        {.rate = 48'000, .in = 2, .out = 3},
        {.rate = 48'000, .in = 320, .out = 480},
        {.rate = 48'000, .in = 32'000, .out = 48'000},
        {.rate = 48'000, .in = 319'473, .out = 479'210},
        {.rate = 48'000, .in = 319'474, .out = 479'211},
        {.rate = 48'000, .in = 1'920'000, .out = 2'880'000},
        {.rate = 44'100, .in = 1, .out = 2},
        {.rate = 44'100, .in = 2, .out = 3},
        {.rate = 44'100, .in = 320, .out = 441},
        {.rate = 44'100, .in = 32'000, .out = 44'100},
        {.rate = 44'100, .in = 319'473, .out = 440'274},
        {.rate = 44'100, .in = 319'474, .out = 440'276},
        {.rate = 44'100, .in = 1'920'000, .out = 2'646'000},
    };
    for (const Row& row : kRows) {
        RationalResampler rs{32'000, row.rate};
        std::uint64_t     count = 0;
        for (std::uint64_t i = 0; i < row.in; ++i) {
            rs.push(0, 0, [&count](std::int16_t, std::int16_t) { ++count; });
        }
        EXPECT_EQ(count, row.out) << row.in << " frames to " << row.rate << " Hz";
    }
}

TEST(RationalResampler, EqualRatesPassEveryFrameThrough) {
    RationalResampler rs{32'000, 32'000};
    std::uint32_t     seed = 0x1234'5678u;
    const auto        next = [&seed] {
        seed = seed * 1'664'525u + 1'013'904'223u;
        return static_cast<std::int16_t>(seed >> 16);
    };
    for (int i = 0; i < 4'096; ++i) {
        const std::int16_t left  = next();
        const std::int16_t right = next();
        int                calls = 0;
        rs.push(left, right, [&](std::int16_t outLeft, std::int16_t outRight) {
            ++calls;
            EXPECT_EQ(outLeft, left);
            EXPECT_EQ(outRight, right);
        });
        EXPECT_EQ(calls, 1);
    }
}

TEST(RationalResampler, AConstantPassesAtItsOwnValue) {
    for (const unsigned rate : {48'000u, 44'100u}) {
        for (const std::int16_t constant : {std::int16_t{1'000}, std::int16_t{-1'000}, std::int16_t{32'767},
                                            std::int16_t{-32'768}}) {
            RationalResampler               rs{32'000, rate};
            const std::vector<std::int16_t> out = collect(rs, std::vector<std::int16_t>(400, constant));
            ASSERT_GT(out.size(), 200u);
            for (std::size_t i = 200; i < out.size(); ++i) {
                ASSERT_EQ(out[i], constant) << "output " << i << " at " << rate << " Hz";
            }
        }
    }
}

TEST(RationalResampler, AnImpulsePeaksOneMillisecondLater) {
    // Output k reads prototype tap k × down; the peak is where that is nearest the prototype's center
    // (kTapsPerPhase × up − 1) / 2 — 96 of 191 at 3 : 2, 14'080 of 28'223 at 441 : 320 — one
    // millisecond at either rate.
    struct Row {
        unsigned    rate;
        std::size_t index;
        int         value;
    };
    constexpr Row kRows[] = {
        {.rate = 48'000, .index = 48, .value = 29'672},
        {.rate = 44'100, .index = 44, .value = 30'684},
    };
    for (const Row& row : kRows) {
        RationalResampler               rs{32'000, row.rate};
        const std::vector<std::int16_t> out     = collect(rs, impulse());
        const auto                      largest = std::max_element(
            out.begin(), out.end(), [](std::int16_t a, std::int16_t b) { return std::abs(a) < std::abs(b); });
        EXPECT_EQ(static_cast<std::size_t>(largest - out.begin()), row.index) << row.rate << " Hz";
        EXPECT_NEAR(*largest, row.value, 4) << row.rate << " Hz";
    }
}

TEST(RationalResampler, ATonePassesAndItsImagesAreRejected) {
    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kInput = 32'000.0;
    for (const unsigned rate : {48'000u, 44'100u}) {
        for (const double f : {1'000.0, 4'000.0, 8'000.0, 12'000.0}) {
            std::vector<std::int16_t> in(3 * 32'000);
            for (std::size_t i = 0; i < in.size(); ++i) {
                in[i] = static_cast<std::int16_t>(
                    std::lround(16'000.0 * std::sin(2.0 * kPi * f * static_cast<double>(i) / kInput)));
            }
            RationalResampler               rs{32'000, rate};
            const std::vector<std::int16_t> out = collect(rs, in);

            const double outRate  = static_cast<double>(rate);
            const double toneOut  = power(out, rate, rate, f, outRate);
            const double toneIn   = power(in, 32'000, 32'000, f, kInput);
            const double gainDb   = 10.0 * std::log10(toneOut / toneIn);
            EXPECT_NEAR(gainDb, 0.0, 0.05) << f << " Hz at " << rate << " Hz";

            double worstDb = -1000.0;
            for (int m = 1; m <= 4; ++m) {
                for (const double image : {m * kInput - f, m * kInput + f}) {
                    double folded = std::fmod(image, outRate);
                    if (folded > outRate / 2.0) {
                        folded = outRate - folded;
                    }
                    if (std::abs(folded - f) < 50.0) {
                        continue;
                    }
                    const double db = 10.0 * std::log10(power(out, rate, rate, folded, outRate) / toneOut);
                    worstDb = std::max(worstDb, db);
                }
            }
            EXPECT_LE(worstDb, -80.0) << f << " Hz at " << rate << " Hz";
        }
    }
}

TEST(RationalResampler, ResetForgetsTheHistoryAndThePhase) {
    RationalResampler rs{32'000, 48'000};
    std::vector<std::int16_t> a = collect(rs, impulse());
    a.resize(100);
    collect(rs, std::vector<std::int16_t>(37, 5'000));
    rs.reset();
    std::vector<std::int16_t> b = collect(rs, impulse());
    b.resize(100);
    EXPECT_EQ(a, b);

    RationalResampler counted{32'000, 48'000};
    EXPECT_EQ(collect(counted, std::vector<std::int16_t>(3, 0)).size(), 5u);  // ceil(4.5)
    collect(counted, std::vector<std::int16_t>(37, 5'000));
    counted.reset();
    EXPECT_EQ(collect(counted, std::vector<std::int16_t>(3, 0)).size(), 5u);
}

// ── SnesBackendAudio ────────────────────────────────────────────────────────────────────────────────

TEST(SnesBackendAudio, TheCartridgesSoundReachesTheSinkAtTheChipsOwnRate) {
    const std::vector<std::uint8_t> rom = soundCartridgeFrom(kToneCartridge, kToneSound);

    Captured    captured;
    SnesBackend backend;
    backend.enableAudio(32'000, captureInto(captured));
    backend.loadRom(rom);
    runFrames(backend, kFrames, kNtscFrame);

    snaggletooth::Snes bare{snaggletooth::SnesConfig{.rom = rom, .region = snaggletooth::Region::Ntsc}};
    const std::vector<snaggletooth::StereoFrame> expected = bareFrames(bare, kFrames, kNtscFrame);

    ASSERT_EQ(expected.size(), 319'473u);
    ASSERT_EQ(captured.left.size(), expected.size());
    ASSERT_EQ(captured.right.size(), expected.size());
    bool sounds = false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(captured.left[i], expected[i].left) << "frame " << i;
        ASSERT_EQ(captured.right[i], expected[i].right) << "frame " << i;
        sounds = sounds || expected[i].left != 0;
    }
    EXPECT_TRUE(sounds);  // the tone plays
}

TEST(SnesBackendAudio, TheFrameCountIsTheDspsOwn) {
    // The chip's clock is 5'632 / 118'125 of the master clock (snes.h), one frame every 32 of its
    // cycles: 532 or 533 frames a 60 Hz frame, 639 or 640 a 50 Hz one. The converted counts are
    // ceil(in x 3 / 2) and ceil(in x 441 / 320). Every figure is the probe's.
    struct Row {
        bool          pal;
        unsigned      rate;
        std::uint64_t frames;
    };
    constexpr Row kRows[] = {
        {.pal = false, .rate = 32'000, .frames = 319'473},
        {.pal = true, .rate = 32'000, .frames = 383'946},
        {.pal = false, .rate = 48'000, .frames = 479'210},
        {.pal = false, .rate = 44'100, .frames = 440'274},
        {.pal = true, .rate = 48'000, .frames = 575'919},
        {.pal = true, .rate = 44'100, .frames = 529'126},
    };
    for (const Row& row : kRows) {
        std::uint64_t count = 0;
        SnesBackend   backend;
        backend.enableAudio(row.rate, countInto(count));
        backend.loadRom(row.pal ? palCartridge(cartridgeFrom(kIdle)) : cartridgeFrom(kIdle));
        runFrames(backend, kFrames, row.pal ? kPalFrame : kNtscFrame);
        EXPECT_EQ(count, row.frames) << (row.pal ? "PAL" : "NTSC") << " at " << row.rate << " Hz";
    }
}

TEST(SnesBackendAudio, AMachineNobodyListensToKeepsNoFrames) {
    SnesBackend silent;
    silent.loadRom(cartridgeFrom(kIdle));
    runFrames(silent, 60, kNtscFrame);
    EXPECT_EQ(SnesBackendTestAccess::pendingAudioFrames(silent), 0u);

    std::uint64_t count = 0;
    SnesBackend   heard;
    heard.enableAudio(48'000, countInto(count));
    heard.loadRom(cartridgeFrom(kIdle));
    runFrames(heard, 60, kNtscFrame);
    EXPECT_EQ(SnesBackendTestAccess::pendingAudioFrames(heard), 0u);
}

TEST(SnesBackendAudio, TheSlicedRunIsTheMachinesOwnRun) {
    const std::vector<std::uint8_t> rom = cartridgeFrom(kIdle);

    SnesBackend backend;
    backend.loadRom(rom);
    runFrames(backend, kFrames, kNtscFrame);

    snaggletooth::Snes bare{snaggletooth::SnesConfig{.rom = rom, .region = snaggletooth::Region::Ntsc}};
    bare.run(kFrames * kNtscFrame);

    // The slicing is invisible to the program: run(a) then run(b) is run(a + b) (snes.h).
    const snaggletooth::SnesState& sliced = SnesBackendTestAccess::state(backend);
    const snaggletooth::SnesState& whole  = bare.state();
    EXPECT_EQ(sliced.master, whole.master);
    EXPECT_EQ(sliced.consumed, whole.consumed);
    EXPECT_EQ(sliced.wram, whole.wram);
    EXPECT_EQ(sliced.apu.divider, whole.apu.divider);
    EXPECT_EQ(sliced.apuPhase, whole.apuPhase);
}

TEST(SnesBackendAudio, ResetStartsTheSoundOver) {
    std::uint64_t count = 0;
    SnesBackend   backend;
    backend.enableAudio(48'000, countInto(count));
    backend.loadRom(soundCartridgeFrom(kToneCartridge, kToneSound));
    runFrames(backend, kFrames, kNtscFrame);
    const std::uint64_t c1 = count;
    EXPECT_EQ(c1, 479'210u);

    backend.reset();
    runFrames(backend, kFrames, kNtscFrame);
    const std::uint64_t c2 = count - c1;
    // A converter that kept its phase across the reset would hand back 479'209:
    // ceil(638'946 × 1.5) − 479'210.
    EXPECT_EQ(c2, c1);
}

TEST(SnesBackendAudio, TheSinkRateIsTheSinksAndThePitchIsTheCartridges) {
    const std::vector<std::uint8_t> rom = soundCartridgeFrom(kToneCartridge, kToneSound);
    for (const unsigned rate : {32'000u, 44'100u, 48'000u}) {
        Captured    captured;
        SnesBackend backend;
        backend.enableAudio(rate, captureInto(captured));
        backend.loadRom(rom);
        runFrames(backend, kFrames, kNtscFrame);

        const std::vector<std::int16_t>& out = captured.left;
        ASSERT_GE(out.size(), rate);
        const std::size_t from = out.size() - rate;  // the last full second
        const double      tone = power(out, from, rate, 1'000.0, rate);
        if (rate == 32'000u) {
            // The tone and the frequency an unconverted stream would show are one bin at the chip's rate.
            EXPECT_GT(tone, 1e6);
            continue;
        }
        // An unconverted stream would put the tone at 1'000 × rate / 32'000: 1'378.125 Hz at 44'100,
        // 1'500 Hz at 48'000.
        const double alias = power(out, from, rate, 1'000.0 * rate / 32'000.0, rate);
        EXPECT_GE(10.0 * std::log10(tone / alias), 60.0) << rate << " Hz";
    }
}

TEST(SnesBackendAudio, EnablingAudioAgainReplacesTheSinkAndTheRate) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    SnesBackend   backend;
    backend.enableAudio(48'000, countInto(a));
    backend.enableAudio(44'100, countInto(b));
    backend.loadRom(soundCartridgeFrom(kToneCartridge, kToneSound));
    runFrames(backend, kFrames, kNtscFrame);
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 440'274u);

    EXPECT_THROW(backend.enableAudio(0, countInto(a)), std::invalid_argument);
}

TEST(SnesBackendAudio, AudioCanBeEnabledAfterHosting) {
    const std::vector<std::uint8_t> rom = soundCartridgeFrom(kToneCartridge, kToneSound);

    std::uint64_t count = 0;
    SnesBackend   backend;
    backend.loadRom(rom);
    runFrames(backend, 60, kNtscFrame);
    backend.enableAudio(48'000, countInto(count));
    runFrames(backend, kFrames, kNtscFrame);

    snaggletooth::Snes bare{snaggletooth::SnesConfig{.rom = rom, .region = snaggletooth::Region::Ntsc}};
    static_cast<void>(bareFrames(bare, 60, kNtscFrame));
    const std::uint64_t in = bareFrames(bare, kFrames, kNtscFrame).size();
    EXPECT_EQ(count, (in * 3 + 1) / 2);  // ceil(in × 3 / 2)
}

// ── SnesAudioSystem ─────────────────────────────────────────────────────────────────────────────────

TEST(SnesAudioSystem, AVoiceStepIsOneFrameOfTheMachinesOwnSound) {
    // One step is one frame of the machine's own clock: 357'366 × rate × 11 / 236'250'000 frames for
    // the SNES, and the Game Boy's own frame for the Game Boy. Constructing hosts nothing, so the SNES
    // core's driver verbs are never reached.
    test::CaptureAudioSink sink;
    const auto snes48 = detail::AudioSystemTestAccess::makeManual(AudioKind::Chiptune, sink, VMPlatform::Snes,
                                                                  TimingProfile::Snes, 48'000);
    EXPECT_EQ(detail::AudioSystemTestAccess::framesPerStep(*snes48), 798u);

    test::CaptureAudioSink sink441;
    const auto snes441 = detail::AudioSystemTestAccess::makeManual(AudioKind::Chiptune, sink441,
                                                                   VMPlatform::Snes, TimingProfile::Snes, 44'100);
    EXPECT_EQ(detail::AudioSystemTestAccess::framesPerStep(*snes441), 733u);

    test::CaptureAudioSink sinkGb;
    const auto gameBoy = detail::AudioSystemTestAccess::makeManual(
        AudioKind::Chiptune, sinkGb, VMPlatform::GameBoyColor, TimingProfile::GameBoyColor, 48'000);
    EXPECT_EQ(detail::AudioSystemTestAccess::framesPerStep(*gameBoy), 803u);
}

}  // namespace
}  // namespace retropp
