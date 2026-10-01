// The chiptune play() path on the SNES: a 65816 sound driver registered for this console runs on
// AudioSystem::SNES exactly as an SM83 driver runs on a Game Boy one — the game drives the audio unit
// through the one general audio path, the audio unit's 32'000 Hz frames reaching the sink.
//
// The cases, one each: a 65816 chiptune plays and its frames reach the sink; a chiptune registered for the
// Game Boy's ISA is refused on a SNES system; and two cues of one chiptune layer as two voices.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "mock_platform.h"  // test::CaptureAudioSink
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/timing.h"
#include "retropp/vm.h"  // Isa, VMPlatform
#include "src/audio/audio_system_testing.h"

namespace retropp {
namespace {

using Access = detail::AudioSystemTestAccess;

// A minimal 65816 sound driver: an infinite loop (BRA to itself). It commands nothing, but the audio unit
// produces frames the whole time the machine runs it — enough to prove the chiptune path reaches the sink.
// Registered AudioType::Music so it never auto-closes, however silent (auto-close is for one-shot SFX).
AudioId registerChiptune(Isa isa) {
    static const std::vector<std::uint8_t> loop{0x80, 0xFE};  // BRA * — a two-byte self-loop
    return AudioLibrary::instance().uploadAudio(loop, AudioType::Music, isa);
}

std::unique_ptr<AudioSystem> snesChiptuneSystem(test::CaptureAudioSink& sink) {
    return Access::makeManual(AudioKind::Chiptune, sink, VMPlatform::Snes, TimingProfile::Snes);
}

TEST(SnesChiptune, A65816ChiptunePlaysAndItsFramesReachTheSink) {
    test::CaptureAudioSink sink;
    auto audio = snesChiptuneSystem(sink);

    audio->play(registerChiptune(Isa::Wdc65816));
    Access::step(*audio);

    EXPECT_FALSE(sink.drain(audio->audioStats().framesBuffered).empty());
}

TEST(SnesChiptune, AChiptuneRegisteredForTheGameBoysIsaIsRefusedHere) {
    test::CaptureAudioSink sink;
    auto audio = snesChiptuneSystem(sink);

    // An SM83 chiptune cannot run on the SNES core: the ISA is checked before anything is placed, so the
    // cue is refused loudly rather than assembling the wrong machine's bytes.
    EXPECT_THROW(audio->play(registerChiptune(Isa::Sm83)), std::runtime_error);
}

TEST(SnesChiptune, TwoCuesOfOneChiptuneLayerAsTwoVoices) {
    test::CaptureAudioSink sink;
    auto audio = snesChiptuneSystem(sink);

    const AudioId song = registerChiptune(Isa::Wdc65816);
    audio->play(song);  // Layer is the default: a second cue plays beside the first, not over it
    audio->play(song);
    EXPECT_EQ(Access::voiceCount(*audio), 2u);

    Access::step(*audio);
    EXPECT_FALSE(sink.drain(audio->audioStats().framesBuffered).empty());
}

}  // namespace
}  // namespace retropp
