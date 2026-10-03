// A registered audio file through the SNES's sound chip: play(id, Cue{…}) on an AudioSystem::SNES of the
// Chiptune kind. The system hosts the engine's default sound driver by itself, converts the file to the
// chip's sample format and loads it once, keys the cue's voice on with the cue's effect, changes the voice
// as it plays on effect(voice, …), and keys it off on stop(voice) or stop(). A `.brr` file loads as it is and streams on a Pcm system through the format's
// own decoder. Device-free: every system here is manual, stepped on this thread, so what a cue left in
// audio RAM and in the chip's registers is read straight from the sampler's machine.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
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
#include "src/audio/audio_system_testing.h"
#include "src/audio/pcm_decode.h"
#include "src/audio/snes/brr.h"
#include "src/audio/snes/default_driver.h"

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;
namespace brr = audio::brr;
namespace sd  = audio::snesdriver;

// The two audio files, registered once: a 20 ms stereo tone the engine converts, and a one-block looping
// square in the chip's own format.
AudioId toneFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/tone.wav", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}
AudioId loopFile() {
    static const AudioId id =
        AudioLibrary::instance().registerAudio("tests/fixtures/loop.brr", AudioType::Sfx, AssetPolicy::Embed);
    return id;
}

std::unique_ptr<AudioSystem> snesSystem(test::CaptureAudioSink& sink) {
    return Access::makeManual(AudioKind::Chiptune, sink, VMPlatform::Snes, TimingProfile::Snes);
}

// One production pass, and everything it buffered.
std::vector<AudioFrame> stepAndDrain(AudioSystem& sys, test::CaptureAudioSink& sink) {
    Access::step(sys);
    return sink.drain(sys.audioStats().framesBuffered);
}

std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    return static_cast<std::size_t>(std::count_if(
        frames.begin(), frames.end(), [](const AudioFrame& f) { return f.left != 0 || f.right != 0; }));
}

// A byte of the sampler's audio RAM, and a register of its chip, as they stand.
std::uint8_t audioRam(AudioSystem& sys, std::uint16_t at) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = snes::audioRam(at), .size = 1}).front();
}
// A byte of the driver's work RAM.
std::uint8_t workRam(AudioSystem& sys, std::uint32_t at) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = at, .size = 1}).front();
}
std::uint8_t dsp(AudioSystem& sys, std::uint8_t reg) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = snes::dspRegister(reg), .size = 1}).front();
}

TEST(SnesSampler, TheBakedSoundProgramIsOnePage) {
    const AudioLibrary::Entry& entry = AudioLibrary::instance().entry(sd::defaultDriver().id());
    ASSERT_TRUE(entry.driver.has_value());
    ASSERT_EQ(entry.driver->images.size(), 3u);
    EXPECT_EQ(entry.driver->images[2].bytes.size(), 256u);  // the init uploads exactly $0100 bytes
    EXPECT_EQ(entry.driver->images[2].base, 0x00A000u);
}

TEST(SnesSampler, AnAudioFileCuedHostsTheSamplerAndLoadsItConverted) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    EXPECT_EQ(Access::voiceCount(*sys), 0u);

    sys->play(toneFile(), Cue{.voice = 2});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(Access::voiceCount(*sys), 1u);  // the sampler, and nothing else

    // The file, as the engine converted it: decoded at the chip's rate, folded to mono, encoded.
    const std::span<const std::uint8_t> file = detail::findEmbeddedAsset("tests/fixtures/tone.wav");
    ASSERT_FALSE(file.empty());
    const std::vector<std::uint8_t> expected =
        brr::encode(brr::toMono(detail::decodePcm(file, brr::kSampleRate)), /*loop=*/true);  // round from its start
    EXPECT_EQ(expected.size(), 40u * brr::kBlockBytes);  // 20 ms at 32 kHz: 640 samples, 40 blocks
    const std::vector<std::uint8_t> loaded = Access::voiceMachine(*sys, 0).read(
        MemoryRegion{.at = snes::audioRam(sd::kSamplesStart), .size = static_cast<std::uint32_t>(expected.size())});
    EXPECT_EQ(loaded, expected);

    // Its directory entry: the sample's start, and its start again as where a looping one continues.
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 0), 0x00u);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 1), 0x04u);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 2), 0x00u);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 3), 0x04u);

    // The voice plays entry 0, as recorded, full on both sides.
    EXPECT_EQ(dsp(*sys, 0x24), 0x00u);  // V2SRCN
    EXPECT_EQ(dsp(*sys, 0x22), 0x00u);  // V2PITCHL
    EXPECT_EQ(dsp(*sys, 0x23), 0x10u);  // V2PITCHH
    EXPECT_EQ(dsp(*sys, 0x20), 0x7Fu);  // V2VOLL
    EXPECT_EQ(dsp(*sys, 0x21), 0x7Fu);  // V2VOLR

    // Cued again it is not loaded again: the next sample would go right after it.
    sys->play(toneFile(), Cue{.voice = 3});
    sys->play(loopFile(), Cue{.voice = 4});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 4), static_cast<std::uint8_t>((sd::kSamplesStart + 360) & 0xFF));
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 5), static_cast<std::uint8_t>((sd::kSamplesStart + 360) >> 8));
    EXPECT_EQ(dsp(*sys, 0x44), 0x01u);  // V4SRCN: the second entry
}

TEST(SnesSampler, AnOggFileCuedLoadsConvertedAndSounds) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(AudioLibrary::instance().registerAudio("tests/fixtures/tone.ogg", AudioType::Sfx, AssetPolicy::Embed),
              Cue{.voice = 6});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);

    // The file, as the engine converted it: decoded at the chip's rate, folded to mono, encoded.
    const std::span<const std::uint8_t> file = detail::findEmbeddedAsset("tests/fixtures/tone.ogg");
    ASSERT_FALSE(file.empty());
    const std::vector<std::uint8_t> expected =
        brr::encode(brr::toMono(detail::decodePcm(file, brr::kSampleRate)), /*loop=*/true);
    const std::vector<std::uint8_t> loaded = Access::voiceMachine(*sys, 0).read(
        MemoryRegion{.at = snes::audioRam(sd::kSamplesStart), .size = static_cast<std::uint32_t>(expected.size())});
    EXPECT_EQ(loaded, expected);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 0), 0x00u);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 1), 0x04u);
    EXPECT_EQ(dsp(*sys, 0x64), 0x00u);  // V6SRCN
}

TEST(SnesSampler, TheKeyedVoiceSoundsAndStopVoiceKeysItOff) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);

    sys->play(loopFile(), Cue{.voice = 1});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);  // a looping sample keeps sounding

    sys->stop(1);
    stepAndDrain(*sys, sink);  // the key-off, and the release that follows it
    EXPECT_EQ(nonSilentCount(stepAndDrain(*sys, sink)), 0u);
    EXPECT_EQ(Access::voiceCount(*sys), 1u);  // the sampler stays
}

TEST(SnesSampler, PlayWithoutACueIsVoiceZeroAsRecorded) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(loopFile());
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_EQ(dsp(*sys, 0x04), 0x00u);  // V0SRCN
    EXPECT_EQ(dsp(*sys, 0x02), 0x00u);  // V0PITCHL
    EXPECT_EQ(dsp(*sys, 0x03), 0x10u);  // V0PITCHH
    EXPECT_EQ(dsp(*sys, 0x00), 0x7Fu);  // V0VOLL
    EXPECT_EQ(dsp(*sys, 0x01), 0x7Fu);  // V0VOLR
}

TEST(SnesSampler, TheEffectReachesTheVoicesRegisters) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(loopFile(), Cue{.voice = 5, .effect = AudioEffect{.pitch = 2.0f, .volume = 0.5f, .pan = -1.0f}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x52), 0x00u);  // V5PITCHL: an octave up is $2000
    EXPECT_EQ(dsp(*sys, 0x53), 0x20u);  // V5PITCHH
    EXPECT_EQ(dsp(*sys, 0x50), 64u);    // V5VOLL: half
    EXPECT_EQ(dsp(*sys, 0x51), 0u);     // V5VOLR: the far side, silent at the edge

    // The middle is full on both sides; panning right takes from the left.
    sys->play(loopFile(), Cue{.voice = 6, .effect = AudioEffect{.pan = 0.5f}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x60), 64u);    // V6VOLL
    EXPECT_EQ(dsp(*sys, 0x61), 0x7Fu);  // V6VOLR
}

// One frame of the sampler's machine, and the chip's KOF register after it: a voice's bit is set by the
// key-off that ends a pass, and cleared by a key-on.
std::uint8_t keyedOffAfterAFrame(AudioSystem& sys) {
    Access::stepVoice(sys, 0);
    return dsp(sys, 0x5C);
}

TEST(SnesSampler, OncePlaysOnePassAndKeysTheVoiceOff) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    // The 20 ms tone, round from its start: continuous, it keeps going; once, it ends after its pass.
    sys->play(toneFile(), Cue{.voice = 0, .mode = PlayMode::continuous()});
    sys->play(toneFile(), Cue{.voice = 1, .mode = PlayMode::once()});
    Access::step(*sys);  // the key-ons
    std::uint8_t kof = 0;
    for (int frame = 0; frame < 6; ++frame) {
        kof = keyedOffAfterAFrame(*sys);
    }
    EXPECT_EQ(kof & 0x01, 0x00);  // voice 0 plays on
    EXPECT_EQ(kof & 0x02, 0x02);  // voice 1 was keyed off at the end of its pass
    EXPECT_EQ(workRam(*sys, sd::voiceBlock(1) + 6), 0u) << "the once is over";
}

TEST(SnesSampler, RepeatStrikesTheVoiceAgainAtItsTempo) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    // 601 a minute is one strike every six frames of the console's 60.0988 Hz; the tone's pass is two.
    sys->play(toneFile(), Cue{.voice = 2, .mode = PlayMode::repeat(601.0f)});
    Access::step(*sys);  // the key-on
    std::vector<std::uint8_t> kofs;
    for (int frame = 0; frame < 20; ++frame) {
        kofs.push_back(static_cast<std::uint8_t>(keyedOffAfterAFrame(*sys) & 0x04));
    }
    // Keyed off once each pass ends, and struck again — the key-off cleared — every six frames.
    std::vector<int> strikes;
    for (std::size_t i = 1; i < kofs.size(); ++i) {
        if (kofs[i - 1] == 0x04 && kofs[i] == 0x00) {
            strikes.push_back(static_cast<int>(i));
        }
    }
    ASSERT_GE(strikes.size(), 3u);
    EXPECT_EQ(strikes[1] - strikes[0], 6);
    EXPECT_EQ(strikes[2] - strikes[1], 6);
    EXPECT_NE(std::count(kofs.begin(), kofs.end(), 0x04), 0);  // and it is off between strikes

    // stop(voice) ends the repeat as well as the sound.
    sys->stop(2);
    Access::step(*sys);
    for (int frame = 0; frame < 14; ++frame) {
        EXPECT_EQ(keyedOffAfterAFrame(*sys) & 0x04, 0x04);
    }
    EXPECT_THROW(sys->play(toneFile(), Cue{.voice = 2, .mode = PlayMode::repeat(0.0f)}), std::invalid_argument);
}

TEST(SnesSampler, EffectChangesAVoiceAsItPlaysWithNoNewKeyOn) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(loopFile(), Cue{.voice = 4});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x43), 0x10u);  // V4PITCHH, as recorded

    // The pitch, volume and pan follow, and the voice plays on.
    sys->effect(4, AudioEffect{.pitch = 0.5f, .volume = 0.5f, .pan = 1.0f});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_EQ(dsp(*sys, 0x43), 0x08u);  // V4PITCHH: an octave down
    EXPECT_EQ(dsp(*sys, 0x40), 0u);     // V4VOLL: panned to the right edge
    EXPECT_EQ(dsp(*sys, 0x41), 64u);    // V4VOLR
    EXPECT_EQ(dsp(*sys, 0x44), 0x00u);  // V4SRCN: the same sample

    // The echo path follows too.
    sys->effect(4, AudioEffect{.echo = Echo{.delay = 0.064f, .feedback = 0.0f, .level = 1.0f}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x4D), 0x10u);  // EON: voice 4
    EXPECT_EQ(dsp(*sys, 0x7D), 4u);     // EDL: 64 ms
    EXPECT_EQ(dsp(*sys, 0x2C), 127u);   // EVOLL
    sys->effect(4, AudioEffect{});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x4D), 0x00u);

    // stop() mutes the echo path's output along with the voices; the next echo cue sets its level again.
    sys->effect(4, AudioEffect{.echo = Echo{.delay = 0.064f, .feedback = 0.0f, .level = 1.0f}});
    sys->stop();
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x2C), 0u);
    EXPECT_EQ(dsp(*sys, 0x3C), 0u);
    sys->play(loopFile(), Cue{.voice = 4, .effect = AudioEffect{.echo = Echo{.delay = 0.064f, .feedback = 0.0f, .level = 0.5f}}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x2C), 64u);

    // Before anything is cued there is no voice to change, and the call is taken quietly.
    test::CaptureAudioSink other;
    auto                   fresh = snesSystem(other);
    EXPECT_NO_THROW(fresh->effect(0, AudioEffect{.pitch = 2.0f}));
    EXPECT_EQ(Access::voiceCount(*fresh), 0u);
    EXPECT_THROW(fresh->effect(8, AudioEffect{}), std::out_of_range);
}

TEST(SnesSampler, TwoVoicesCuedInOneTickBothReachTheChip) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(loopFile(), Cue{.voice = 0});
    sys->play(toneFile(), Cue{.voice = 1, .effect = AudioEffect{.pitch = 0.5f}});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_EQ(dsp(*sys, 0x04), 0x00u);  // V0SRCN: the loop
    EXPECT_EQ(dsp(*sys, 0x14), 0x01u);  // V1SRCN: the tone
    EXPECT_EQ(dsp(*sys, 0x13), 0x08u);  // V1PITCHH: an octave down
}

TEST(SnesSampler, EchoPlacesItsBufferAndReverbIsTheSamePathDarkened) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);

    sys->play(loopFile(), Cue{.voice = 2, .effect = AudioEffect{.echo = Echo{.delay = 0.128f, .feedback = 0.25f, .level = 0.5f}}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x7D), 8u);     // EDL: 128 ms is eight 16 ms steps
    EXPECT_EQ(dsp(*sys, 0x6D), 0xBFu);  // ESA: the 16 KB buffer ends at the boot window
    EXPECT_EQ(dsp(*sys, 0x0D), 32u);    // EFB
    EXPECT_EQ(dsp(*sys, 0x2C), 64u);    // EVOLL
    EXPECT_EQ(dsp(*sys, 0x3C), 64u);    // EVOLR
    EXPECT_EQ(dsp(*sys, 0x0F), 0x7Fu);  // C0: the sound passed straight
    EXPECT_EQ(dsp(*sys, 0x1F), 0x00u);  // C1
    EXPECT_EQ(dsp(*sys, 0x6C), 0x00u);  // FLG: echo writes on
    EXPECT_EQ(dsp(*sys, 0x4D), 0x04u);  // EON: voice 2 feeds the path
    EXPECT_EQ(audioRam(*sys, 0xBF00), 0x00u);  // the buffer, cleared

    // A cue without an effect leaves its own voice out of the path and the others in.
    sys->play(loopFile(), Cue{.voice = 3});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x4D), 0x04u);

    // A reverb: the same path, shorter, fed back, through the darkening filter.
    sys->play(loopFile(), Cue{.voice = 3, .effect = AudioEffect{.reverb = Reverb{.decay = 1.0f, .level = 0.25f}}});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x7D), 2u);     // EDL
    EXPECT_EQ(dsp(*sys, 0x0D), 100u);   // EFB at full decay
    EXPECT_EQ(dsp(*sys, 0x2C), 32u);    // EVOLL
    EXPECT_EQ(dsp(*sys, 0x0F), 0x3Au);  // C0
    EXPECT_EQ(dsp(*sys, 0x1F), 0x20u);  // C1
    EXPECT_EQ(dsp(*sys, 0x4D), 0x0Cu);  // EON: voices 2 and 3

    // A cue on voice 2 with no effect takes it out of the path.
    sys->play(loopFile(), Cue{.voice = 2});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x4D), 0x08u);

    // The buffer was placed for 128 ms: a longer echo is refused; echo and reverb together, and a voice
    // the chip does not have, are refused at the call.
    EXPECT_THROW(sys->play(loopFile(), Cue{.voice = 4, .effect = AudioEffect{.echo = Echo{.delay = 0.24f}}}),
                 std::invalid_argument);
    EXPECT_THROW(sys->play(loopFile(), Cue{.voice = 4, .effect = AudioEffect{.echo = Echo{}, .reverb = Reverb{}}}),
                 std::invalid_argument);
    EXPECT_THROW(sys->play(loopFile(), Cue{.voice = 8}), std::out_of_range);
    EXPECT_THROW(sys->stop(8), std::out_of_range);
}

TEST(SnesSampler, StopKeysEveryVoiceOffAndKeepsTheSampler) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    sys->play(loopFile(), Cue{.voice = 0});
    sys->play(loopFile(), Cue{.voice = 7});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);

    sys->stop();
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, 0x5C), 0xFFu);  // KOF: every voice held off
    EXPECT_EQ(nonSilentCount(stepAndDrain(*sys, sink)), 0u);
    EXPECT_EQ(Access::voiceCount(*sys), 1u);
    EXPECT_TRUE(sys->isPlaying());

    // The samples stay loaded: a cue after stop() sounds again, with nothing loaded anew.
    sys->play(loopFile(), Cue{.voice = 0});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_EQ(dsp(*sys, 0x04), 0x00u);
}

TEST(SnesSampler, ASystemWhoseConsoleHasNoSamplePlayerRefusesACue) {
    test::CaptureAudioSink sink;
    auto gb = Access::makeManual(AudioKind::Chiptune, sink);
    EXPECT_THROW(gb->play(loopFile(), Cue{}), std::logic_error);
    EXPECT_THROW(gb->effect(0, AudioEffect{}), std::logic_error);
    EXPECT_THROW(gb->stop(0), std::logic_error);
    auto pcm = Access::makeManual(AudioKind::Pcm, sink, VMPlatform::Snes, TimingProfile::Snes);
    EXPECT_THROW(pcm->play(loopFile(), Cue{}), std::logic_error);
    auto unit = Access::makeManual(AudioKind::HostDriven, sink, VMPlatform::Snes, TimingProfile::Snes);
    EXPECT_THROW(unit->play(loopFile(), Cue{}), std::logic_error);

    // And a chiptune is not an audio file: a Cue cannot play one.
    auto sys = snesSystem(sink);
    const AudioId tune = AudioLibrary::instance().uploadAudio(std::vector<std::uint8_t>{0x60}, AudioType::Music,
                                                              Isa::Wdc65816);
    EXPECT_THROW(sys->play(tune, Cue{}), std::runtime_error);
}

TEST(SnesSampler, ABrrFileStreamsOnAPcmSystem) {
    test::CaptureAudioSink sink;
    auto                   pcm = Access::makeManual(AudioKind::Pcm, sink);
    pcm->play(loopFile());
    const std::vector<AudioFrame> frames = stepAndDrain(*pcm, sink);
    // One block decodes to sixteen samples at 32 kHz — 24 frames at 48 kHz, twelve up and twelve down —
    // and the stream's release tail follows them, decaying from the last one.
    ASSERT_GE(frames.size(), 24u);
    EXPECT_EQ(frames.front().left, frames.front().right);
    EXPECT_GT(frames[0].left, 0);
    EXPECT_GT(frames[11].left, 0);
    EXPECT_LT(frames[12].left, 0);
    EXPECT_LT(frames[23].left, 0);
    EXPECT_EQ(frames[0].left, -frames[12].left);  // the square's two levels, +7 and -7 at shift 12
}

TEST(SnesSampler, TheDirectoryHoldsSixtyFourSamples) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSystem(sink);
    for (int i = 0; i < 64; ++i) {  // sixty-four registrations of the one file: sixty-four samples
        sys->play(AudioLibrary::instance().registerAudio("tests/fixtures/loop.brr", AudioType::Sfx, AssetPolicy::Embed),
                  Cue{.voice = 0});
    }
    stepAndDrain(*sys, sink);
    EXPECT_EQ(audioRam(*sys, sd::kDirectory + 63 * 4), static_cast<std::uint8_t>((sd::kSamplesStart + 63 * 9) & 0xFF));
    // A manual system applies the cue at the call, so the refusal lands there.
    EXPECT_THROW(sys->play(AudioLibrary::instance().registerAudio("tests/fixtures/loop.brr", AudioType::Sfx,
                                                                  AssetPolicy::Embed),
                           Cue{.voice = 0}),
                 std::length_error);
}

// ── The sample format ───────────────────────────────────────────────────────────────────────────

TEST(SnesSamplerBrr, TheEncoderRoundTripsWithinTheFormatsError) {
    // A 440 Hz sine, a tenth of a second at the chip's rate, at a third of full scale.
    std::vector<std::int16_t> sine(3200);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<std::int16_t>(
            std::lround(10000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / brr::kSampleRate)));
    }
    const std::vector<std::uint8_t> blocks = brr::encode(sine, /*loop=*/false);
    EXPECT_EQ(blocks.size(), 200u * brr::kBlockBytes);
    EXPECT_EQ(blocks[199 * brr::kBlockBytes] & 0x03, 0x01);  // END on the last block, no LOOP
    EXPECT_EQ(blocks[0] & 0x0C, 0x00);                        // the first block predicts from nothing

    const std::vector<std::int16_t> back = brr::decode(blocks);
    ASSERT_EQ(back.size(), sine.size());
    double sumSq = 0;
    int    worst = 0;
    for (std::size_t i = 0; i < sine.size(); ++i) {
        const int e = std::abs(back[i] - sine[i]);
        worst       = std::max(worst, e);
        sumSq += static_cast<double>(e) * e;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(sine.size()));
    EXPECT_LT(rms, 100.0);  // one percent of the amplitude
    EXPECT_LT(worst, 1000);  // no single sample a tenth of it off
}

TEST(SnesSamplerBrr, TheLastBlockCarriesTheFlagsAndIsPaddedWithSilence) {
    const std::vector<std::int16_t> twenty(20, 8000);  // one full block and four samples
    const std::vector<std::uint8_t> once = brr::encode(twenty, /*loop=*/false);
    const std::vector<std::uint8_t> loop = brr::encode(twenty, /*loop=*/true);
    EXPECT_EQ(once.size(), 2u * brr::kBlockBytes);
    EXPECT_EQ(once[0] & 0x03, 0x00);
    EXPECT_EQ(once[brr::kBlockBytes] & 0x03, 0x01);
    EXPECT_EQ(loop[brr::kBlockBytes] & 0x03, 0x03);
    const std::vector<std::int16_t> back = brr::decode(once);
    ASSERT_EQ(back.size(), 32u);
    EXPECT_NEAR(back[19], 8000, 300);
    EXPECT_NEAR(back[31], 0, 300);  // the padding decays to silence

    EXPECT_EQ(brr::encode({}, false).size(), brr::kBlockBytes);  // nothing encodes to one silent END block
    EXPECT_THROW((void)brr::decode(std::vector<std::uint8_t>(10, 0)), std::invalid_argument);
    EXPECT_THROW(brr::requireWellFormed(std::vector<std::uint8_t>(9, 0)), std::invalid_argument);  // no END
    EXPECT_NO_THROW(brr::requireWellFormed(once));
}

}  // namespace
}  // namespace retropp
