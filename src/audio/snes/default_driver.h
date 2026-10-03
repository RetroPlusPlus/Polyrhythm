#ifndef RETROPP_SRC_AUDIO_SNES_DEFAULT_DRIVER_H
#define RETROPP_SRC_AUDIO_SNES_DEFAULT_DRIVER_H

// The engine's default SNES sound driver: the resident driver an AudioSystem::SNES of the Chiptune kind
// hosts by itself the first time a registered audio file is played on it, so a game plays samples through
// the console's sound chip with no sound program of its own. A 65816 init uploads the SPC700 sound program
// to the audio unit and a 65816 tick hands it commands over the ports each frame; the sources are under
// src/audio/snes/driver/ and the build assembles them.
//
// The game never names this driver. It registers audio files and plays them with play(id, SampleCue);
// what is here is the registration the system hosts and the work-RAM layout its gestures write.
//
// INTERNAL — under src/audio/, never include/retropp/.

#include <cstdint>
#include <optional>

#include "retropp/audio_library.h"  // DriverId

namespace retropp::audio::snesdriver {

// What the driver reports back each frame.
struct Slots {
    std::optional<std::uint8_t> ended;   // a bit per voice that has reached the end of its sample (ENDX)
    std::optional<std::uint8_t> frames;  // the frames the tick has run, as a byte
};

// The driver's registration on the library — made on the first call, the same id after.
[[nodiscard]] DriverId<Slots> defaultDriver();

// Audio RAM as the driver lays it out: the sound program at $0200, the sample directory at page $03 (64
// entries of four bytes), the samples from $0400 up to the boot window.
inline constexpr std::uint16_t kDirectory    = 0x0300;
inline constexpr std::uint8_t  kDirectorySlots = 64;
inline constexpr std::uint16_t kSamplesStart = 0x0400;
inline constexpr std::uint16_t kSamplesEnd   = 0xFFC0;

// The driver's bytes in work RAM: a block per voice, the two mailboxes the handle's verbs write, the two
// bytes it reports, and a repeat's period and countdown per voice, in frames.
inline constexpr std::uint32_t kBlockBase   = 0x7E0020;
inline constexpr std::uint32_t kBlockStride = 8;
inline constexpr std::uint32_t kPlayMailbox = 0x7E0060;
inline constexpr std::uint32_t kStopMailbox = 0x7E0061;
inline constexpr std::uint32_t kEnded       = 0x7E0063;
inline constexpr std::uint32_t kFrames      = 0x7E0064;
inline constexpr std::uint32_t kPeriods     = 0x7E0070;  // a word per voice
inline constexpr std::uint32_t kCountdowns  = 0x7E0080;  // a word per voice
inline constexpr std::uint8_t  kVoices      = 8;

[[nodiscard]] constexpr std::uint32_t voiceBlock(std::uint8_t voice) noexcept {
    return kBlockBase + voice * kBlockStride;
}
[[nodiscard]] constexpr std::uint32_t voicePeriod(std::uint8_t voice) noexcept { return kPeriods + voice * 2u; }
[[nodiscard]] constexpr std::uint32_t voiceCountdown(std::uint8_t voice) noexcept { return kCountdowns + voice * 2u; }

// The play mode byte of a voice's block, as the tick reads it.
enum class Mode : std::uint8_t { Continuous = 0, Once = 1, Repeat = 2 };

// A voice's block for a key-on — the command, the sample's directory entry, the pitch, the two volumes
// and the mode — packed low byte first into the one value an eight-byte Instruction::write carries.
[[nodiscard]] constexpr std::uint64_t keyOnBlock(std::uint8_t entry, std::uint16_t pitch, std::uint8_t left,
                                                 std::uint8_t right, Mode mode) noexcept {
    return std::uint64_t{1} | (std::uint64_t{entry} << 8) | (std::uint64_t{pitch} << 16) |
           (std::uint64_t{left} << 32) | (std::uint64_t{right} << 40) |
           (std::uint64_t{static_cast<std::uint8_t>(mode)} << 48);
}
inline constexpr int kKeyOnWidth = 8;

// A voice's block for a key-off: the command alone, the rest of the block unread.
inline constexpr std::uint64_t kKeyOffBlock = 2;

// A voice's block for a change to the voice as it plays — its pitch and volumes, no key-on; the mode
// byte past these six is left as it is.
[[nodiscard]] constexpr std::uint64_t changeBlock(std::uint16_t pitch, std::uint8_t left, std::uint8_t right) noexcept {
    return std::uint64_t{3} | (std::uint64_t{pitch} << 16) | (std::uint64_t{left} << 32) | (std::uint64_t{right} << 40);
}
inline constexpr int kChangeWidth = 6;

}  // namespace retropp::audio::snesdriver

#endif  // RETROPP_SRC_AUDIO_SNES_DEFAULT_DRIVER_H
