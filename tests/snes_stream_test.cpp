// An audio file longer than audio RAM holds, played through the SNES's sound chip: the system encodes it
// once, keeps it, and delivers its blocks into a fixed window of audio RAM for the cue's voice as the voice
// plays through them. The window is an opening, written once, and a loop the chip plays round, its blocks
// written again as the voice's decoder leaves them. Device-free: every system here is manual, its sampler
// machine stepped a frame at a time on this thread, so the window and the voice's decoder are read
// straight from that machine between frames.

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "mock_platform.h"  // test::CaptureAudioSink
#include "retropp/asset_policy.h"
#include "retropp/asset_registry.h"  // detail::findEmbeddedAsset
#include "retropp/audio_effect.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "snaggletooth/snes/snes.h"  // snaggletooth::consoleClock — the two console clocks
#include "src/audio/audio_system_testing.h"
#include "src/audio/pcm_decode.h"
#include "src/audio/snes/brr.h"
#include "src/audio/snes/default_driver.h"
#include "src/audio/snes/sample_stream.h"
#include "src/vm/vm_core_access.h"

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;
namespace brr = audio::brr;
namespace sd  = audio::snesdriver;
namespace ss  = audio::snesstream;

using Block = std::array<std::uint8_t, brr::kBlockBytes>;

// The files, registered once: a 6 s sweep that encodes to far more than audio RAM, a 1 s sweep that
// encodes to more than a window and less than audio RAM, a 600-block sample in the chip's own format whose
// last block ends without looping, and the 20 ms tone that loads whole.
AudioId longFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/long.wav", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}
AudioId midFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/mid.wav", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}
AudioId longBrrFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/long.brr", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}
AudioId toneFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/tone.wav", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}

// A file as the system encodes it: decoded at the chip's rate, folded to mono, encoded to loop round from
// its start; a `.brr` as it is.
std::vector<std::uint8_t> encoded(const char* path) {
    const std::span<const std::uint8_t> file = detail::findEmbeddedAsset(path);
    EXPECT_FALSE(file.empty()) << path;
    const std::string name(path);
    if (name.ends_with(".brr")) {
        return std::vector<std::uint8_t>(file.begin(), file.end());
    }
    return brr::encode(brr::toMono(detail::decodePcm(file, brr::kSampleRate)), /*loop=*/true);
}

// A block with its END and LOOP flags cleared — what a block of the file is, wherever it was written.
Block unflagged(Block b) {
    b[0] = static_cast<std::uint8_t>(b[0] & ~0x03);
    return b;
}
Block blockOf(const std::vector<std::uint8_t>& file, std::size_t index) {
    Block b{};
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(index * brr::kBlockBytes), brr::kBlockBytes, b.begin());
    return b;
}

// Every block of a file, unflagged, to its index — asserting first that no two are alike, which is what
// lets a block read from the window name the one place in the file it came from.
std::map<Block, std::size_t> indexOf(const std::vector<std::uint8_t>& file) {
    std::map<Block, std::size_t> index;
    const std::size_t            blocks = file.size() / brr::kBlockBytes;
    for (std::size_t i = 0; i < blocks; ++i) {
        index.emplace(unflagged(blockOf(file, i)), i);
    }
    EXPECT_EQ(index.size(), blocks) << "every block of the fixture differs from every other";
    return index;
}

std::unique_ptr<AudioSystem> snesSystem(test::CaptureAudioSink& sink) {
    return Access::makeManual(AudioKind::Chiptune, sink, VMPlatform::Snes, TimingProfile::Snes);
}

Vm& sampler(AudioSystem& sys) { return Access::voiceMachine(sys, 0); }

std::vector<std::uint8_t> audioRam(AudioSystem& sys, std::uint16_t at, std::uint32_t size) {
    return sampler(sys).read(MemoryRegion{.at = snes::audioRam(at), .size = size});
}
Block ramBlock(AudioSystem& sys, std::uint16_t at) {
    const std::vector<std::uint8_t> bytes = audioRam(sys, at, brr::kBlockBytes);
    Block                           b{};
    std::copy(bytes.begin(), bytes.end(), b.begin());
    return b;
}
std::uint8_t dsp(AudioSystem& sys, std::uint8_t reg) {
    return sampler(sys).read(MemoryRegion{.at = snes::dspRegister(reg), .size = 1}).front();
}

// A directory entry: where its sample starts, and where a voice continues when it reaches an end block
// that loops.
struct DirectoryEntry {
    std::uint16_t start = 0;
    std::uint16_t loop  = 0;
};
DirectoryEntry entry(AudioSystem& sys, std::uint8_t index) {
    const std::vector<std::uint8_t> e = audioRam(sys, static_cast<std::uint16_t>(sd::kDirectory + 4 * index), 4);
    return DirectoryEntry{.start = static_cast<std::uint16_t>(e[0] | (e[1] << 8)),
                          .loop  = static_cast<std::uint16_t>(e[2] | (e[3] << 8))};
}
// The directory entry a voice plays, as its SRCN register names it.
DirectoryEntry voiceEntry(AudioSystem& sys, std::uint8_t voice) {
    return entry(sys, dsp(sys, static_cast<std::uint8_t>(voice * 0x10 + 0x04)));
}

// A console's clock, as the machine answers it.
vm::MachineClock clockOf(snaggletooth::Region region) {
    const snaggletooth::ConsoleClock c = snaggletooth::consoleClock(region);
    return vm::MachineClock{.hertzNumerator = static_cast<std::uint32_t>(c.hertzNumerator),
                            .hertzDivisor   = static_cast<std::uint32_t>(c.hertzDenominator),
                            .cyclesPerFrame = static_cast<std::uint32_t>(c.masterCyclesPerFrame)};
}

std::uint16_t decoderOf(AudioSystem& sys, std::uint8_t voice) {
    return vm::VmCoreAccess::sampleDecoderAddress(sampler(sys), voice);
}

// One frame of the sampler's machine, mixed, and the frames it made.
std::vector<AudioFrame> frame(AudioSystem& sys, test::CaptureAudioSink& sink) {
    Access::stepVoice(sys, 0);
    Access::mix(sys);
    return sink.drain(sys.audioStats().framesBuffered);
}
std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    return static_cast<std::size_t>(std::count_if(
        frames.begin(), frames.end(), [](const AudioFrame& f) { return f.left != 0 || f.right != 0; }));
}

// The block after `at` on the voice's way through the window: on through the opening into the loop, and
// from the loop's last block back to its first.
std::uint16_t nextInWindow(std::uint16_t at, const DirectoryEntry& window) {
    const auto lastOfLoop = static_cast<std::uint16_t>(window.loop + (ss::kStreamLoopBlocks - 1) * brr::kBlockBytes);
    return at == lastOfLoop ? window.loop : static_cast<std::uint16_t>(at + brr::kBlockBytes);
}

// The block the voice's decoder is in, and the `ahead` after it on its way through the window, are the
// file's blocks in order — the file going round from its last block to its first. Answers the file block
// the decoder is in.
std::size_t requireFilesBlocksAhead(AudioSystem& sys, std::uint8_t voice, const std::vector<std::uint8_t>& file,
                                    const std::map<Block, std::size_t>& index, std::size_t ahead) {
    const DirectoryEntry window  = voiceEntry(sys, voice);
    const std::size_t    blocks  = file.size() / brr::kBlockBytes;
    std::uint16_t        at      = decoderOf(sys, voice);
    const auto           found   = index.find(unflagged(ramBlock(sys, at)));
    EXPECT_NE(found, index.end()) << "the decoder is in a block of the file";
    if (found == index.end()) {
        return 0;
    }
    const std::size_t first = found->second;
    for (std::size_t i = 1; i <= ahead; ++i) {
        at = nextInWindow(at, window);
        EXPECT_EQ(unflagged(ramBlock(sys, at)), unflagged(blockOf(file, (first + i) % blocks)))
            << "the block " << i << " after the decoder's";
        if (unflagged(ramBlock(sys, at)) != unflagged(blockOf(file, (first + i) % blocks))) {
            break;
        }
    }
    return first;
}

// A voice played at `pitch` for `frames` frames: after each, the decoder finds the file's next blocks ahead
// of it, and it has moved on through the file by what a frame plays at that pitch.
void requireDeliveryKeepsUp(AudioSystem& sys, test::CaptureAudioSink& sink, std::uint8_t voice,
                            const std::vector<std::uint8_t>& file, int frames, std::size_t leastPerFrame,
                            std::size_t mostPerFrame) {
    const std::map<Block, std::size_t> index  = indexOf(file);
    const std::size_t                  blocks = file.size() / brr::kBlockBytes;
    const std::size_t                  ahead  = ss::blocksPerStepAtMostFor(clockOf(snaggletooth::Region::Ntsc));
    frame(sys, sink);  // a cue's key-on lands in the frame after it
    std::size_t last = requireFilesBlocksAhead(sys, voice, file, index, ahead);
    for (int f = 0; f < frames; ++f) {
        frame(sys, sink);
        const std::size_t now   = requireFilesBlocksAhead(sys, voice, file, index, ahead);
        const std::size_t moved = (now + blocks - last) % blocks;
        EXPECT_GE(moved, leastPerFrame) << "frame " << f;
        EXPECT_LE(moved, mostPerFrame) << "frame " << f;
        if (::testing::Test::HasFailure()) {
            return;
        }
        last = now;
    }
}

// At the pitch a file was recorded at a voice plays 532 or 533 of the chip's samples a frame of the 60 Hz
// console: 33 or 34 blocks. A frame's first look at the decoder can land either side of a group of four,
// so a frame's count is one block wider either way.
constexpr std::size_t kLeastAsRecorded = 32;
constexpr std::size_t kMostAsRecorded  = 35;

TEST(SnesStream, ALongFileCuedPlaysFromAFixedWindow) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.wav");
    ASSERT_GT(file.size(), std::size_t{sd::kSamplesEnd - sd::kSamplesStart}) << "larger than all of audio RAM";

    ASSERT_NO_THROW(sys->play(longFile(), Cue{.voice = 0}));
    EXPECT_EQ(Access::streamedVoices(*sys), 0x01u);

    // The window's directory entry: the opening, where a key-on starts; the loop, where the chip goes round.
    const DirectoryEntry window = entry(*sys, 0);
    EXPECT_EQ(window.start, sd::kSamplesStart);
    EXPECT_EQ(window.loop, sd::kSamplesStart + ss::kStreamOpeningBlocks * brr::kBlockBytes);

    // The opening holds the file's first 176 blocks with their end bits clear; the loop's last block ends
    // and loops.
    for (std::size_t i = 0; i < ss::kStreamOpeningBlocks; ++i) {
        const Block b = ramBlock(*sys, static_cast<std::uint16_t>(window.start + i * brr::kBlockBytes));
        ASSERT_EQ(b, unflagged(blockOf(file, i))) << "opening block " << i;
    }
    const Block last =
        ramBlock(*sys, static_cast<std::uint16_t>(window.loop + (ss::kStreamLoopBlocks - 1) * brr::kBlockBytes));
    EXPECT_EQ(last[0] & 0x03, 0x03);
    EXPECT_EQ(unflagged(last), unflagged(blockOf(file, ss::kStreamWindowBlocks - 1)));

    // The window took 4095 bytes of audio RAM and one directory entry: the next sample goes right after it.
    sys->play(toneFile(), Cue{.voice = 1});
    EXPECT_EQ(entry(*sys, 1).start, sd::kSamplesStart + ss::kStreamWindowBytes);
    EXPECT_EQ(ss::kStreamWindowBytes, 4095u);

    // And the voice sounds.
    std::size_t sounding = 0;
    for (int f = 0; f < 10; ++f) {
        sounding += nonSilentCount(frame(*sys, sink));
    }
    EXPECT_GT(sounding, 1000u);
}

TEST(SnesStream, TheThresholdIsTheWindowsSize) {
    const auto file = encoded("tests/fixtures/mid.wav");
    ASSERT_GT(file.size(), ss::kStreamWindowBytes);
    ASSERT_LT(file.size(), std::size_t{sd::kSamplesEnd - sd::kSamplesStart});

    // A file the size of the threshold loads whole: its bytes contiguous, its entry its start twice.
    test::CaptureAudioSink wholeSink;
    auto                   whole = snesSystem(wholeSink);
    Access::streamThreshold(*whole, file.size());
    whole->play(midFile(), Cue{.voice = 0});
    EXPECT_EQ(Access::streamedVoices(*whole), 0x00u);
    EXPECT_EQ(entry(*whole, 0).start, sd::kSamplesStart);
    EXPECT_EQ(entry(*whole, 0).loop, sd::kSamplesStart);
    EXPECT_EQ(audioRam(*whole, sd::kSamplesStart, static_cast<std::uint32_t>(file.size())), file);

    // One byte under its size, it streams.
    test::CaptureAudioSink streamSink;
    auto                   streamed = snesSystem(streamSink);
    Access::streamThreshold(*streamed, file.size() - 1);
    streamed->play(midFile(), Cue{.voice = 0});
    EXPECT_EQ(Access::streamedVoices(*streamed), 0x01u);
    EXPECT_EQ(entry(*streamed, 0).loop, sd::kSamplesStart + ss::kStreamOpeningBlocks * brr::kBlockBytes);
}

TEST(SnesStream, TheDecoderAlwaysFindsTheFilesNextBlocks) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(longFile(), Cue{.voice = 0});
    requireDeliveryKeepsUp(*sys, sink, 0, encoded("tests/fixtures/long.wav"), 400, kLeastAsRecorded,
                           kMostAsRecorded);
}

TEST(SnesStream, AStreamedFileProducesTheFramesOfTheSameFileLoadedWhole) {
    test::CaptureAudioSink wholeSink, streamSink;
    auto                   whole    = snesSystem(wholeSink);
    auto                   streamed = snesSystem(streamSink);
    const auto             file     = encoded("tests/fixtures/mid.wav");
    Access::streamThreshold(*whole, file.size());  // loads whole here, streams on the other
    whole->play(midFile(), Cue{.voice = 0, .mode = PlayMode::continuous()});
    streamed->play(midFile(), Cue{.voice = 0, .mode = PlayMode::continuous()});
    ASSERT_EQ(Access::streamedVoices(*whole), 0x00u);
    ASSERT_EQ(Access::streamedVoices(*streamed), 0x01u);

    // Two full passes of the file and a little more, at the system's 48 kHz.
    const std::size_t passFrames = file.size() / brr::kBlockBytes * brr::kBlockSamples * 48'000 / brr::kSampleRate;
    std::vector<AudioFrame> a, b;
    while (a.size() < 2 * passFrames + 4'800) {
        const std::vector<AudioFrame> fa = frame(*whole, wholeSink);
        const std::vector<AudioFrame> fb = frame(*streamed, streamSink);
        a.insert(a.end(), fa.begin(), fa.end());
        b.insert(b.end(), fb.begin(), fb.end());
    }
    ASSERT_EQ(a.size(), b.size());
    EXPECT_GT(nonSilentCount(a), a.size() / 2);
    std::size_t firstDifference = a.size();
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].left != b[i].left || a[i].right != b[i].right) {
            firstDifference = i;
            break;
        }
    }
    EXPECT_EQ(firstDifference, a.size()) << "the first frame that differs";
}

TEST(SnesStream, TheHighestPitchStillFindsItsBlocks) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(longFile(), Cue{.voice = 0, .effect = AudioEffect{.pitch = 4.0f}});
    // $3FFF plays 2130 of the chip's samples a frame: 133 blocks, a block either way.
    requireDeliveryKeepsUp(*sys, sink, 0, encoded("tests/fixtures/long.wav"), 400, 132, 135);
}

TEST(SnesStream, APitchChangeWhileItPlaysKeepsTheBlocksInOrder) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.wav");
    sys->play(longFile(), Cue{.voice = 0});
    requireDeliveryKeepsUp(*sys, sink, 0, file, 30, kLeastAsRecorded, kMostAsRecorded);
    sys->effect(0, AudioEffect{.pitch = 4.0f});
    requireDeliveryKeepsUp(*sys, sink, 0, file, 60, kLeastAsRecorded, 135);
    sys->effect(0, AudioEffect{.pitch = 0.25f});
    requireDeliveryKeepsUp(*sys, sink, 0, file, 30, 7, 135);
    sys->effect(0, AudioEffect{.pitch = 3.0f});
    requireDeliveryKeepsUp(*sys, sink, 0, file, 60, 7, 135);
}

// The window's bytes, as they stand.
std::vector<std::uint8_t> windowBytes(AudioSystem& sys, std::uint8_t voice) {
    return audioRam(sys, voiceEntry(sys, voice).start, static_cast<std::uint32_t>(ss::kStreamWindowBytes));
}

// Whether the window holds the file's last block written with END alone.
bool endsOnTheLastBlock(AudioSystem& sys, std::uint8_t voice, const std::vector<std::uint8_t>& file) {
    const std::vector<std::uint8_t> window = windowBytes(sys, voice);
    const Block                     last   = unflagged(blockOf(file, file.size() / brr::kBlockBytes - 1));
    for (std::size_t i = 0; i < ss::kStreamWindowBlocks; ++i) {
        Block b{};
        std::copy_n(window.begin() + static_cast<std::ptrdiff_t>(i * brr::kBlockBytes), brr::kBlockBytes, b.begin());
        if (unflagged(b) == last && (b[0] & 0x03) == 0x01) {
            return true;
        }
    }
    return false;
}

TEST(SnesStream, OnceEndsOnTheFilesLastBlock) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.wav");
    sys->play(longFile(), Cue{.voice = 0, .mode = PlayMode::once()});

    // The file is 12'000 blocks; at 33 a frame its pass is a little over 360 frames.
    std::size_t sounding = 0;
    for (int f = 0; f < 340; ++f) {
        sounding += nonSilentCount(frame(*sys, sink));
    }
    EXPECT_GT(sounding, 200'000u) << "it sounds through the pass";
    for (int f = 0; f < 40; ++f) {
        frame(*sys, sink);
    }
    EXPECT_TRUE(endsOnTheLastBlock(*sys, 0, file));

    // Past the end the voice is silent and the window stands as it is.
    const std::vector<std::uint8_t> ended = windowBytes(*sys, 0);
    for (int f = 0; f < 60; ++f) {
        EXPECT_EQ(nonSilentCount(frame(*sys, sink)), 0u) << "frame " << f << " after the end";
    }
    EXPECT_EQ(windowBytes(*sys, 0), ended);
}

TEST(SnesStream, ContinuousCarriesOnFromTheFilesFirstBlock) {
    test::CaptureAudioSink sink;
    auto                   sys    = snesSystem(sink);
    const auto             file   = encoded("tests/fixtures/long.wav");
    const auto             index  = indexOf(file);
    const std::size_t      blocks = file.size() / brr::kBlockBytes;
    sys->play(longFile(), Cue{.voice = 0, .mode = PlayMode::continuous()});

    // Frame by frame until the file's last block is in reach of the decoder: the block after it is the
    // file's first.
    bool seen = false;
    for (int f = 0; f < 400 && !seen; ++f) {
        frame(*sys, sink);
        const DirectoryEntry window = voiceEntry(*sys, 0);
        std::uint16_t        at     = decoderOf(*sys, 0);
        for (std::size_t i = 0; i < 100; ++i, at = nextInWindow(at, window)) {
            const auto found = index.find(unflagged(ramBlock(*sys, at)));
            if (found != index.end() && found->second == blocks - 1) {
                EXPECT_EQ(unflagged(ramBlock(*sys, nextInWindow(at, window))), unflagged(blockOf(file, 0)));
                seen = true;
                break;
            }
        }
    }
    ASSERT_TRUE(seen) << "the file's last block reached the window";

    // And past the end of the file the voice plays on.
    std::size_t sounding = 0;
    for (int f = 0; f < 20; ++f) {
        sounding += nonSilentCount(frame(*sys, sink));
    }
    EXPECT_GT(sounding, 10'000u);
}

TEST(SnesStream, ARepeatStrikesFromTheOpeningAndTheLoopFollows) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.wav");
    // 30 a minute: a strike every 120 frames, about a third of the way through the file.
    sys->play(longFile(), Cue{.voice = 0, .mode = PlayMode::repeat(30.0f)});
    const DirectoryEntry window = voiceEntry(*sys, 0);
    const auto inOpening = [&](std::uint16_t at) { return at >= window.start && at < window.loop; };

    // The first lap of the loop ends about fourteen frames in; the voice sounds on across it and after.
    std::size_t quietFrames = 0;
    bool        wasInLoop   = false;
    bool        struck      = false;
    for (int f = 0; f < 140 && !struck; ++f) {
        const std::vector<AudioFrame> frames = frame(*sys, sink);
        if (f >= 3 && nonSilentCount(frames) == 0) {
            ++quietFrames;
        }
        const std::uint16_t at = decoderOf(*sys, 0);
        if (wasInLoop && inOpening(at)) {
            struck = true;
            EXPECT_GE(f, 110) << "the strike comes at the tempo";
            // The loop holds the file's blocks from the one after the opening's last, round to its last slot.
            for (std::size_t slot = 0; slot < ss::kStreamLoopBlocks; ++slot) {
                const Block b = ramBlock(*sys, static_cast<std::uint16_t>(window.loop + slot * brr::kBlockBytes));
                ASSERT_EQ(unflagged(b), unflagged(blockOf(file, ss::kStreamOpeningBlocks + slot))) << "loop slot " << slot;
            }
        }
        wasInLoop = !inOpening(at);
    }
    EXPECT_TRUE(struck);
    EXPECT_EQ(quietFrames, 0u) << "the voice is not keyed off as it goes round the loop";

    // After the strike the decoder finds the file's blocks ahead of it from the file's start again.
    requireDeliveryKeepsUp(*sys, sink, 0, file, 40, kLeastAsRecorded, kMostAsRecorded);
}

TEST(SnesStream, ABrrFileStreamsWithItsOwnEnding) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.brr");
    ASSERT_EQ(file.size(), 600u * brr::kBlockBytes);
    ASSERT_EQ(file[599 * brr::kBlockBytes] & 0x03, 0x01) << "the file's own last block ends without looping";
    sys->play(longBrrFile(), Cue{.voice = 0, .mode = PlayMode::continuous()});
    ASSERT_EQ(Access::streamedVoices(*sys), 0x01u);

    std::size_t sounding = 0;
    for (int f = 0; f < 12; ++f) {
        sounding += nonSilentCount(frame(*sys, sink));
    }
    EXPECT_GT(sounding, 5'000u);
    for (int f = 0; f < 20; ++f) {
        frame(*sys, sink);
    }
    EXPECT_TRUE(endsOnTheLastBlock(*sys, 0, file));
    for (int f = 0; f < 20; ++f) {
        EXPECT_EQ(nonSilentCount(frame(*sys, sink)), 0u) << "frame " << f << " after the end";
    }
}

TEST(SnesStream, TwoVoicesStreamOneFileEachAtItsOwnPlace) {
    test::CaptureAudioSink sink;
    auto                   sys  = snesSystem(sink);
    const auto             file = encoded("tests/fixtures/long.wav");
    sys->play(longFile(), Cue{.voice = 1});
    for (int f = 0; f < 40; ++f) {
        frame(*sys, sink);
    }
    sys->play(longFile(), Cue{.voice = 2});
    EXPECT_EQ(Access::streamedVoices(*sys), 0x06u);
    frame(*sys, sink);  // the key-on, which sets the voice's sample
    EXPECT_EQ(voiceEntry(*sys, 1).start, sd::kSamplesStart);
    EXPECT_EQ(voiceEntry(*sys, 2).start, sd::kSamplesStart + ss::kStreamWindowBytes);

    // Each voice finds the file's next blocks in its own window, the two forty frames apart.
    const std::map<Block, std::size_t> index = indexOf(file);
    for (int f = 0; f < 200; ++f) {
        frame(*sys, sink);
        const std::size_t one = requireFilesBlocksAhead(*sys, 1, file, index, 134);
        const std::size_t two = requireFilesBlocksAhead(*sys, 2, file, index, 134);
        ASSERT_GT(one, two) << "frame " << f;
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST(SnesStream, EightVoicesStreamAndAShortSampleStillLoads) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    for (std::uint8_t v = 0; v < sd::kVoices; ++v) {
        sys->play(longFile(), Cue{.voice = v});
    }
    EXPECT_EQ(Access::streamedVoices(*sys), 0xFFu);
    EXPECT_EQ(8 * ss::kStreamWindowBytes, 32'760u);

    // The short tone loads whole after the eight windows, and sounds.
    sys->play(toneFile(), Cue{.voice = 3});
    EXPECT_EQ(entry(*sys, 8).start, sd::kSamplesStart + 8 * ss::kStreamWindowBytes);
    EXPECT_EQ(Access::streamedVoices(*sys), 0xF7u) << "voice 3 plays the tone, loaded whole";
    std::size_t sounding = 0;
    for (int f = 0; f < 4; ++f) {
        sounding += nonSilentCount(frame(*sys, sink));
    }
    EXPECT_GT(sounding, 1000u);

    // Another long file on a voice that has its window streams there: nothing more of audio RAM is taken.
    sys->play(longBrrFile(), Cue{.voice = 5});
    sys->play(midFile(), Cue{.voice = 3});  // streams in voice 3's window, taken when it first streamed
    sys->play(AudioLibrary::instance().registerAudio("tests/fixtures/tone.ogg", AudioType::Sfx, AssetPolicy::Embed),
              Cue{.voice = 6});
    frame(*sys, sink);  // the key-ons, which set the voices' samples
    EXPECT_EQ(voiceEntry(*sys, 5).start, sd::kSamplesStart + 5 * ss::kStreamWindowBytes);
    EXPECT_EQ(voiceEntry(*sys, 3).start, sd::kSamplesStart + 3 * ss::kStreamWindowBytes);
    EXPECT_EQ(voiceEntry(*sys, 6).start,
              sd::kSamplesStart + 8 * ss::kStreamWindowBytes + encoded("tests/fixtures/tone.wav").size())
        << "the next sample goes right after the tone";
}

TEST(SnesStream, StopEndsTheDeliveryAndAWindowThatDoesNotFitIsRefused) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(longFile(), Cue{.voice = 4});
    for (int f = 0; f < 20; ++f) {
        frame(*sys, sink);
    }
    sys->stop(4);
    frame(*sys, sink);  // the key-off
    EXPECT_EQ(Access::streamedVoices(*sys), 0x00u);
    const std::vector<std::uint8_t> stopped = windowBytes(*sys, 4);
    for (int f = 0; f < 60; ++f) {
        frame(*sys, sink);
    }
    EXPECT_EQ(windowBytes(*sys, 4), stopped);

    // Audio RAM filled with whole samples until a window does not fit: the cue that needs one is refused.
    test::CaptureAudioSink fullSink;
    auto                   full = snesSystem(fullSink);
    Access::streamThreshold(*full, 1u << 16);  // the 1 s sweep loads whole
    for (int i = 0; i < 3; ++i) {
        full->play(AudioLibrary::instance().registerAudio("tests/fixtures/mid.wav", AudioType::Sfx, AssetPolicy::Embed),
                   Cue{.voice = 0});
    }
    full->play(longFile(), Cue{.voice = 1});
    full->play(longFile(), Cue{.voice = 2});
    EXPECT_THROW(full->play(longFile(), Cue{.voice = 3}), std::length_error);
}

// ── The arithmetic ──────────────────────────────────────────────────────────────────────────────────

// A file of `blocks` blocks, each holding its own index, so a written block names its place in the file.
std::shared_ptr<const std::vector<std::uint8_t>> numbered(std::size_t blocks, std::uint8_t lastFlags) {
    auto file = std::make_shared<std::vector<std::uint8_t>>(blocks * brr::kBlockBytes, 0);
    for (std::size_t i = 0; i < blocks; ++i) {
        (*file)[i * brr::kBlockBytes + 1] = static_cast<std::uint8_t>(i & 0xFF);
        (*file)[i * brr::kBlockBytes + 2] = static_cast<std::uint8_t>(i >> 8);
    }
    (*file)[(blocks - 1) * brr::kBlockBytes] = lastFlags;
    return file;
}
std::size_t numberOf(const ss::Write& w) { return w.block[1] | (w.block[2] << 8); }

TEST(SnesStreamArithmetic, TheWindowOutlastsAStepOnBothClocks) {
    const vm::MachineClock ntsc = clockOf(snaggletooth::Region::Ntsc);
    const vm::MachineClock pal  = clockOf(snaggletooth::Region::Pal);
    EXPECT_EQ(ss::blocksPerStepAtMostFor(ntsc), 134u);
    EXPECT_EQ(ss::blocksPerStepAtMostFor(pal), 161u);
    for (const vm::MachineClock& clock : {ntsc, pal}) {
        EXPECT_LE(ss::blocksPerStepAtMostFor(clock) + ss::kStreamGuardBlocks, ss::kStreamOpeningBlocks);
        EXPECT_LE(ss::blocksPerStepAtMostFor(clock), std::size_t{ss::kStreamLoopBlocks - ss::kStreamGuardBlocks});
        EXPECT_NO_THROW(ss::requireStreamable(clock));
    }
    vm::MachineClock slow = ntsc;  // a frame twice as long plays twice the blocks
    slow.cyclesPerFrame *= 2;
    EXPECT_THROW(ss::requireStreamable(slow), std::logic_error);

    // The start: the opening, then the whole loop, its last block ending and looping.
    constexpr std::uint16_t kOpening = 0x1000;
    const std::uint16_t     loop     = kOpening + ss::kStreamOpeningBlocks * brr::kBlockBytes;
    ss::Stream              stream(numbered(1000, 0x03), kOpening, /*selfEnding=*/false);
    const std::vector<ss::Write> start = stream.start();
    ASSERT_EQ(start.size(), ss::kStreamWindowBlocks);
    for (std::size_t i = 0; i < start.size(); ++i) {
        EXPECT_EQ(start[i].address, kOpening + i * brr::kBlockBytes);
        EXPECT_EQ(numberOf(start[i]), i);
        EXPECT_EQ(start[i].block[0], i == ss::kStreamWindowBlocks - 1 ? 0x03 : 0x00);
    }

    // In the opening, nothing; in the loop, the blocks it has left, up to the guard behind it.
    const auto slotAddress = [&](std::size_t slot) {
        return static_cast<std::uint16_t>(loop + slot * brr::kBlockBytes);
    };
    EXPECT_TRUE(stream.advance(kOpening + 100 * brr::kBlockBytes).empty());
    EXPECT_TRUE(stream.advance(slotAddress(2)).empty()) << "two behind it are the guard";
    std::vector<ss::Write> w = stream.advance(slotAddress(10));
    ASSERT_EQ(w.size(), 8u);
    for (std::size_t i = 0; i < w.size(); ++i) {
        EXPECT_EQ(w[i].address, slotAddress(i));
        EXPECT_EQ(numberOf(w[i]), ss::kStreamWindowBlocks + i);
    }
    // Round the loop: the slots behind it written, the last one ending and looping, two left as the guard.
    w = stream.advance(slotAddress(5));
    ASSERT_EQ(w.size(), ss::kStreamLoopBlocks - 5u);  // from slot 8 round to slot 2
    EXPECT_EQ(w.front().address, slotAddress(8));
    EXPECT_EQ(w.back().address, slotAddress(2));
    for (const ss::Write& x : w) {
        EXPECT_EQ(x.block[0], x.address == slotAddress(ss::kStreamLoopBlocks - 1) ? 0x03 : 0x00);
    }
    // Keyed on again: the whole loop again, from the file's block after the opening's last.
    w = stream.advance(kOpening);
    ASSERT_EQ(w.size(), ss::kStreamLoopBlocks);
    EXPECT_EQ(w.front().address, slotAddress(0));
    EXPECT_EQ(numberOf(w.front()), ss::kStreamOpeningBlocks);
    EXPECT_EQ(numberOf(w.back()), ss::kStreamWindowBlocks - 1);
    EXPECT_TRUE(stream.advance(0x0400).empty()) << "outside the window, nothing";

    // A file that ends itself: its last block with END alone, wherever it lands, and nothing after it.
    ss::Stream                   once(numbered(500, 0x01), kOpening, /*selfEnding=*/true);
    const std::vector<ss::Write> first = once.start();
    EXPECT_FALSE(once.drained());
    w = once.advance(slotAddress(100));
    ASSERT_FALSE(w.empty());
    EXPECT_EQ(numberOf(w.back()), 499u);
    EXPECT_EQ(w.back().block[0], 0x01);
    EXPECT_EQ(w.size(), 500u - ss::kStreamWindowBlocks);
    EXPECT_TRUE(once.drained());
    EXPECT_TRUE(once.advance(slotAddress(200)).empty());
    EXPECT_EQ(first.size(), ss::kStreamWindowBlocks);

    // A file that goes round: the block after its last is its first.
    ss::Stream round(numbered(460, 0x03), kOpening, /*selfEnding=*/false);
    (void)round.start();
    w = round.advance(slotAddress(9));
    ASSERT_EQ(w.size(), 7u);
    EXPECT_EQ(numberOf(w[4]), 459u);
    EXPECT_EQ(w[4].block[0], 0x00) << "the file's own END and LOOP are cleared";
    EXPECT_EQ(numberOf(w[5]), 0u);
}

}  // namespace
}  // namespace retropp
