#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "retropp/audio.h"
#include "retropp/input.h"
#include "retropp/platform.h"

namespace retropp::test {

// A headless AudioSink for the device-free suite: it opens no device. start() records the rate /
// channels / pull the AudioSystem hands it; drain() invokes that pull on demand (standing in for the
// audio thread), so a test can observe exactly what the chain produced without a sound device.
class CaptureAudioSink final : public AudioSink {
public:
    void start(unsigned rate, int channels, AudioPullFn pull) override {
        rate_ = rate;
        channels_ = channels;
        pull_ = std::move(pull);
        started_ = true;
    }
    void stop() override {
        started_ = false;
        pull_ = nullptr;
    }

    // Pull up to `n` frames through the stored pull, exactly as the audio thread would; returns the
    // frames actually produced (a short vector means the chain underflowed).
    std::vector<AudioFrame> drain(std::size_t n) {
        std::vector<AudioFrame> out(n);
        const std::size_t got = pull_ ? pull_(std::span<AudioFrame>(out.data(), out.size())) : 0;
        out.resize(got);
        return out;
    }

    [[nodiscard]] unsigned rate() const noexcept { return rate_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    AudioPullFn pull_;
    unsigned    rate_ = 0;
    int         channels_ = 0;
    bool        started_ = false;
};

// A headless Platform stand-in for the windowed-host suite. It opens no window and
// touches no device: it reports a scripted InputSample and drawable size, latches
// quit after a set number of pumps, counts pumps, and runs an optional per-pump hook
// (which can advance an injected clock so the run loop ticks deterministically). The
// per-slot setters default to slot 0, so single-player tests read like their subject.
class MockPlatform final : public Platform {
public:
    explicit MockPlatform(int quitAfterPumps) noexcept
        : quitAfter_(quitAfterPumps), quit_(quitAfterPumps <= 0) {}

    void setHeld(ActionSet held, int player = 0) noexcept {
        sample_.players[static_cast<std::size_t>(player)].held = held;
    }
    void setAnalog(const AnalogInput& a, int player = 0) noexcept {
        sample_.players[static_cast<std::size_t>(player)].analog = a;
    }
    void setValue(ActionId action, Vec2 value, int player = 0) noexcept {
        sample_.players[static_cast<std::size_t>(player)].values[action] = value;
    }
    void setActiveDevice(ActiveDevice device, int player = 0) noexcept {
        sample_.players[static_cast<std::size_t>(player)].device = device;
    }
    void setSample(const InputSample& sample) noexcept { sample_ = sample; }
    void setOnPump(std::function<void()> fn) { onPump_ = std::move(fn); }
    void setDrawableSize(PixelSize size) noexcept { drawable_ = size; }
    void setUsableDisplaySize(PixelSize size) noexcept { usable_ = size; }

    void pumpEvents() override {
        ++pumpCount_;
        if (onPump_) onPump_();
        // Latch quit once, at the threshold pump — a one-shot quit matching a real OS QUIT event posted
        // once, so clearQuitRequest() sticks (a level test would re-latch every later pump).
        if (pumpCount_ == quitAfter_) quit_ = true;
    }
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    void clearQuitRequest() noexcept override { quit_ = false; }

    // Latch a quit explicitly, decoupled from the pump threshold — lets a test re-trigger an OS close
    // after an earlier one was vetoed (drive it from onPump), modeling a second window-close event.
    void requestQuit() noexcept { quit_ = true; }
    [[nodiscard]] const InputSample& input() const override { return sample_; }
    void pointerCaptured(bool captured) override { pointerCaptured_ = captured; }
    [[nodiscard]] bool pointerCaptured() const override { return pointerCaptured_; }
    void cursorVisible(bool visible) override { cursorVisible_ = visible; }
    [[nodiscard]] bool cursorVisible() const override { return cursorVisible_; }
    [[nodiscard]] PixelSize drawableSize() const override { return drawable_; }

    // Headless window sizing: track the requested logical size and reflect it as the drawable
    // (density 1, so logical == physical) so tests can observe a resize through drawableSize().
    [[nodiscard]] PixelSize windowSize() const override { return drawable_; }
    void windowSize(PixelSize size) override { drawable_ = size; }
    [[nodiscard]] PixelSize usableDisplaySize() const override { return usable_; }

    // Headless fullscreen: just track the requested state (no window to toggle).
    void fullscreen(bool enabled) override { fullscreen_ = enabled; }
    [[nodiscard]] bool fullscreen() const override { return fullscreen_; }

    // Headless native-chrome suppression: track the requested state (no window to decorate).
    void suppressNativeWindowChrome(bool suppress) override { chromeSuppressed_ = suppress; }
    [[nodiscard]] bool suppressNativeWindowChrome() const override { return chromeSuppressed_; }

    // Headless window placement: store and reflect the requested position (no window to move). The
    // drag hit-test needs no stub — the predicate registration and dragHit() live on the base Platform;
    // a test calls dragHit(viewportPos) directly to simulate an OS drag query.
    void windowPosition(Vec2i pos) override { position_ = pos; }
    [[nodiscard]] Vec2i windowPosition() const override { return position_; }

    // ── Frame pacing ──
    // Deterministic, device-free stand-ins for the pacing seam. nowMonotonic returns a controllable
    // clock; displayRefreshPeriod is settable (default 60 Hz); sleepPrecise never waits — it records the
    // request (count + last + total) and advances now_ by the slept amount, so a multi-iteration host
    // test progresses self-consistently (each iteration's sleep moves the mock clock toward the deadline).
    void setNow(std::chrono::nanoseconds now) noexcept { now_ = now; }
    void advanceNow(std::chrono::nanoseconds by) noexcept { now_ += by; }
    void setRefreshPeriod(std::chrono::nanoseconds period) noexcept { refreshPeriod_ = period; }

    [[nodiscard]] std::chrono::nanoseconds nowMonotonic() const override { return now_; }
    [[nodiscard]] std::chrono::nanoseconds displayRefreshPeriod() const override { return refreshPeriod_; }
    void sleepPrecise(std::chrono::nanoseconds duration) override {
        ++sleepCount_;
        lastSleep_ = duration;
        if (duration > std::chrono::nanoseconds::zero()) {
            totalSlept_ += duration;
            now_        += duration;  // simulate time passing without actually waiting
        }
    }

    [[nodiscard]] int pumpCount() const noexcept { return pumpCount_; }
    [[nodiscard]] int sleepCount() const noexcept { return sleepCount_; }
    [[nodiscard]] std::chrono::nanoseconds lastSleep() const noexcept { return lastSleep_; }
    [[nodiscard]] std::chrono::nanoseconds totalSlept() const noexcept { return totalSlept_; }

    // ── Vibration output capture ──
    // Records the CHANGED motor states the base Platform's flush emits (post-diff), so a test observes
    // exactly what would reach the device. Mark a slot absent to model a slot with no pad — its
    // emissions are dropped (SdlPlatform's no-pad no-op), so nothing is recorded for it.
    struct FlushedVibration {
        int         player;
        MotorLevels levels;
    };
    void setGamepadPresent(int player, bool present) noexcept {
        padPresent_[static_cast<std::size_t>(player)] = present;
    }
    [[nodiscard]] const std::vector<FlushedVibration>& flushedVibrations() const noexcept {
        return flushedVibrations_;
    }

protected:
    void emitVibration(int player, const MotorLevels& levels) noexcept override {
        if (!padPresent_[static_cast<std::size_t>(player)]) return;  // no pad on this slot → nothing reaches a device
        flushedVibrations_.push_back(FlushedVibration{player, levels});
    }

private:
    int  quitAfter_;
    bool quit_;
    int  pumpCount_ = 0;
    bool fullscreen_ = false;
    bool chromeSuppressed_ = false;  // native OS chrome shown by default (matches SdlPlatform)
    bool pointerCaptured_ = false;
    bool cursorVisible_ = true;   // host-OS cursor shown by default (matches SdlPlatform)
    Vec2i position_{};            // headless window position (0,0 default)
    InputSample sample_;
    PixelSize drawable_{640, 576};   // 4× the GB viewport by default
    PixelSize usable_{4096, 4096};   // a roomy default "display" for headless scale-fit tests
    std::function<void()> onPump_;

    // Pacing seam state.
    std::chrono::nanoseconds now_{};                       // controllable monotonic clock
    std::chrono::nanoseconds refreshPeriod_{16'666'667};   // 60 Hz default display refresh
    std::chrono::nanoseconds lastSleep_{};                 // most recent sleepPrecise request
    std::chrono::nanoseconds totalSlept_{};                // sum of positive sleeps
    int sleepCount_ = 0;                                   // number of sleepPrecise calls

    // Vibration capture: which slots have a pad (default all present) + the flushed motor states.
    std::array<bool, kMaxPlayers> padPresent_{{true, true, true, true}};
    std::vector<FlushedVibration> flushedVibrations_;
};

}  // namespace retropp::test
