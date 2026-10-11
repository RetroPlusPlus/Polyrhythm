#ifndef RETROPP_SRC_AUDIO_SNES_SAMPLE_STREAM_H
#define RETROPP_SRC_AUDIO_SNES_SAMPLE_STREAM_H

// An audio file longer than audio RAM holds, streamed through the SNES's sound chip: the file is encoded
// once and held on the host, and its blocks are written into a fixed stretch of audio RAM — the voice's
// window — a few at a time, as the voice plays through them.
//
// The window has two parts. The opening holds the file's first blocks and is written once, when the
// stream starts; a key-on starts the voice there. The loop follows it directly and holds the blocks after
// those, its last block carrying the END and LOOP flags, so the chip plays from the opening into the loop
// and then round the loop for as long as the voice is held. The chip reads a voice's blocks through its
// decoder, which runs a few samples ahead of what the voice sounds; as the decoder leaves a loop block,
// that block's place is written again with the next block of the file the loop has not had yet.
//
// This is the arithmetic alone, with no machine in it: a Stream answers the blocks to write and where, and
// the caller moves the bytes. INTERNAL — under src/audio/, never include/retropp/.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/audio/snes/brr.h"   // brr::kBlockBytes
#include "src/vm/vm_backend.h"    // vm::MachineClock

namespace retropp::audio::snesstream {

// The window, in blocks and in bytes. The opening outlasts the most blocks a voice plays in one frame of
// either console clock, so a voice keyed on part-way through a frame is still inside the opening when the
// next frame's delivery runs; the loop, less the guard, holds more than a frame's worth ahead of the voice.
inline constexpr std::uint16_t kStreamOpeningBlocks = 176;
inline constexpr std::uint16_t kStreamLoopBlocks    = 279;
inline constexpr std::uint16_t kStreamWindowBlocks  = kStreamOpeningBlocks + kStreamLoopBlocks;
inline constexpr std::size_t   kStreamWindowBytes   = kStreamWindowBlocks * brr::kBlockBytes;  // 4095

// How far short of the decoder, coming round the loop, the writing stops: the block it just left, whose
// header the chip's end-and-loop check still reads, and the one before that are never written.
inline constexpr std::uint16_t kStreamGuardBlocks = 2;

// The most blocks a voice's decoder passes in one frame of `clock`, at the chip's highest pitch ($3FFF):
// the samples the voice plays in a frame at that pitch, plus the eight by which the decoder's lead over
// them can grow, in blocks — 134 on the 60 Hz console's clock, 161 on the 50 Hz one.
[[nodiscard]] std::size_t blocksPerStepAtMostFor(const vm::MachineClock& clock);

// Throws std::logic_error when the window cannot keep ahead of a voice on `clock`: when the opening does
// not outlast a frame's blocks with the guard to spare.
void requireStreamable(const vm::MachineClock& clock);

// One block to write: its address in audio RAM and its nine bytes, header flags as they are to stand.
struct Write {
    std::uint16_t                              address = 0;
    std::array<std::uint8_t, brr::kBlockBytes> block{};
};

// One file streamed through one window.
//
// The file is the stream's sequence of blocks: from its first block to its last and, unless the stream
// ends itself, round from its first again. A stream that ends itself — a pass played once or struck at a
// tempo, or a `.brr` whose own last block ends without looping — writes the file's last block with END
// alone, wherever it lands, and writes nothing after it; the chip mutes the voice there. Every block is
// written with its END and LOOP flags cleared, then the loop's last block gains both.
class Stream {
public:
    // `blocks` is the encoded file, a nonzero multiple of nine bytes; `opening` is the window's address in
    // audio RAM.
    Stream(std::shared_ptr<const std::vector<std::uint8_t>> blocks, std::uint16_t opening, bool selfEnding);

    // The opening and the whole loop, for a stream that starts.
    [[nodiscard]] std::vector<Write> start();

    // The blocks to write now that the voice's decoder stands at `decoderAddress`. In the loop, the blocks
    // it has left are written again with the file's next blocks, up to the guard behind it. In the opening
    // after it has been in the loop, the voice has been keyed on again: the whole loop is written again
    // from the file's block after the opening's. Anywhere outside the window, nothing.
    [[nodiscard]] std::vector<Write> advance(std::uint16_t decoderAddress);

    [[nodiscard]] std::uint16_t opening() const noexcept { return opening_; }
    [[nodiscard]] std::uint16_t loop() const noexcept {
        return static_cast<std::uint16_t>(opening_ + kStreamOpeningBlocks * brr::kBlockBytes);
    }
    [[nodiscard]] bool selfEnding() const noexcept { return selfEnding_; }
    // Whether the file's last block has been written and nothing more will be.
    [[nodiscard]] bool drained() const noexcept { return drained_; }

private:
    // The block at `position` in the stream's sequence into `address`, flagged; `lastOfLoop` when the
    // address is the loop's last block. Answers whether a block was written: past a self-ending file's end
    // there is none, and the stream is drained.
    bool writeBlock(std::vector<Write>& out, std::uint64_t position, std::uint16_t address, bool lastOfLoop);
    // The next loop block, at the loop slot `written_` names, with the stream's next block.
    void writeNextLoopBlock(std::vector<Write>& out);

    std::shared_ptr<const std::vector<std::uint8_t>> blocks_;
    std::uint64_t  fileBlocks_   = 0;
    std::uint16_t  opening_      = 0;
    bool           selfEnding_   = false;
    std::uint64_t  nextPosition_ = 0;      // the next block of the sequence to write into the loop
    std::uint64_t  written_      = 0;      // loop blocks written since the stream started or the voice was keyed on again
    std::uint64_t  consumed_     = 0;      // loop blocks the decoder has passed, on the same count
    std::uint16_t  lastSlot_     = 0;      // the loop slot the decoder stood in at the last advance
    bool           enteredLoop_  = false;  // the decoder has been in the loop since that start
    bool           drained_      = false;
};

}  // namespace retropp::audio::snesstream

#endif  // RETROPP_SRC_AUDIO_SNES_SAMPLE_STREAM_H
