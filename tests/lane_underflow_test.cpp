// How the mix treats a machine that is behind, and how far ahead of the mix a machine may get.
//
// While the output holds more than its floor there is time to wait, so a pass takes only what every
// machine has ready and comes back for the rest. Once the output is down to that floor the pass
// advances by what the most-advanced voice produced; a voice with fewer contributes silence for the
// shortfall and that shortfall is counted against it, so a machine that falls behind mutes itself and
// nothing else, and its late frames still play — after a gap, intact.
//
// Ahead of the mix, a machine runs only until its lane and the output buffer together hold the latency
// target, so a machine on its own thread parks the output's own inventory and nothing further.
//
// Device-free where it can be: CaptureAudioSink + the internal synchronous seam, whose selective
// stepping advances one voice at a time. Stepping the voices unevenly is what produces a short lane
// deterministically, without waiting on a real machine to fall behind a real one. The pacing runs on a
// machine's own thread, so the case that pins it does too.
#include "retropp/audio_system.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "retropp/asset_policy.h"    // AssetPolicy::LoadFromPath
#include "retropp/asset_registry.h"  // setAssetRoot — the single LoadFromPath base for the literal names
#include "retropp/audio_library.h"   // AudioLibrary, AudioType, AudioId
#include "src/audio/audio_system_testing.h"  // detail::AudioSystemTestAccess — the synchronous seam
#include "mock_platform.h"                   // test::CaptureAudioSink

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// More frames than any pass here produces, so a drain always takes everything the ring holds.
constexpr std::size_t kDrainAll = 1u << 20;

void setTonesRoot() { setAssetRoot(std::filesystem::path(RETROPP_ASSETS_DIR) / "tones"); }

// Wait for something the machines' own threads bring about.
template <typename Predicate>
bool waitFor(Predicate done, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return done();
}

AudioId sustainedTone() {
    return AudioLibrary::instance().registerAudio("tone_sustain.asm", AudioType::Music, Isa::Sm83,
                                                  AssetPolicy::LoadFromPath);
}

// A second entry, for the cases that need two voices a cue can tell apart.
AudioId otherTone() {
    return AudioLibrary::instance().registerAudio("tone_c.asm", AudioType::Music, Isa::Sm83,
                                                  AssetPolicy::LoadFromPath);
}

// What one voice produces, step by step, on a system with nothing to substitute for — the reference a
// starved mix is measured against. Every machine is deterministic and starts from reset, so a second
// voice of the same tone produces this same sequence.
std::vector<std::vector<AudioFrame>> soloSteps(int steps) {
    setTonesRoot();
    test::CaptureAudioSink            sink;
    const std::unique_ptr<AudioSystem> audio = Access::makeManual(AudioKind::Chiptune, sink);
    audio->play(sustainedTone());
    std::vector<std::vector<AudioFrame>> out;
    for (int i = 0; i < steps; ++i) {
        Access::stepVoice(*audio, 0);
        Access::mix(*audio);
        out.push_back(sink.drain(kDrainAll));
    }
    return out;
}

class LaneUnderflow : public ::testing::Test {
protected:
    void SetUp() override {
        setTonesRoot();
        audio = Access::makeManual(AudioKind::Chiptune, sink);
    }

    // Two voices of the same tone, so each one's own audio is the reference for what the other's
    // silence must leave untouched.
    void playTwo() {
        const AudioId tone = sustainedTone();
        audio->play(tone);
        audio->play(tone);
    }

    std::vector<AudioFrame> mixAndDrain() {
        Access::mix(*audio);
        return sink.drain(kDrainAll);
    }

    // Put the output above the level where the mix stops waiting, out of both voices' own frames, and
    // leave the two lanes level and empty. Both voices have produced by the end of it, so what follows
    // measures how a straggler is treated rather than how a machine starting up is.
    void fillPastTheWaitingFloor() {
        for (int i = 0; i < 2; ++i) {
            Access::stepVoice(*audio, 0);
            Access::stepVoice(*audio, 1);
        }
        ASSERT_EQ(Access::laneFrames(*audio, 0), Access::laneFrames(*audio, 1))
            << "two machines of the same tone stepped the same number of times hold the same frames";
        Access::mix(*audio, Access::laneFrames(*audio, 0));
        ASSERT_GT(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));
    }

    // Run one voice a whole step further ahead than the other, and report what each holds.
    struct Lanes {
        std::size_t deep;
        std::size_t shallow;
    };
    Lanes unevenLanes() {
        Access::stepVoice(*audio, 0);
        Access::stepVoice(*audio, 0);
        Access::stepVoice(*audio, 1);
        return Lanes{.deep = Access::laneFrames(*audio, 0), .shallow = Access::laneFrames(*audio, 1)};
    }

    test::CaptureAudioSink       sink;
    std::unique_ptr<AudioSystem> audio;
};

// A voice that produced nothing this pass contributes silence, and what reaches the ring is the
// running voice's own audio, unchanged. The shortfall is counted against the voice that owed it.
TEST_F(LaneUnderflow, AStarvedLaneContributesSilenceAndTheMixAdvances) {
    const std::vector<std::vector<AudioFrame>> solo = soloSteps(2);
    ASSERT_FALSE(solo[0].empty());
    ASSERT_FALSE(solo[1].empty());

    playTwo();
    ASSERT_EQ(Access::voiceCount(*audio), 2u);

    // Both run once, so both have produced and what follows measures starvation rather than startup.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    ASSERT_EQ(mixAndDrain().size(), solo[0].size());

    // Now only the first one runs.
    Access::stepVoice(*audio, 0);
    const std::vector<AudioFrame> alone = mixAndDrain();

    ASSERT_EQ(alone.size(), solo[1].size());
    for (std::size_t i = 0; i < alone.size(); ++i) {
        EXPECT_EQ(alone[i].left, solo[1][i].left) << "at frame " << i;
        EXPECT_EQ(alone[i].right, solo[1][i].right) << "at frame " << i;
        if (HasFailure()) {
            break;  // one mismatch position is enough to diagnose
        }
    }
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), alone.size());
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// The starved voice's stream resumes exactly where it left off: the gap is inserted, nothing is
// dropped. Its next frames are the ones a voice that never fell behind would have produced.
TEST_F(LaneUnderflow, TheLateFramesPlayIntactAfterTheGap) {
    const std::vector<std::vector<AudioFrame>> solo = soloSteps(2);
    ASSERT_FALSE(solo[1].empty());

    playTwo();
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    ASSERT_FALSE(mixAndDrain().empty());

    Access::stepVoice(*audio, 0);  // the second voice misses this pass
    ASSERT_FALSE(mixAndDrain().empty());

    Access::stepVoice(*audio, 1);  // and catches up on the next one
    const std::vector<AudioFrame> late = mixAndDrain();

    ASSERT_EQ(late.size(), solo[1].size());
    for (std::size_t i = 0; i < late.size(); ++i) {
        EXPECT_EQ(late[i].left, solo[1][i].left) << "at frame " << i;
        EXPECT_EQ(late[i].right, solo[1][i].right) << "at frame " << i;
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), late.size());  // this pass, the first one owed
}

// When no voice has produced anything, the pass produces nothing. Silence is substituted for a voice
// that is behind the others, never for the whole mix being behind the ring's appetite — frames that
// are merely not produced yet are waited for, and the ring's own underflow covers the wait.
TEST_F(LaneUnderflow, EveryLaneEmptyProducesNothing) {
    playTwo();
    ASSERT_EQ(audio->audioStats().framesBuffered, 0u);

    Access::mix(*audio);

    EXPECT_EQ(audio->audioStats().framesBuffered, 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// A voice that has not produced its first frame is still building its machine. The count measures
// starvation, not startup, so it begins at that first frame.
TEST_F(LaneUnderflow, AVoiceThatHasNotProducedYetIsNotCounted) {
    playTwo();

    Access::stepVoice(*audio, 0);
    EXPECT_FALSE(mixAndDrain().empty());

    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// Running further ahead than a neighbor is not starving the neighbor. Machines on their own clocks
// run ahead in whole steps, so at any moment one of them holds a step more than another; the mix keeps
// the OUTPUT's pace, and every voice stocked past that pace delivers in full. Pacing off the deepest
// lane instead would read every shallower voice as starving and pad it with a step of silence — worse
// the more machines there are, until the mix is mostly silence.
TEST_F(LaneUnderflow, ADeeperLaneDoesNotStarveAShallowerOne) {
    playTwo();

    Access::stepVoice(*audio, 0);  // one voice a whole step further ahead than the other
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);

    // The output wants a fraction of a step; both voices hold at least that much.
    const std::size_t wanted = 200;
    Access::mix(*audio, wanted);

    EXPECT_EQ(sink.drain(kDrainAll).size(), wanted);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// With one voice there is nothing to be behind: the substitution is inert by construction, which is
// what keeps a lone sound's bytes exactly as its machine produced them.
TEST_F(LaneUnderflow, ALoneVoiceNeverSubstitutes) {
    audio->play(sustainedTone());

    for (int i = 0; i < 8; ++i) {
        Access::stepVoice(*audio, 0);
        ASSERT_FALSE(mixAndDrain().empty()) << "at pass " << i;
    }

    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// A machine a little behind costs nobody anything while the output still has frames in hand: the pass
// takes what the shorter lane holds, leaves the deeper voice's surplus parked as inventory, and counts
// no shortfall against anyone. A few milliseconds of scheduling is not starvation.
TEST_F(LaneUnderflow, AShortLaneIsWaitedForWhileTheOutputHoldsCushion) {
    playTwo();
    fillPastTheWaitingFloor();

    const Lanes       lanes  = unevenLanes();
    const std::size_t before = audio->audioStats().framesBuffered;
    ASSERT_GT(lanes.deep, lanes.shallow);

    Access::mix(*audio, lanes.deep);

    EXPECT_EQ(audio->audioStats().framesBuffered - before, lanes.shallow)
        << "the mix advanced past what the shorter lane held";
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// Once the output is down to its floor the waiting is over. The pass advances by what the deeper lane
// holds, the straggler is silent for the difference, and the difference is counted against it — the
// output is never left to run dry over a machine that has stalled for longer than it can cover.
TEST_F(LaneUnderflow, SubstitutionResumesWhenTheOutputRunsLow) {
    playTwo();
    fillPastTheWaitingFloor();

    const Lanes lanes = unevenLanes();
    ASSERT_GT(lanes.deep, lanes.shallow);

    sink.drain(kDrainAll);  // the device took everything; there is nothing left to wait with
    ASSERT_LE(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));

    Access::mix(*audio, lanes.deep);

    EXPECT_EQ(sink.drain(kDrainAll).size(), lanes.deep) << "the mix waited with a drained output";
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), lanes.deep - lanes.shallow);
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 0), 0u);
}

// A machine that has yet to produce its first frame is still building, not lagging, so it holds nothing
// back: the voices that are running deliver in full while it starts up, and nothing is counted against
// it. Waiting on it instead would stop the mix dead at zero for as long as a machine takes to place its
// content.
TEST_F(LaneUnderflow, AVoiceThatHasNotProducedDoesNotHoldTheMix) {
    playTwo();

    // One voice runs; the other has never been stepped. The output ends up with its cushion.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 0);
    Access::mix(*audio, Access::laneFrames(*audio, 0));
    ASSERT_GT(audio->audioStats().framesBuffered, Access::waitingFloor(*audio));

    Access::stepVoice(*audio, 0);
    const std::size_t ready  = Access::laneFrames(*audio, 0);
    const std::size_t before = audio->audioStats().framesBuffered;
    ASSERT_GT(ready, 0u);

    Access::mix(*audio, ready);

    EXPECT_EQ(audio->audioStats().framesBuffered - before, ready) << "a machine still starting up held the mix at zero";
    EXPECT_EQ(Access::laneUnderflowFrames(*audio, 1), 0u);
}

// A voice riding its release fade is finished rather than paced, so it does not hold the waiting mix
// either: its tail is a few hundred frames on their way out, and the voices still playing deliver at
// their own depth while it goes.
TEST_F(LaneUnderflow, AVoiceRidingItsFadeDoesNotHoldTheMix) {
    const AudioId going  = sustainedTone();
    const AudioId lasting = otherTone();
    audio->play(going);
    audio->play(lasting);
    ASSERT_EQ(Access::voiceCount(*audio), 2u);
    fillPastTheWaitingFloor();

    // The voice about to fade holds the shallower lane, so waiting for it would be visible.
    Access::stepVoice(*audio, 0);
    Access::stepVoice(*audio, 1);
    Access::stepVoice(*audio, 1);
    const std::size_t fading  = Access::laneFrames(*audio, 0);
    const std::size_t playing = Access::laneFrames(*audio, 1);
    ASSERT_GT(playing, fading);

    audio->play(going, CueMode::Retrigger);  // the first voice enters its fade; a fresh one starts
    ASSERT_EQ(Access::voiceCount(*audio), 3u);

    const std::size_t before = audio->audioStats().framesBuffered;
    Access::mix(*audio, playing);

    EXPECT_EQ(audio->audioStats().framesBuffered - before, playing) << "a fading voice held the mix to its lane";
}

// A machine may fill its lane with the whole latency target — an output that has taken nothing yet
// holds none of it, so all of it stands in this one lane — plus the step it was in the middle of. The
// lane holds that much, which is what lets a machine's APU push a frame without asking whether it
// landed.
TEST_F(LaneUnderflow, ALaneHoldsEverythingThePacingLetsAMachineProduce) {
    audio->play(sustainedTone());
    ASSERT_EQ(Access::voiceCount(*audio), 1u);

    EXPECT_GE(Access::laneCapacity(*audio, 0),
              Access::latencyTarget(*audio) + Access::framesPerStep(*audio));
}

// ── On real threads ───────────────────────────────────────────────────────────────────────────────
// Several machines running on their own threads keep the output fed: the sink pulls at the device's
// rate and the frames keep coming, for as long as it pulls.
//
// How MUCH silence the mix substitutes along the way is a property of the machine the test runs on —
// how promptly its scheduler gets each machine's thread onto a core — not of the engine, and a run on a
// busier host substitutes several times what a quiet one does. So this case asserts that audio flows
// and nothing else; the substitution rule itself is pinned by the cases above, where the lanes are
// filled by hand and the answer is exact.
TEST(LaneUnderflowThreaded, MachinesOnTheirOwnThreadsKeepTheOutputFed) {
    using namespace std::chrono_literals;
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem            audio{AudioKind::Chiptune, sink};  // a production thread, and a thread each

    const AudioId tone = sustainedTone();
    for (int i = 0; i < 4; ++i) {
        audio.play(tone);
    }
    ASSERT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered > 0; }, 2000ms))
        << "the machines produced nothing";

    std::size_t drained = 0;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(10ms);
        drained += sink.drain(400).size();
    }

    EXPECT_GT(drained, std::size_t{5000}) << "the output stopped being fed while the machines ran";
}

// A machine on its own thread runs ahead only until the frames waiting downstream of it — its lane plus
// the output buffer they are mixed into — come to the latency target, and parks there. So the standing
// inventory between a machine and the device is that target plus at most the one step the machine was
// already committed to, which is what the output holds when the produce pass steps the machines itself:
// threading a machine buys throughput and costs no latency.
//
// The bound carries half a step of tolerance, and both halves of that are deliberate. `framesPerStep` is
// the frames a step is WORTH — a truncated ratio of cycles to samples — while a step yields whatever its
// APU actually resampled, which runs a frame or so richer; and the pacing reads the buffer's occupancy
// across threads, where the answer is approximate by design. Pacing a machine against its own lane
// instead parks a standing STEP more than this, so the bound sits midway between the two and clears each
// by about half a step. Neither figure is the host's to decide: a busier host gets a machine onto a core
// later and leaves LESS parked, never more.
TEST(LaneUnderflowThreaded, AMachineRunsAheadOnlyToTheLatencyTarget) {
    using namespace std::chrono_literals;
    setTonesRoot();
    test::CaptureAudioSink sink;
    AudioSystem            audio{AudioKind::Chiptune, sink};  // a production thread, and one for the machine

    audio.play(sustainedTone());
    const std::size_t target = Access::latencyTarget(audio);
    const std::size_t step   = Access::framesPerStep(audio);
    ASSERT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered >= target; }, 4000ms))
        << "the output never reached its latency target";

    // Nothing drains the sink, so the output stays at its target and the mix has nothing left to ask
    // for: what the two reads below see is at rest. A machine that kept running would spend this time
    // stacking frames in its lane behind a full buffer.
    std::this_thread::sleep_for(200ms);

    const std::size_t buffered  = audio.audioStats().framesBuffered;
    const std::size_t inventory = buffered + Access::laneFrames(audio, 0);

    EXPECT_LE(inventory, target + step + step / 2)
        << "frames are standing between the machine and the output beyond the latency target ("
        << buffered << " buffered, " << inventory - buffered << " laned, a step is " << step << ")";
}

// Tearing a system down while several machines are mid-step. Closing a voice releases its hold on its
// runner before it waits for that runner's thread, so anything the thread reaches for during the wait
// must be something it holds itself rather than something the voice was holding for it. Reaching the
// end without a crash is the signal.
TEST(LaneUnderflowThreaded, ASystemWithManyMachinesTearsDownWhileTheyRun) {
    using namespace std::chrono_literals;
    setTonesRoot();
    for (int round = 0; round < 3; ++round) {
        test::CaptureAudioSink sink;
        AudioSystem            audio{AudioKind::Chiptune, sink};
        const AudioId          tone = sustainedTone();
        for (int i = 0; i < 6; ++i) {
            audio.play(tone);
        }
        ASSERT_TRUE(waitFor([&] { return audio.audioStats().framesBuffered > 0; }, 2000ms))
            << "the machines produced nothing in round " << round;
        sink.drain(kDrainAll);
        // and out of scope, with every machine still running
    }
    SUCCEED();
}

}  // namespace
}  // namespace retropp
