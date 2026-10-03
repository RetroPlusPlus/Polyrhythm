#ifndef RETROPP_SRC_AUDIO_SNES_BRR_H
#define RETROPP_SRC_AUDIO_SNES_BRR_H

// The S-DSP's sample format, both ways: a registered audio file is encoded into it so the SNES's sound
// chip can play the file, and a `.brr` file is decoded out of it so a Pcm system can stream the sample.
//
// BRR is bit-rate-reduced PCM: blocks of nine bytes, each holding sixteen samples as signed four-bit
// values. A block's header byte carries the shift its values are scaled by (bits 7-4), which of four
// prediction filters the chip runs the scaled value through (bits 3-2), whether the block is the last
// (bit 0, END) and whether the voice loops when it is (bit 1, LOOP). The chip plays at 32'000 Hz; a
// sample encoded here is at that rate, so a pitch of $1000 plays it as recorded.
//
// INTERNAL — under src/audio/, never include/retropp/.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/audio.h"  // AudioFrame

namespace retropp::audio::brr {

inline constexpr std::size_t kBlockBytes   = 9;
inline constexpr std::size_t kBlockSamples = 16;
inline constexpr unsigned    kSampleRate   = 32'000;  // the chip's own rate

// Encode mono 16-bit samples at kSampleRate into BRR blocks. Each block takes the shift and filter that
// reproduce its sixteen samples with the least error through the chip's own arithmetic; the first block
// uses filter 0, since the chip's filter history is empty at a key-on. The last block is padded with
// silence to sixteen samples and carries END, plus LOOP when `loop` is set — the voice then continues from
// the first block. An empty input encodes to one silent END block.
[[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::int16_t> samples, bool loop);

// Decode BRR blocks to mono 16-bit samples with the chip's arithmetic, one pass through the blocks (LOOP is
// not followed). Throws std::invalid_argument on a size that is not a nonzero multiple of nine.
[[nodiscard]] std::vector<std::int16_t> decode(std::span<const std::uint8_t> blocks);

// Decode BRR blocks to the engine's stereo frames at `targetRate` — a `.brr` file played on a Pcm system.
[[nodiscard]] std::vector<AudioFrame> decodeToFrames(std::span<const std::uint8_t> blocks, unsigned targetRate);

// Throw std::invalid_argument unless `blocks` is a sample the chip can play to its end: a nonzero multiple
// of nine bytes whose last block carries END.
void requireWellFormed(std::span<const std::uint8_t> blocks);

// Fold the engine's stereo frames to the mono samples the encoder takes.
[[nodiscard]] std::vector<std::int16_t> toMono(std::span<const AudioFrame> frames);

}  // namespace retropp::audio::brr

#endif  // RETROPP_SRC_AUDIO_SNES_BRR_H
