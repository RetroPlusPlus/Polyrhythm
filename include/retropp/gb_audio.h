#pragma once

// Game Boy audio presets — the GB-family catalog of ready-made audio for the AudioLibrary, the audio
// analog of retropp/gb_routines.h (the GB RNG presets over a generic Vm).
//
// These are CONSOLE-SPECIFIC by design. A preset REGISTERS Game-Boy audio on the single catalog
// (AudioLibrary), selecting Isa::Sm83 — it does NOT touch an AudioSystem (registration is not the
// system's job). Anything that bakes in Game-Boy specifics (a wave-channel diagnostic tone here) lives
// HERE, NOT on the generic surfaces — so a SNES / Genesis console brings
// its own presets in its own header, and nothing Game-Boy leaks into the shared API. The returned AudioId
// plays on any AudioSystem constructed for a Game Boy console (VMPlatform::GameBoy / GameBoyColor); cuing
// it on a different-ISA system throws (the ISA selected here is verified at play()).
//
// Eligibility: only public-domain / permissively-licensed content ships as a platform preset. A game's
// own sound driver — or a tracker-exported driver — is not a preset: it is HOSTED through the
// driver-hosting surface (AudioLibrary::uploadDriver / AudioSystem::host), which places the driver's own
// extracted images, so a copyrighted sound engine is never embedded here — the game supplies and hosts it.

#include "retropp/audio_library.h"

namespace retropp::sameboy {

// diagnosticTone — register the platform's built-in Game Boy test tone on the single AudioLibrary: a
// gentle, steady ~250 Hz triangle on the wave channel (a low-harmonic, moderate-volume waveform chosen
// to be accessibility-considerate, not a harsh square). The verification signal for the audio chain —
// prove output works with no game driver. Returns a handle to cue (on a Game Boy AudioSystem) exactly
// like any registered audio. The tone is SM83 wave-channel assembly, baked at compile time.
AudioId diagnosticTone(AudioType type = AudioType::Sfx);

}  // namespace retropp::sameboy
