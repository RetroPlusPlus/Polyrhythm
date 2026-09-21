#pragma once

// The internal test seam for driving AudioSystem production SYNCHRONOUSLY, with the production thread
// suppressed.
//
// Production normally runs autonomously on its own thread, so the game just cues with play()/stop() and
// never steps anything — there is deliberately no public stepping method. But device-free tests need
// DETERMINISTIC production: drive one pass, assert the exact buffer level. This seam gives them that
// without polluting the game-facing surface — `makeManual` builds an AudioSystem whose production thread
// is NOT started (play()/stop() apply their cue inline instead of marshaling), and `step` /
// `stepDriverRaw` drive it by hand on the calling thread. Reachable only through this internal header (a
// friend of AudioSystem); never in include/retropp/.
//
// INTERNAL — under src/audio/. Definitions live in audio_system.cpp, where AudioSystem::Impl is
// complete; makeManual is defined here, so the core is resolved where the test names the platform.
#ifndef RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H
#define RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "retropp/audio.h"          // AudioSink, kAudioSampleRate
#include "retropp/audio_system.h"
#include "retropp/timing.h"         // TimingProfile
#include "retropp/vm.h"             // VMPlatform

namespace retropp::detail {

// Friend-of-AudioSystem accessor: builds and drives a manual-mode (thread-suppressed) AudioSystem so
// tests get deterministic, synchronous production they can step and assert against by hand.
struct AudioSystemTestAccess {
    // Build a `kind` AudioSystem (borrowing `sink`) whose production thread is NOT started. In this mode
    // play()/stop() apply their cue inline on the calling thread, so isPlaying() reflects a cue
    // immediately; the caller produces frames via step()/stepDriverRaw().
    static std::unique_ptr<AudioSystem> makeManual(AudioKind     kind,
                                                   AudioSink&    sink,
                                                   VMPlatform    platform   = VMPlatform::GameBoyColor,
                                                   TimingProfile timing     = TimingProfile::GameBoyColor,
                                                   unsigned      sampleRate = kAudioSampleRate) {
        return std::unique_ptr<AudioSystem>(
            new AudioSystem(AudioSystem::ManualTag{}, kind, sink, platform, timing, sampleRate));
    }

    // One production iteration on the calling thread: drain any pending cues, then, if playing, run one
    // frame-quantized refill-to-target produce pass and the auto-close check. The deterministic,
    // synchronous path the device-free AudioSystem tests drive.
    static void step(AudioSystem& sys);

    // How many voices are currently sounding on `sys`.
    static std::size_t voiceCount(const AudioSystem& sys);

    // Advance ONE voice's machine by a single step, leaving every other voice where it is. Stepping the
    // voices unevenly is how a test produces a short lane — the case the mix substitutes silence for —
    // without threads, and therefore without waiting on one machine to fall behind another.
    static void stepVoice(AudioSystem& sys, std::size_t index);

    // Mix what the voices have produced into the ring: one pass of the substitution, on demand.
    // `wanted` is how many frames the output asks for, which is what paces the mix when the machines
    // run on their own clocks; the default asks for everything, as a caller that stepped the machines
    // itself does.
    static void mix(AudioSystem& sys, std::size_t wanted = SIZE_MAX);

    // Frames of silence the mix has substituted for voice `index` since it first produced — its share of
    // the passes where it had less to give than the pace the mix kept. Reads the voice directly, so it
    // belongs to a system whose voices this thread owns.
    static std::size_t laneUnderflowFrames(const AudioSystem& sys, std::size_t index);

    // Frames voice `index` has produced that the mix has not taken yet — its share of the standing
    // inventory between a machine and the output. The count reads the lane's own atomics, so it
    // answers while a machine runs; the voice list it indexes belongs to the production thread, so a
    // threaded caller reads a system whose voices have settled — every cue applied, nothing closing.
    static std::size_t laneFrames(const AudioSystem& sys, std::size_t index);

    // Frames voice `index`'s lane holds before it is full — the room a machine has to produce into,
    // which the pacing is what keeps it inside.
    static std::size_t laneCapacity(const AudioSystem& sys, std::size_t index);

    // The frames the output keeps in hand: the level production tops the buffer back up to, and the
    // level it stops waiting for a straggling machine at. The pacing every machine runs under is stated
    // in these two numbers, so a test asserting on inventory or on substitution reads them rather than
    // restating the arithmetic that derives them.
    static std::size_t latencyTarget(const AudioSystem& sys);
    static std::size_t waitingFloor(const AudioSystem& sys);

    // The audio frames one step of a machine produces — the granularity a machine overshoots the
    // latency target by, since it decides whether to step before it knows how much the step yields.
    static std::size_t framesPerStep(const AudioSystem& sys);

    // Lower-level: drain any pending cues, then, if playing, advance the FIRST voice's driver by exactly
    // `cycles` CPU cycles (one stepDriver call) and mix its laned samples into the ring, returning the
    // cycles actually run (0 if not playing). Bypasses refill-to-target so a test can capture a driver's
    // output at a chosen cycle granularity (the golden gate compares two granularities over the same
    // total cycles, asserting identical PCM — it cues a single voice, where the mix is the identity).
    static std::uint64_t stepDriverRaw(AudioSystem& sys, std::uint64_t cycles);
};

}  // namespace retropp::detail

#endif  // RETROPP_SRC_AUDIO_AUDIO_SYSTEM_TESTING_H
