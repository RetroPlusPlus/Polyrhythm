// The SNES sampler never refuses from its production path: a voice always plays. A whole sample that does
// not fit is delivered through its voice's window; a sample no voice is playing gives way so a new one can
// be placed; the directory's 256 entries recycle; the echo path is set once for the system and its ring
// grows into the cache, waiting on a sample a voice is playing in its way. The one value that would crash
// the chip — a voice past the eight — is a compile-time literal, pinned by the compile-outcome cases
// (tests/compile_fail/). Device-free unless a case says threaded: each manual system is stepped on this
// thread, so what a cue left in audio RAM and the chip's registers is read straight from the sampler.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mock_platform.h"  // test::CaptureAudioSink
#include "retropp/asset_policy.h"
#include "retropp/audio_effect.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/memory_region.h"
#include "retropp/snes.h"
#include "retropp/timing.h"
#include "retropp/vm.h"
#include "src/audio/audio_system_testing.h"
#include "src/audio/snes/default_driver.h"

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;
namespace sd = audio::snesdriver;

// The S-DSP registers these cases read.
constexpr std::uint8_t kEon = 0x4D, kEdl = 0x7D, kEsa = 0x6D, kEvolL = 0x2C, kEfb = 0x0D, kFlg = 0x6C;
// A voice's registers, by its base (voice × 0x10): volume, pitch, source number.
constexpr std::uint8_t kVolL = 0x00, kPitchL = 0x02, kPitchH = 0x03, kSrcn = 0x04;

std::unique_ptr<AudioSystem::SNES> snesSys(test::CaptureAudioSink& sink) {
    return Access::makeManualSnes(AudioKind::Chiptune, sink);
}

// Each call mints its own AudioId, so distinct cues of the same file are distinct samples.
AudioId loopFile() {  // a one-block square, 9 bytes
    return AudioLibrary::instance().registerAudio("tests/fixtures/loop.brr", AudioType::Sfx, AssetPolicy::Embed);
}
AudioId midFile() {  // ~1 s — a cache-sized whole sample
    return AudioLibrary::instance().registerAudio("tests/fixtures/mid.wav", AudioType::Sfx, AssetPolicy::Embed);
}
AudioId longFile() {  // larger than all of audio RAM — always streamed
    return AudioLibrary::instance().registerAudio("tests/fixtures/long.wav", AudioType::Sfx, AssetPolicy::Embed);
}
AudioId toneFile() {  // a 20 ms tone, dozens of blocks — a once pass that lasts several frames
    return AudioLibrary::instance().registerAudio("tests/fixtures/tone.wav", AudioType::Sfx, AssetPolicy::Embed);
}

std::vector<AudioFrame> stepAndDrain(AudioSystem& sys, test::CaptureAudioSink& sink) {
    Access::step(sys);
    return sink.drain(sys.audioStats().framesBuffered);
}

std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    return static_cast<std::size_t>(std::count_if(
        frames.begin(), frames.end(), [](const AudioFrame& f) { return f.left != 0 || f.right != 0; }));
}

std::uint8_t dsp(AudioSystem& sys, std::uint8_t reg) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = snes::dspRegister(reg), .size = 1}).front();
}
std::uint8_t voiceReg(AudioSystem& sys, std::uint8_t voice, std::uint8_t which) {
    return dsp(sys, static_cast<std::uint8_t>(voice * 0x10 + which));
}
std::vector<std::uint8_t> audioRamBytes(AudioSystem& sys, std::uint16_t at, std::uint32_t size) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = snes::audioRam(at), .size = size});
}
// A voice's play-mode byte in the driver's work RAM: non-zero while the driver plays it, zeroed when the
// driver keys a once pass off.
std::uint8_t modeByte(AudioSystem& sys, std::uint8_t voice) {
    return Access::voiceMachine(sys, 0).read(MemoryRegion{.at = sd::voiceBlock(voice) + 6, .size = 1}).front();
}

// ── N1: the default cue is bit-identical ──────────────────────────────────────────────────────────────
TEST(SnesNoRefusal, ADefaultCueKeysTheVoiceOnOutOfThePathAsBefore) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->play(loopFile());  // play(id): voice 0, continuous, no effect
    stepAndDrain(*sys, sink);

    EXPECT_EQ(dsp(*sys, kEon), 0u);
    EXPECT_EQ(dsp(*sys, kFlg), 0x20u);  // echo writes off — no path set
    EXPECT_EQ(dsp(*sys, kEdl), 0u);
    EXPECT_EQ(dsp(*sys, kEsa), 0xFFu);  // the idle ring
    EXPECT_EQ(voiceReg(*sys, 0, kSrcn), sd::kVoices);  // the cache's first entry, eight
    EXPECT_EQ(voiceReg(*sys, 0, kPitchL), 0x00u);      // $1000, as recorded
    EXPECT_EQ(voiceReg(*sys, 0, kPitchH), 0x10u);
    EXPECT_EQ(voiceReg(*sys, 0, kVolL), 0x7Fu);
    EXPECT_EQ(voiceReg(*sys, 0, kVolL + 1), 0x7Fu);

    // The silent sample a once pass ends on sits inside the program page.
    EXPECT_EQ(audioRamBytes(*sys, sd::kSilenceBlock, 9),
              (std::vector<std::uint8_t>{0x03, 0, 0, 0, 0, 0, 0, 0, 0}));
}

// ── N2: a whole sample that does not fit streams ────────────────────────────────────────────────────
TEST(SnesNoRefusal, AWholeSampleThatDoesNotFitStreams) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->echo(Echo{.delay = 0.24f});  // EDL 15 — the ring reaches down to eight bytes above the cache
    stepAndDrain(*sys, sink);
    ASSERT_EQ(dsp(*sys, kEdl), 15u);

    sys->play(midFile(), {.voice = 3});  // cannot fit — it is delivered through voice 3's window
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 100u);
    EXPECT_EQ(Access::streamedVoices(*sys), 0x08u);  // bit three
}

// ── N3: an evicted sample is placed again on its next cue ─────────────────────────────────────────────
TEST(SnesNoRefusal, AnEvictedSampleIsLoadedAgainOnItsNextCue) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);

    // Fill every sample entry with distinct, unheld samples: each cue on voice 0 releases the last.
    std::vector<AudioId> ids;
    for (unsigned i = 0; i < static_cast<unsigned>(sd::kDirectorySlots - sd::kVoices); ++i) {
        ids.push_back(loopFile());
        sys->play(ids.back(), {.voice = 0});
    }
    const AudioId first = ids.front();
    ASSERT_TRUE(Access::placementOf(*sys, first).has_value());

    // A new distinct sample frees an entry by dropping the stalest — the first cued.
    sys->play(loopFile(), {.voice = 1});
    EXPECT_FALSE(Access::placementOf(*sys, first).has_value());

    // Cued again it is placed again, from the kept encoding, and sounds.
    sys->play(first, {.voice = 2});
    EXPECT_GT(nonSilentCount(stepAndDrain(*sys, sink)), 50u);
    EXPECT_TRUE(Access::placementOf(*sys, first).has_value());
}

// ── N4: a held sample survives eviction; a once pass releases its hold ───────────────────────────────
TEST(SnesNoRefusal, AHeldSampleSurvivesEvictionAndAOncePassReleasesIt) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);

    const AudioId a = loopFile();
    sys->play(a, {.voice = 0});  // continuous — held on voice 0
    ASSERT_TRUE(Access::placementOf(*sys, a).has_value());
    const std::uint16_t aPlace = *Access::placementOf(*sys, a);

    // Fill and churn the cache around it; the held sample never moves or is dropped.
    for (unsigned i = 0; i < static_cast<unsigned>(sd::kDirectorySlots); ++i) {
        sys->play(loopFile(), {.voice = 1});
    }
    EXPECT_EQ(Access::placementOf(*sys, a), aPlace);
    EXPECT_NE(Access::heldVoices(*sys) & 0x01u, 0u);  // voice 0 still holds it
    sys->stop(AudioSystem::SNES::Voice{0});
    EXPECT_EQ(Access::heldVoices(*sys) & 0x01u, 0u);  // stop releases the hold

    // A once pass on voice 2 holds exactly while the driver is playing it: the hold tracks the driver's
    // own mode byte, frame for frame — set while the byte is non-zero, cleared when it reads 0. A whole
    // sample loaded by raising the threshold gives the pass many frames; stepping one machine frame at a
    // time (not the refill-to-target pass) observes the mode byte as it changes.
    Access::streamThreshold(*sys, SIZE_MAX);
    const AudioId b = midFile();
    sys->play(b, {.voice = 2, .mode = PlayMode::once()});
    EXPECT_NE(Access::heldVoices(*sys) & 0x04u, 0u);  // held right after the cue, not released early
    bool sawPlaying = false;
    bool released   = false;
    for (int f = 0; f < 1200 && !released; ++f) {
        Access::stepVoice(*sys, 0);  // one machine frame of the sampler
        const bool playing = modeByte(*sys, 2) != 0u;
        const bool held    = (Access::heldVoices(*sys) & 0x04u) != 0u;
        if (playing) {
            sawPlaying = true;
            EXPECT_TRUE(held) << "held while the driver is still playing the pass, frame " << f;
        } else if (sawPlaying) {
            EXPECT_FALSE(held) << "released when the driver keyed the once pass off, frame " << f;
            released = true;
        }
    }
    EXPECT_TRUE(sawPlaying) << "the once pass played for several frames";
    EXPECT_TRUE(released) << "the once pass ended and freed its hold";
}

// ── N5: a send before the path is set lands on the chip ──────────────────────────────────────────────
TEST(SnesNoRefusal, ASendBeforeThePathIsSetLandsOnTheChip) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->play(loopFile(), {.voice = 0, .effect = AudioEffect{.echo = true}});  // no echo() yet
    stepAndDrain(*sys, sink);
    EXPECT_NE(dsp(*sys, kEon) & 0x01u, 0u);  // the voice feeds the path
    EXPECT_EQ(dsp(*sys, kEdl), 0u);          // the parked ring
    EXPECT_EQ(dsp(*sys, kEsa), 0xFFu);
    EXPECT_EQ(dsp(*sys, kEvolL), 0u);        // silent until a path is set

    sys->echo(Echo{.delay = 0.064f, .feedback = 0.0f, .level = 1.0f});
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, kEdl), 4u);
    EXPECT_EQ(dsp(*sys, kEvolL), 127u);
    EXPECT_NE(dsp(*sys, kEon) & 0x01u, 0u);  // the send is still set
}

// ── N6: the ring grows and an unheld sample in its way gives way ─────────────────────────────────────
TEST(SnesNoRefusal, TheRingGrowsAndWhatIsInItsWayGoes) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    Access::streamThreshold(*sys, SIZE_MAX);  // a cache-sized file loads whole

    const AudioId low = loopFile();
    sys->play(low, {.voice = 1});    // held low, below where the ring will grow
    const AudioId big = midFile();
    sys->play(big, {.voice = 0});    // placed above `low`, reaching up into the ring's way
    sys->stop(AudioSystem::SNES::Voice{0});  // big is now unheld
    stepAndDrain(*sys, sink);
    ASSERT_TRUE(Access::placementOf(*sys, big).has_value());

    sys->echo(Echo{.delay = 0.128f});  // EDL 8 — base $BF00
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, kEsa), 0xBFu);
    EXPECT_EQ(dsp(*sys, kEdl), 8u);
    EXPECT_EQ(Access::cacheLimit(*sys), 0xBF00u);
    EXPECT_FALSE(Access::placementOf(*sys, big).has_value());  // the unheld one in the way gave way
    EXPECT_TRUE(Access::placementOf(*sys, low).has_value());   // held low, survives
}

// ── N7: the ring waits for a sample a voice is playing in its way ────────────────────────────────────
TEST(SnesNoRefusal, TheRingWaitsForAHeldSampleInItsWay) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    Access::streamThreshold(*sys, SIZE_MAX);

    const AudioId big = midFile();
    sys->play(big, {.voice = 0});  // continuous — held, reaching above the EDL-8 base
    stepAndDrain(*sys, sink);
    ASSERT_GT(*Access::placementOf(*sys, big) + 0x4000, 0xBF00);

    sys->echo(Echo{.delay = 0.128f});
    EXPECT_TRUE(Access::echoPending(*sys));       // the held sample is in the way
    EXPECT_EQ(dsp(*sys, kEdl), 0u);               // the chip ring has not moved
    EXPECT_NE(dsp(*sys, kEfb), 0u);               // the rest of the path is written
    EXPECT_EQ(Access::cacheLimit(*sys), 0xBF00u);  // the pending base

    sys->stop(AudioSystem::SNES::Voice{0});       // the voice releases
    EXPECT_FALSE(Access::echoPending(*sys));
    EXPECT_EQ(dsp(*sys, kEsa), 0xBFu);
    EXPECT_EQ(dsp(*sys, kEdl), 8u);
}

// ── N8: the ring shrinks in two steps ─────────────────────────────────────────────────────────────────
TEST(SnesNoRefusal, TheRingShrinksInTwoSteps) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->echo(Echo{.delay = 0.128f});  // EDL 8, base $BF00
    stepAndDrain(*sys, sink);
    ASSERT_EQ(dsp(*sys, kEsa), 0xBFu);

    sys->echo(Reverb{});  // EDL 2 — a shrink: the new size now, the base once the old ring has elapsed
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, kEdl), 2u);
    EXPECT_EQ(dsp(*sys, kEsa), 0xBFu);             // the base has not moved yet
    EXPECT_EQ(Access::cacheLimit(*sys), 0xBF00u);

    for (int i = 0; i < 12; ++i) {
        stepAndDrain(*sys, sink);
    }
    EXPECT_EQ(dsp(*sys, kEsa), 0xEFu);             // the shrunk ring's base, 0xFF00 - 2048*2
    EXPECT_EQ(Access::cacheLimit(*sys), 0xEF00u);
}

// ── N9: a grow during a shrink takes over ─────────────────────────────────────────────────────────────
TEST(SnesNoRefusal, AGrowDuringAShrinkTakesOver) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->echo(Echo{.delay = 0.128f});  // EDL 8
    stepAndDrain(*sys, sink);
    sys->echo(Reverb{});               // EDL 2 — shrink pending
    stepAndDrain(*sys, sink);
    sys->echo(Echo{.delay = 0.192f});  // EDL 12 — a grow, before the shrink's base moved
    for (int i = 0; i < 20; ++i) {
        stepAndDrain(*sys, sink);
    }
    EXPECT_EQ(dsp(*sys, kEsa), 0x9Fu);  // 0xFF00 - 2048*12, written with EDL at once — no later move
    EXPECT_EQ(dsp(*sys, kEdl), 12u);
}

// ── N10: the voice verbs are the protocols and the SNES implements them ──────────────────────────────
template <class S>
concept CuesAFileOnAVoice = requires(S& s, AudioId id) { s.play(id, {.voice = 0}); };
template <class S>
concept ChangesAVoice = requires(S& s, const AudioEffect& e) { s.effect(0, e); };
template <class S>
concept StopsAVoice = requires(S& s) { s.stop(0); };
template <class S>
concept SetsTheEchoPath = requires(S& s) {
    s.echo(Echo{});
    s.echo(Reverb{});
};
template <class S>
concept CuesAndStops = requires(S& s, AudioId id) {
    s.play(id);
    s.stop();
};

static_assert(CuesAFileOnAVoice<AudioSystem::SNES>);
static_assert(!CuesAFileOnAVoice<AudioSystem::GBC>);
static_assert(!CuesAFileOnAVoice<AudioSystem>);
static_assert(ChangesAVoice<AudioSystem::SNES>);
static_assert(!ChangesAVoice<AudioSystem::GBC>);
static_assert(!ChangesAVoice<AudioSystem>);
static_assert(StopsAVoice<AudioSystem::SNES>);
static_assert(!StopsAVoice<AudioSystem::GBC>);
static_assert(!StopsAVoice<AudioSystem>);
static_assert(SetsTheEchoPath<AudioSystem::SNES>);
static_assert(!SetsTheEchoPath<AudioSystem::GBC>);
static_assert(!SetsTheEchoPath<AudioSystem>);
static_assert(CuesAndStops<AudioSystem::SNES>);
static_assert(CuesAndStops<AudioSystem::GBC>);
static_assert(CuesAndStops<AudioSystem>);

static_assert(std::is_base_of_v<VoiceSystem<8>, AudioSystem::SNES>);
static_assert(std::is_base_of_v<EffectSystem, AudioSystem::SNES>);
static_assert(!std::is_base_of_v<EffectSystem, AudioSystem::GBC>);
static_assert(std::is_same_v<decltype(AudioEffect::echo), bool>);
static_assert(std::is_same_v<decltype(AudioEffect::reverb), bool>);
static_assert(std::is_trivially_copyable_v<AudioSystem::SNES::Cue>);
static_assert(AudioSystem::SNES::kVoices == 8);
static_assert(AudioSystem::SNES::voices.size() == 8);
static_assert(AudioSystem::SNES::voices[5].index() == 5);
static_assert(AudioSystem::SNES::voices[7].index() == 7);

TEST(SnesNoRefusal, EchoAndReverbTogetherKeyOnWithNoThrow) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);
    sys->echo(Echo{.delay = 0.064f, .level = 1.0f});
    ASSERT_NO_THROW(sys->play(loopFile(), {.voice = 0, .effect = AudioEffect{.echo = true, .reverb = true}}));
    stepAndDrain(*sys, sink);
    EXPECT_NE(dsp(*sys, kEon) & 0x01u, 0u);  // the voice feeds the path

    ASSERT_NO_THROW(sys->play(loopFile(), {.voice = 1, .effect = AudioEffect{.echo = false, .reverb = false}}));
    stepAndDrain(*sys, sink);
    EXPECT_EQ(dsp(*sys, kEon) & 0x02u, 0u);  // voice 1 does not
}

// ── N11: a file that cannot be read or decoded is refused at the call ────────────────────────────────
TEST(SnesNoRefusal, AFileThatCannotBeReadOrDecodedIsRefusedAtTheCall) {
    test::CaptureAudioSink sink;
    auto                   sys = snesSys(sink);

    // A ten-byte `.brr` is not a whole number of blocks — the encode refuses at the call.
    const AudioId bad =
        AudioLibrary::instance().registerAudio("tests/fixtures/bad.brr", AudioType::Sfx, AssetPolicy::Embed);
    EXPECT_THROW(sys->play(bad, {.voice = 0}), std::exception);
    stepAndDrain(*sys, sink);
    EXPECT_EQ(Access::voiceCount(*sys), 0u);  // the throw preceded any cue — nothing was hosted

    // A path that does not exist cannot be read — the read refuses at the call.
    const AudioId missing =
        AudioLibrary::instance().registerAudio("tests/fixtures/does-not-exist.wav", AudioType::Sfx,
                                               AssetPolicy::LoadFromPath);
    EXPECT_THROW(sys->play(missing, {.voice = 0}), std::exception);
}

// ── N12: a threaded system survives every former refusal ─────────────────────────────────────────────
TEST(SnesNoRefusal, AThreadedSystemSurvivesEveryFormerRefusal) {
    // Register every sample up front, as a game does — the library is the program-wide catalog, not
    // something cued verbs touch; the cues below churn the cache and directory with 300 distinct samples.
    std::vector<AudioId> churn;
    churn.reserve(300);
    for (unsigned i = 0; i < 300; ++i) {
        churn.push_back(loopFile());
    }
    const AudioId square = loopFile();
    const AudioId both   = loopFile();
    const AudioId longId = longFile();

    test::CaptureAudioSink sink;
    AudioSystem::SNES      snes{AudioKind::Chiptune, sink};  // a real production thread

    snes.play(square, {.voice = 2});
    snes.echo(Reverb{});                                       // the reverb's EDL 2
    snes.effect(2, AudioEffect{.echo = true});
    snes.echo(Echo{.delay = 0.128f});                          // then the echo's EDL 8 — the §AE repro
    for (AudioId id : churn) {
        snes.play(id, {.voice = 0});
    }
    snes.play(longId, {.voice = 1});                           // larger than all of audio RAM — it streams
    snes.play(both, {.voice = 3, .effect = AudioEffect{.echo = true, .reverb = true}});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // let the production thread work through it
    EXPECT_TRUE(snes.isPlaying());
    snes.stop();
    // The system is destroyed here: on main the second echo aborts the production thread; here it does not,
    // and the destructor joins cleanly. A process abort would fail the suite.
}

}  // namespace
}  // namespace retropp
