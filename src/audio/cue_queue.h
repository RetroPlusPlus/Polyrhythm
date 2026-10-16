#pragma once

// The main→production cue channel.
//
// Production runs on a dedicated thread, so the game's play()/stop() calls do not touch the VM directly —
// the VM lives on the production thread. Instead they marshal a tiny command onto this lock-free SPSC
// queue, which the production thread drains and applies (it owns the VM, so the materialize / startDriver
// work happens there). This keeps the audio data path mutex-free: the same SpscRingBuffer that carries
// PCM frames carries these commands. The condvar in audio_system.cpp governs only the production thread's
// wait/wake, never this data.
//
// SPSC contract: exactly the main (game) thread pushes (play()/stop()); exactly the production thread
// pops (its loop). A second consumer or producer is undefined, same as the PCM ring.
//
// INTERNAL — under src/audio/, never include/retropp/. Header-only.
#ifndef RETROPP_SRC_AUDIO_CUE_QUEUE_H
#define RETROPP_SRC_AUDIO_CUE_QUEUE_H

#include <cstdint>
#include <type_traits>
#include <variant>

#include "retropp/audio_effect.h"   // Echo, Reverb, AudioEffect — the effect path's shape and a cue's effect
#include "retropp/audio_library.h"  // AudioId, AudioType — the cued vocabulary + the driver play lane
#include "retropp/audio_system.h"   // CueMode, PlayMode — how a Play treats same-id voices; a cue's mode
#include "src/audio/ring_buffer.h"

namespace retropp::audio {

// A cue through the sound chip, on the production side: the voice as the number the sampler indexes with,
// the mode, and the effect. AudioSystem::SNES::play(id, Cue) lowers its typed Voice to this plain record.
struct VoiceCue {
    std::uint8_t voice = 0;
    PlayMode     mode{};
    AudioEffect  effect{};
};

// A cue marshaled from the game thread to the production thread. Trivially copyable so it rides the
// existing SpscRingBuffer<T> unchanged. `id` and `mode` are meaningful only for Play (Stop ignores them);
// `mode` is exactly what the play() call site named (or the fixed Layer default — there is no per-system
// mode state).
//
// The Driver* ops carry a hosted resident driver's traffic (retropp/audio_system.h — HostedDriver): `id`
// names the hosted driver's AudioId (the production thread finds its voice by it); `lane` selects a
// DriverPlay's play realization; `value` is the played id a DriverPlay carries into the mailbox / register,
// or the value a DriverSlot writes; `slotIndex` is the declared slot a DriverSlot targets. The handle
// lowers a typed slots(...) batch to one DriverSlot per engaged field on the game thread, so every field
// here is a plain scalar — the queue stays trivially copyable. (HOSTING itself does not ride this queue: a
// host() hands its shared voice across through the Impl's host inbox, since the voice carries a shared_ptr.)
//
// PlayCue carries a play(id, Cue{…}) — the audio file `id` and its `cue`, a trivially copyable value;
// EffectVoice an effect(voice, AudioEffect{…}), the voice and the effect as `cue`; StopVoice a stop(voice),
// the voice in `value`; SetEchoPath an echo(Echo{…}) / echo(Reverb{…}), the path's configuration in `path`.
struct AudioCommand {
    enum class Op { Play, Stop, DriverPlay, DriverStop, DriverSlot, DriverRestart, DriverClose, PlayCue, EffectVoice, StopVoice, SetEchoPath };
    Op                         op;
    AudioId                    id;
    CueMode                    mode      = CueMode::Layer;    // Play only
    AudioType                  lane      = AudioType::Music;  // DriverPlay — which play lane
    std::uint64_t              value     = 0;                 // DriverPlay: the played id; DriverSlot: the write value; StopVoice: the voice
    std::uint32_t              slotIndex = 0;                 // DriverSlot: the declared slot index
    VoiceCue                   cue{};                         // PlayCue / EffectVoice: the voice and the effect
    std::variant<Echo, Reverb> path{};                        // SetEchoPath: the effect path's configuration
};

// The ring copies its slots (ring_buffer.h), so the command it carries must be trivially copyable — the
// one place this is checked, so a member that is not reports itself here.
static_assert(std::is_trivially_copyable_v<AudioCommand>,
              "AudioCommand rides the SPSC ring and must be trivially copyable");

// The main→production cue channel: the same lock-free ring the PCM path uses, instantiated for commands.
using CueQueue = SpscRingBuffer<AudioCommand>;

}  // namespace retropp::audio

#endif  // RETROPP_SRC_AUDIO_CUE_QUEUE_H
