// The audio-production thread, device-free. Production was relocated off the main thread onto
// a dedicated, self-pacing per-AudioSystem thread; play()/stop() marshal over a lock-free SPSC cue queue;
// the produced PCM stream is byte-identical to the pre-D.1 main-thread producer. This file proves all four:
//   1. the cue queue preserves command order (the marshaling channel),
//   2. frame-quantized stepping is byte-identical to sub-frame chunking over the same total cycles (the
//      golden gate — drives the synchronous seam so it is insensitive to wall-clock scheduling),
//   3. a real threaded system cues, produces autonomously, and stops/drains,
//   4. a finished one-shot SFX auto-closes ON the production thread, while Music never does.
#include "retropp/audio_system.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio.h"
#include "retropp/audio_library.h"   // AudioLibrary, AudioType, AudioId
#include "retropp/gb_audio.h"        // sameboy::diagnosticTone
#include "retropp/isa.h"             // Isa::Sm83
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — synchronous production seam
#include "src/audio/cue_queue.h"             // audio::CueQueue / AudioCommand
#include "mock_platform.h"                    // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;
using namespace std::chrono_literals;

// Poll a predicate until true or the timeout elapses; used to observe the autonomous production thread.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::size_t nonSilentCount(const std::vector<AudioFrame>& frames) {
    std::size_t n = 0;
    for (const AudioFrame& f : frames) {
        if (f.left != 0 || f.right != 0) {
            ++n;
        }
    }
    return n;
}

// ── 1. The cue queue (pure) ──────────────────────────────────────────────────────────────────────
// The main→production channel preserves command order; a drained queue reports empty. The full-queue
// drop behavior is the SpscRingBuffer's existing contract (covered by ring_buffer_test) — not re-tested.
TEST(AudioProductionThread, CueQueuePreservesOrder) {
    using Op = audio::AudioCommand::Op;
    audio::CueQueue q{16};
    EXPECT_TRUE(q.push(audio::AudioCommand{Op::Play, AudioId{7}}));
    EXPECT_TRUE(q.push(audio::AudioCommand{Op::Stop, AudioId{}}));
    EXPECT_TRUE(q.push(audio::AudioCommand{Op::Play, AudioId{3}}));

    std::array<audio::AudioCommand, 1> one;
    ASSERT_EQ(q.pop(std::span<audio::AudioCommand>(one)), 1u);
    EXPECT_EQ(one[0].op, Op::Play);
    EXPECT_EQ(one[0].id, AudioId{7});
    ASSERT_EQ(q.pop(std::span<audio::AudioCommand>(one)), 1u);
    EXPECT_EQ(one[0].op, Op::Stop);
    ASSERT_EQ(q.pop(std::span<audio::AudioCommand>(one)), 1u);
    EXPECT_EQ(one[0].op, Op::Play);
    EXPECT_EQ(one[0].id, AudioId{3});
    EXPECT_EQ(q.pop(std::span<audio::AudioCommand>(one)), 0u);  // drained
}

// ── 2. The golden gate: frame-quantized == sub-frame, byte-for-byte ───────────────────────────────
// Drive the SAME driver over the SAME total CPU cycles at two step granularities through the synchronous
// seam (the thread not running, so scheduling can't perturb the capture); the produced PCM streams must
// be byte-identical. This is what licenses the D.1 re-quantization to "no output change".
std::vector<AudioFrame> produceExact(AudioSystem& sys, test::CaptureAudioSink& sink, std::uint64_t chunk,
                                     int steps) {
    std::vector<AudioFrame> out;
    for (int i = 0; i < steps; ++i) {
        Access::stepDriverRaw(sys, chunk);              // run exactly `chunk` cycles
        const std::vector<AudioFrame> got = sink.drain(1u << 20);  // collect what they produced
        out.insert(out.end(), got.begin(), got.end());
    }
    return out;
}

TEST(AudioProductionThread, FrameQuantizedStreamMatchesSubFrameChunking) {
    test::CaptureAudioSink sinkA;
    test::CaptureAudioSink sinkB;
    auto a = Access::makeManual(AudioKind::Chiptune, sinkA);
    auto b = Access::makeManual(AudioKind::Chiptune, sinkB);
    const AudioId tone = sameboy::diagnosticTone();
    a->play(tone);
    b->play(tone);

    constexpr std::uint64_t kFrameCycles = 70'224;  // the GB frame quantum
    // Same total cycles (16 × 70'224 == 128 × 8'778), two granularities — one whole-frame, one sub-frame.
    const std::vector<AudioFrame> frameQuantized = produceExact(*a, sinkA, kFrameCycles, 16);
    const std::vector<AudioFrame> subFrame       = produceExact(*b, sinkB, kFrameCycles / 8, 128);

    ASSERT_FALSE(frameQuantized.empty());
    ASSERT_GT(nonSilentCount(frameQuantized), std::size_t{100});  // a real waveform, not silence
    ASSERT_EQ(frameQuantized.size(), subFrame.size()) << "same total cycles → same frame count";
    for (std::size_t i = 0; i < frameQuantized.size(); ++i) {
        ASSERT_EQ(frameQuantized[i].left, subFrame[i].left) << "left mismatch at frame " << i;
        ASSERT_EQ(frameQuantized[i].right, subFrame[i].right) << "right mismatch at frame " << i;
    }
}

// ── 3. Threaded lifecycle / cueing (integration) ──────────────────────────────────────────────────
// A real production thread (ctor 1) applies a cue, fills the ring autonomously, and halts on stop().
TEST(AudioProductionThread, ThreadedPlayProducesThenStopDrains) {
    test::CaptureAudioSink sink;
    AudioSystem audio{AudioKind::Chiptune, sink};  // ctor 1 — production thread running, parked idle
    EXPECT_FALSE(audio.isPlaying());

    const AudioId tone = sameboy::diagnosticTone();
    audio.play(tone);
    EXPECT_TRUE(waitFor([&] { return audio.isPlaying(); }, 2000ms));
    EXPECT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered >= kAudioSampleRate / 40; }, 2000ms))
        << "the production thread did not fill the ring toward its target";

    audio.stop();
    EXPECT_TRUE(waitFor([&] { return !audio.isPlaying(); }, 2000ms));
    // With production stopped, draining empties the ring and it stays empty (no new pushes).
    sink.drain(1u << 20);
    std::this_thread::sleep_for(50ms);  // any in-flight produce settles
    sink.drain(1u << 20);
    EXPECT_EQ(audio.audioStats().framesBuffered, 0u);
}

// Construct + destruct with nothing playing: the thread starts, parks idle (untimed wait → no spin), and
// joins cleanly on teardown (sink stop → join → members). Reaching the end without a hang is the signal.
TEST(AudioProductionThread, IdleSystemStartsAndJoinsCleanly) {
    test::CaptureAudioSink sink;
    {
        AudioSystem audio{AudioKind::Chiptune, sink};
        EXPECT_FALSE(audio.isPlaying());
        std::this_thread::sleep_for(20ms);  // idle — produces nothing
        EXPECT_EQ(audio.audioStats().framesBuffered, 0u);
    }  // dtor joins the production thread
    SUCCEED();
}

// ── 4. Auto-close on the production thread ────────────────────────────────────────────────────────
void setTonesRoot() {
    setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones");
}

// A finished one-shot SFX auto-closes on the production thread once its output goes silent. The test acts
// as the consumer (drains continuously) so the thread keeps stepping the driver toward its silent tail.
TEST(AudioProductionThread, FinishedSfxAutoClosesOnThread) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{AudioKind::Chiptune, sink};
    const AudioId blip = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Sfx, Isa::Sm83,
                                                                AssetPolicy::LoadFromPath);
    audio.play(blip);
    ASSERT_TRUE(waitFor([&] { return audio.isPlaying(); }, 2000ms));

    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (audio.isPlaying() && std::chrono::steady_clock::now() < deadline) {
        sink.drain(1u << 20);            // consume → the thread keeps producing toward silence
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(audio.isPlaying()) << "a finished one-shot SFX did not auto-close on the production thread";
}

// Music registered from the same decaying source goes silent but must NEVER auto-close — the game owns a
// Music track's lifetime. The AudioType gate, on the production thread.
TEST(AudioProductionThread, SilentMusicDoesNotAutoCloseOnThread) {
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem audio{AudioKind::Chiptune, sink};
    const AudioId asMusic = AudioLibrary::instance().registerAudio("sfx_blip.asm", AudioType::Music,
                                                                   Isa::Sm83, AssetPolicy::LoadFromPath);
    audio.play(asMusic);
    ASSERT_TRUE(waitFor([&] { return audio.isPlaying(); }, 2000ms));

    const auto until = std::chrono::steady_clock::now() + 600ms;  // well past the decay + silence threshold
    while (std::chrono::steady_clock::now() < until) {
        sink.drain(1u << 20);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(audio.isPlaying()) << "Music must never auto-close, even when its output goes silent";
}

}  // namespace
}  // namespace retropp
