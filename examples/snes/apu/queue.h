// The hand-off to the device.
//
// The chip's frames are produced on the game's thread, from inside AudioUnit::advance(); the device takes
// them on SDL's audio thread, whenever it asks. Between the two sits a queue: the game's thread pushes
// each frame as the chip makes it, the audio thread pops what the device needs. One writer and one
// reader, so two atomic counters are the whole synchronization. A full queue drops the frame — the
// machine is paced by its own clock, not by the device.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <span>

#include "retropp/audio.h"  // AudioFrame

namespace demo {

class FrameQueue {
public:
    static constexpr std::size_t kCapacity = 8192;  // about 170 ms at 48'000 Hz

    bool push(retropp::AudioFrame frame) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        if (w - r == kCapacity) {
            return false;
        }
        frames_[w % kCapacity] = frame;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    std::size_t pop(std::span<retropp::AudioFrame> out) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t n = std::min(out.size(), w - r);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = frames_[(r + i) % kCapacity];
        }
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::array<retropp::AudioFrame, kCapacity> frames_{};
    std::atomic<std::size_t>                   write_{0};
    std::atomic<std::size_t>                   read_{0};
};

}  // namespace demo
