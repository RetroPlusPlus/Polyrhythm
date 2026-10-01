// The SNES audio unit, driven by the game the way the console's CPU drives it.
//
// The audio unit is a sound CPU, 64 KB of its own RAM, and the S-DSP, an eight-voice sample player. It
// makes no sound of its own: the console gives it sample data and tells the chip to play it. On the
// console the 65816 does that through four communication ports. Here there is no 65816 at all: an
// AudioSystem::SNES of the HostDriven kind holds the audio unit alone — no console CPU, no picture chip —
// running on its own clock from the moment it is built, and the game reaches it as places:
//
//   snes::audioPort(n)     a communication port, from the console's side: writing sends a byte the sound
//                          CPU reads, reading receives the byte the sound CPU last sent
//   snes::audioRam(x)      the audio unit's RAM, where samples and their directory go
//   snes::dspRegister(n)   an S-DSP register: writing one is the sound CPU's own write to the chip
//
// Everything this class does is one of those three, through the system's own read and write. What the
// chip makes is the system's output: the system plays it through the device.
#pragma once

#include <cstdint>

#include "retropp/audio.h"
#include "retropp/audio_system.h"

#include "samples.h"

namespace demo {

class AudioUnit {
public:
    static constexpr int kVoices = 8;
    static constexpr int kSlots  = 4;  // sample slots in the directory this demo fills

    // The audio unit alone, its main volume up and the chip unmuted, its sound on the device.
    AudioUnit();

    // The same, its sound to `sink` instead of the device — what --verify listens with.
    explicit AudioUnit(retropp::AudioSink& sink);

    // ── The ports: the sound CPU's boot program ─────────────────────────────────────────────────
    // The sound CPU's boot program waits on the ports: $AA in port 0, $BB in port 1. Reading the ports
    // receives what it sent. Writing them sends it a transfer request, which it acknowledges by echoing
    // the kick byte, $CC, back in port 0 — the first exchange of a program upload, and all this demo needs
    // of it, since the sound CPU has nothing to run here.
    [[nodiscard]] bool bootProgramReady();
    [[nodiscard]] bool bootProgramAcknowledgesAKick();

    // ── Audio RAM: the samples ──────────────────────────────────────────────────────────────────
    // The block into audio RAM, and a directory entry naming it as source `slot`: the sample's start and
    // where it loops back to — the same address, since the block loops on itself.
    void loadSample(int slot, const BrrBlock& block);

    // ── The chip: one voice ─────────────────────────────────────────────────────────────────────
    // Each voice has eight registers of its own at voice x $10; these write the ones this demo uses.
    void source(int voice, int slot);                           // which sample
    void pitch(int voice, std::uint16_t pitch);                  // $1000 plays the sample at its own rate
    // A voice's volume is two registers, its left and its right, $00-$7F each: a pan is nothing more than
    // the two set unequally.
    void volume(int voice, std::uint8_t left, std::uint8_t right);
    // A key-on is an event — the chip's next poll takes it once. A key-off is a level: the chip reads
    // KOFF on every poll and holds the voice off for as long as its bit stays set, so keying on clears
    // the voice's KOFF bit first, or the key-on would be taken straight back off and the voice would never
    // sound again. A key-off does not stop the voice at once: its envelope releases over a few frames.
    void keyOn(int voice);
    void keyOff(int voice);

    // A DSP register as it stands, by number.
    [[nodiscard]] std::uint8_t dspRegister(std::uint8_t reg);
    // A byte of audio RAM as it stands.
    [[nodiscard]] std::uint8_t audioRam(std::uint16_t address);

private:
    void setUpChip();
    void writeDsp(std::uint8_t reg, std::uint8_t value);

    retropp::AudioSystem::SNES system_;   // holds the audio unit alone and plays what it makes
};

}  // namespace demo
