#include "src/audio/snes/sample_stream.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace retropp::audio::snesstream {

namespace {

// The chip plays a voice at 32'000 samples a second times its pitch over $1000; the pitch register holds
// 14 bits.
constexpr std::uint64_t kPitchAsRecorded = 0x1000;
constexpr std::uint64_t kPitchMax        = 0x3FFF;

// The decoder fills a voice's buffer a group of four samples at a time, four to twelve samples ahead of
// the one sounding, so over a frame it can move eight samples further than the voice does.
constexpr std::uint64_t kDecoderLeadSwing = 8;

// A block header's flags.
constexpr std::uint8_t kEnd     = 0x01;
constexpr std::uint8_t kLoop    = 0x02;
constexpr std::uint8_t kEndLoop = kEnd | kLoop;

}  // namespace

std::size_t blocksPerStepAtMostFor(const vm::MachineClock& clock) {
    // The samples a frame plays at the highest pitch, rounded up: 32'000 × the frame's seconds × $3FFF / $1000.
    const std::uint64_t numerator = static_cast<std::uint64_t>(brr::kSampleRate) * clock.cyclesPerFrame *
                                    clock.hertzDivisor * kPitchMax;
    const std::uint64_t divisor   = static_cast<std::uint64_t>(clock.hertzNumerator) * kPitchAsRecorded;
    const std::uint64_t samples   = (numerator + divisor - 1) / divisor + kDecoderLeadSwing;
    return static_cast<std::size_t>((samples + brr::kBlockSamples - 1) / brr::kBlockSamples);
}

void requireStreamable(const vm::MachineClock& clock) {
    const std::size_t perStep = blocksPerStepAtMostFor(clock);
    if (perStep + kStreamGuardBlocks > kStreamOpeningBlocks) {
        throw std::logic_error("AudioSystem::play: a voice on this clock plays " + std::to_string(perStep) +
                               " blocks a frame, more than a streamed file's window keeps ahead of it");
    }
}

Stream::Stream(std::shared_ptr<const std::vector<std::uint8_t>> blocks, std::uint16_t opening, bool selfEnding)
    : blocks_(std::move(blocks)),
      fileBlocks_(blocks_->size() / brr::kBlockBytes),
      opening_(opening),
      selfEnding_(selfEnding) {}

std::vector<Write> Stream::start() {
    std::vector<Write> out;
    out.reserve(kStreamWindowBlocks);
    nextPosition_ = 0;
    written_      = 0;
    consumed_     = 0;
    lastSlot_     = 0;
    enteredLoop_  = false;
    drained_      = false;
    for (std::uint16_t i = 0; i < kStreamOpeningBlocks && !drained_; ++i) {
        writeBlock(out, nextPosition_++, static_cast<std::uint16_t>(opening_ + i * brr::kBlockBytes),
                   /*lastOfLoop=*/false);
    }
    while (written_ < kStreamLoopBlocks && !drained_) {
        writeNextLoopBlock(out);
    }
    return out;
}

std::vector<Write> Stream::advance(std::uint16_t decoderAddress) {
    std::vector<Write> out;
    if (decoderAddress < opening_) {
        return out;
    }
    const std::size_t index = (decoderAddress - opening_) / brr::kBlockBytes;
    if (index >= kStreamWindowBlocks) {
        return out;
    }
    if (index < kStreamOpeningBlocks) {
        if (!enteredLoop_) {
            return out;  // still on its way through the opening; the loop is full ahead of it
        }
        // Keyed on again: the opening holds the file's start as it always does, and the loop is written
        // again with what follows it.
        nextPosition_ = kStreamOpeningBlocks;
        written_      = 0;
        consumed_     = 0;
        enteredLoop_  = false;
        drained_      = false;
        while (written_ < kStreamLoopBlocks && !drained_) {
            writeNextLoopBlock(out);
        }
        return out;
    }
    const auto slot = static_cast<std::uint16_t>(index - kStreamOpeningBlocks);
    if (!enteredLoop_) {
        enteredLoop_ = true;
        consumed_    = slot;  // the loop blocks before the one it stands in
    } else {
        // A frame passes fewer blocks than the loop holds, so the distance round the loop is exact.
        consumed_ += (slot + kStreamLoopBlocks - lastSlot_) % kStreamLoopBlocks;
    }
    lastSlot_ = slot;
    while (!drained_ && written_ < consumed_ + (kStreamLoopBlocks - kStreamGuardBlocks)) {
        writeNextLoopBlock(out);
    }
    return out;
}

void Stream::writeNextLoopBlock(std::vector<Write>& out) {
    const auto slot = static_cast<std::uint16_t>(written_ % kStreamLoopBlocks);
    if (writeBlock(out, nextPosition_, static_cast<std::uint16_t>(loop() + slot * brr::kBlockBytes),
                   slot == kStreamLoopBlocks - 1)) {
        ++nextPosition_;
        ++written_;
    }
}

bool Stream::writeBlock(std::vector<Write>& out, std::uint64_t position, std::uint16_t address, bool lastOfLoop) {
    if (selfEnding_ && position >= fileBlocks_) {
        drained_ = true;  // past the end of a file that ends itself: nothing is left to write
        return false;
    }
    const std::uint64_t fileBlock = position % fileBlocks_;
    Write               w{.address = address};
    std::copy_n(blocks_->begin() + static_cast<std::ptrdiff_t>(fileBlock * brr::kBlockBytes), brr::kBlockBytes,
                w.block.begin());
    w.block[0] = static_cast<std::uint8_t>(w.block[0] & ~kEndLoop);
    if (selfEnding_ && fileBlock == fileBlocks_ - 1) {
        w.block[0] = static_cast<std::uint8_t>(w.block[0] | kEnd);  // the pass ends here, wherever here is
        drained_   = true;
    } else if (lastOfLoop) {
        w.block[0] = static_cast<std::uint8_t>(w.block[0] | kEndLoop);
    }
    out.push_back(w);
    return true;
}

}  // namespace retropp::audio::snesstream
