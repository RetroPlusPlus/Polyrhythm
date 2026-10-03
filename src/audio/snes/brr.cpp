// The S-DSP's sample format. The decoder is the chip's arithmetic — the nibble scaled and halved, the four
// filters in their integer forms, the clamp to 16 bits and the clip to 15 — so what the encoder measures a
// choice against is what the chip will play. Samples move through the chip as 15-bit values; a 16-bit
// sample enters as its upper fifteen bits and leaves doubled.
#include "src/audio/snes/brr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace retropp::audio::brr {

namespace {

constexpr int kEndFlag  = 0x01;
constexpr int kLoopFlag = 0x02;
constexpr int kMaxShift = 12;

// A signed four-bit value scaled by the block's shift, then halved: the sample the filter takes.
[[nodiscard]] int scaled(int nibble, int shift) noexcept { return (nibble * (1 << shift)) >> 1; }

// The four prediction filters over the two samples decoded before this one.
[[nodiscard]] int filtered(int filter, int sample, int old, int older) noexcept {
    switch (filter) {
        case 0:
            return sample;
        case 1:
            return sample + old + ((-old) >> 4);
        case 2:
            return sample + old * 2 + ((-old * 3) >> 5) - older + (older >> 4);
        default:
            return sample + old * 2 + ((-old * 13) >> 6) - older + ((older * 3) >> 4);
    }
}

// Clamp to 16 bits, then clip to 15 by reading bit 14 as the sign — the chip's own.
[[nodiscard]] int clampAndClip(int value) noexcept {
    value = std::clamp(value, -0x8000, 0x7FFF);
    return ((value & 0x7FFF) ^ 0x4000) - 0x4000;
}

// The chip's filter history: the two 15-bit samples decoded most recently.
struct History {
    int old   = 0;
    int older = 0;
};

// Decode one nibble through `filter` at `shift`, advancing the history; answers the 15-bit sample.
[[nodiscard]] int step(int nibble, int shift, int filter, History& h) noexcept {
    const int out = clampAndClip(filtered(filter, scaled(nibble, shift), h.old, h.older));
    h.older       = h.old;
    h.old         = out;
    return out;
}

// Encode sixteen 15-bit samples with one shift and filter: the nibbles, the history after them, and the
// squared error against the input. Each nibble is the one of three candidates around the ideal that the
// chip's arithmetic brings nearest the sample.
struct Trial {
    std::array<int, kBlockSamples> nibbles{};
    History                        history;
    double                         error = 0;
};

[[nodiscard]] Trial trial(std::span<const int, kBlockSamples> target, int shift, int filter, History h) {
    Trial t;
    t.history = h;
    for (std::size_t i = 0; i < kBlockSamples; ++i) {
        // The residual the nibble must supply, and the ideal nibble for it: the filter's prediction is
        // what the chip adds to the scaled nibble, so the nibble covers the rest.
        const int prediction = filtered(filter, 0, t.history.old, t.history.older);
        const int residual   = target[i] - prediction;
        const int ideal      = static_cast<int>(std::lround(static_cast<double>(residual) * 2.0 / (1 << shift)));
        int       bestNibble = 0;
        int       bestOut    = 0;
        long long bestError  = -1;
        for (int candidate = ideal - 1; candidate <= ideal + 1; ++candidate) {
            const int n   = std::clamp(candidate, -8, 7);
            History   h2  = t.history;
            const int out = step(n, shift, filter, h2);
            const long long e = static_cast<long long>(out - target[i]) * (out - target[i]);
            if (bestError < 0 || e < bestError) {
                bestError  = e;
                bestNibble = n;
                bestOut    = out;
            }
        }
        t.nibbles[i]     = bestNibble;
        t.history.older  = t.history.old;
        t.history.old    = bestOut;
        t.error         += static_cast<double>(bestError);
    }
    return t;
}

}  // namespace

std::vector<std::uint8_t> encode(std::span<const std::int16_t> samples, bool loop) {
    const std::size_t blocks = std::max<std::size_t>(1, (samples.size() + kBlockSamples - 1) / kBlockSamples);
    std::vector<std::uint8_t> out;
    out.reserve(blocks * kBlockBytes);
    History history;
    for (std::size_t b = 0; b < blocks; ++b) {
        std::array<int, kBlockSamples> target{};
        for (std::size_t i = 0; i < kBlockSamples; ++i) {
            const std::size_t at = b * kBlockSamples + i;
            target[i]            = at < samples.size() ? samples[at] >> 1 : 0;  // 15-bit, silence past the end
        }
        const int filters = b == 0 ? 1 : 4;  // the first block predicts from nothing
        Trial     best;
        int       bestShift  = 0;
        int       bestFilter = 0;
        bool      found      = false;
        for (int filter = 0; filter < filters; ++filter) {
            for (int shift = 0; shift <= kMaxShift; ++shift) {
                Trial t = trial(std::span<const int, kBlockSamples>(target), shift, filter, history);
                if (!found || t.error < best.error) {
                    best       = t;
                    bestShift  = shift;
                    bestFilter = filter;
                    found      = true;
                }
            }
        }
        history  = best.history;
        int head = (bestShift << 4) | (bestFilter << 2);
        if (b + 1 == blocks) {
            head |= kEndFlag | (loop ? kLoopFlag : 0);
        }
        out.push_back(static_cast<std::uint8_t>(head));
        for (std::size_t i = 0; i < kBlockSamples; i += 2) {
            const int hi = best.nibbles[i] & 0x0F;
            const int lo = best.nibbles[i + 1] & 0x0F;
            out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
    }
    return out;
}

std::vector<std::int16_t> decode(std::span<const std::uint8_t> blocks) {
    if (blocks.empty() || blocks.size() % kBlockBytes != 0) {
        throw std::invalid_argument("brr::decode: a BRR sample is a nonzero multiple of nine bytes");
    }
    std::vector<std::int16_t> out;
    out.reserve(blocks.size() / kBlockBytes * kBlockSamples);
    History history;
    for (std::size_t b = 0; b < blocks.size(); b += kBlockBytes) {
        const int header = blocks[b];
        const int shift  = header >> 4;
        const int filter = (header >> 2) & 0x03;
        for (std::size_t i = 0; i < kBlockSamples; ++i) {
            const std::uint8_t byte = blocks[b + 1 + i / 2];
            const int          raw  = (i & 1) != 0 ? (byte & 0x0F) : (byte >> 4);
            const int          nib  = raw - ((raw & 0x08) != 0 ? 16 : 0);
            // A shift past twelve decodes as twelve over the nibble's sign alone, as the chip does.
            const int sample = shift > kMaxShift ? scaled(nib >> 3, kMaxShift) : scaled(nib, shift);
            const int value  = clampAndClip(filtered(filter, sample, history.old, history.older));
            history.older    = history.old;
            history.old      = value;
            out.push_back(static_cast<std::int16_t>(value * 2));
        }
    }
    return out;
}

std::vector<AudioFrame> decodeToFrames(std::span<const std::uint8_t> blocks, unsigned targetRate) {
    const std::vector<std::int16_t> mono = decode(blocks);
    std::vector<AudioFrame>         out;
    if (targetRate == kSampleRate || mono.size() < 2) {
        out.reserve(mono.size());
        for (const std::int16_t s : mono) {
            out.push_back(AudioFrame{s, s});
        }
        return out;
    }
    const std::size_t frames = static_cast<std::size_t>(
        static_cast<std::uint64_t>(mono.size()) * targetRate / kSampleRate);
    out.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double      pos  = static_cast<double>(i) * kSampleRate / targetRate;
        const std::size_t i0   = static_cast<std::size_t>(pos);
        const std::size_t i1   = std::min(i0 + 1, mono.size() - 1);
        const double      frac = pos - static_cast<double>(i0);
        const auto        s    = static_cast<std::int16_t>(std::lround(mono[i0] + (mono[i1] - mono[i0]) * frac));
        out.push_back(AudioFrame{s, s});
    }
    return out;
}

void requireWellFormed(std::span<const std::uint8_t> blocks) {
    if (blocks.empty() || blocks.size() % kBlockBytes != 0) {
        throw std::invalid_argument("a BRR sample is a nonzero multiple of nine bytes");
    }
    if ((blocks[blocks.size() - kBlockBytes] & kEndFlag) == 0) {
        throw std::invalid_argument("a BRR sample's last block must carry the END flag");
    }
}

std::vector<std::int16_t> toMono(std::span<const AudioFrame> frames) {
    std::vector<std::int16_t> out;
    out.reserve(frames.size());
    for (const AudioFrame& f : frames) {
        out.push_back(static_cast<std::int16_t>((static_cast<int>(f.left) + f.right) / 2));
    }
    return out;
}

}  // namespace retropp::audio::brr
