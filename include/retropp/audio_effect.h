#pragma once

// The audio effects vocabulary: what a game asks of a sound as it cues it, in audio terms — the `.effect`
// of a Cue (retropp/audio_system.h):
//
//   music.play(kick, Cue{.voice = 3, .effect = AudioEffect{.pitch = 1.5f,
//                        .echo = Echo{.delay = 0.128f, .feedback = 0.25f, .level = 0.5f}}});
//
// One vocabulary for every system; each system realizes the fields its sound hardware has and throws
// std::invalid_argument naming a field it cannot, so an effect is never silently dropped.
//
// Every field has a default that changes nothing, so a default-constructed AudioEffect is the identity —
// a cue names only the fields it changes. The struct grows by fields; a call site that names what it uses
// is untouched when a field is added.

#include <optional>

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
    float                 pitch  = 1.0f;  // the playback rate: 1 as recorded, 2 an octave up, 0.5 an octave down
    float                 volume = 1.0f;  // 0 silent to 1 full
    float                 pan    = 0.0f;  // -1 the left side only, 0 the middle, 1 the right side only
    std::optional<Echo>   echo{};
    std::optional<Reverb> reverb{};
};

}  // namespace retropp
