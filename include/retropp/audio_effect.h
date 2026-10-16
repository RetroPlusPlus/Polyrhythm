#pragma once

// The audio effects vocabulary: how a sound is shaped as a game cues it. `AudioEffect` is the per-voice
// shaping a cue carries in its `.effect` (retropp/audio_system.h) — the playback rate, the volume, the
// stereo position, and whether the voice feeds the system's effect path:
//
//   snes.play(kick, {.voice = 3, .effect = {.pitch = 1.5f, .echo = true}});
//
// `Echo` and `Reverb` are the shape of that path itself — its delay, feedback and levels — set once for
// the system through its effect surface (AudioSystem::SNES::echo), for every voice that feeds it. One
// vocabulary for every system that has effects; the SNES's sound chip realizes it today, and the PCM
// system is the next to. A system with one effect path (the SNES's sound chip) routes a voice's `echo`
// and its `reverb` to that single path, which sounds as the system's most recent echo() set it; a system
// with paths of its own kind routes each to its own.
//
// Every field has a default that changes nothing, so a default-constructed AudioEffect leaves the voice as
// recorded — a cue names only the fields it changes. The struct grows by fields; a call site that names
// what it uses is untouched when a field is added. AudioEffect is trivially copyable, so it rides the cue
// channel as it is.

namespace retropp {

// A delayed copy of the sound fed back on itself. `delay` is the time between repeats in seconds,
// `feedback` how much of each repeat feeds the next (0 one repeat, 1 repeats forever), `level` how loud the
// repeats are against the sound itself.
struct Echo {
    float delay    = 0.128f;
    float feedback = 0.25f;
    float level    = 0.5f;
};

// A diffuse tail on the sound. `decay` is how long the tail hangs on (0 none, 1 the longest the hardware
// gives), `level` how loud it is against the sound itself.
struct Reverb {
    float decay = 0.5f;
    float level = 0.5f;
};

struct AudioEffect {
    float pitch  = 1.0f;   // the playback rate: 1 as recorded, 2 an octave up, 0.5 an octave down
    float volume = 1.0f;   // 0 silent to 1 full
    float pan    = 0.0f;   // -1 the left side only, 0 the middle, 1 the right side only
    bool  echo   = false;  // the voice feeds the system's effect path
    bool  reverb = false;  // the voice feeds the system's effect path
};

}  // namespace retropp
