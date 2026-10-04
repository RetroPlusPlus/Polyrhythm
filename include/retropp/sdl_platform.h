#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/analog_response.h"
#include "retropp/audio.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/platform.h"

namespace retropp {

namespace detail {
class SourceCapture;
struct SdlPlatformTestAccess;  // src/sdl_platform_testing.h — a platform with its window and input, no GPU device
}

// The production AudioSink: an SDL audio stream on the default playback device. start() opens the
// stream at the requested rate and begins draining the supplied pull on SDL's audio thread (the
// stream callback); stop() destroys it. Stereo signed-16 PCM (matching AudioFrame).
//
// Freely constructible — make one per AudioSystem (an AudioSystem drives one of these). Multiple
// sinks open multiple device streams that the OS mixes, so several AudioSystems (chiptune, PCM, …)
// play at once. Requires SDL audio initialised — SdlPlatform does that in its constructor (SDL_INIT_
// AUDIO); a game has a platform, so this is satisfied by the time it builds an AudioSystem.
class SdlAudioSink final : public AudioSink {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink() override;

    SdlAudioSink(const SdlAudioSink&)            = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    void start(unsigned rate, int channels, AudioPullFn pull) override;
    void stop() override;

private:
    // SDL's audio-thread callback: pull frames, silence-fill underflow, feed the stream.
    static void SDLCALL audioCallback(void* userdata, SDL_AudioStream* stream,
                                      int additionalAmount, int totalAmount);

    SDL_AudioStream* stream_ = nullptr;
    AudioPullFn      pull_;
};

// A connected gamepad as the game sees it: the SDL instance id (the handle for assignGamepad), the
// detected family (glyph selection / family-qualified rows), and the player slot it feeds.
struct GamepadInfo {
    SDL_JoystickID id;
    ControllerType family;
    int            slot;
};

// The production Platform: owns an SDL_Window, an SDL_GPUDevice, and the swapchain
// association. The constructor initialises SDL (video + gamepad + audio), creates the window
// (from the EngineConfig's WindowConfig), acquires the GPU device, and claims the window
// for it; the destructor releases them in reverse order. Single-threaded — every call
// runs on the platform thread.
//
// It owns the window, device, and input — not the drawing. Drawing is the Renderer's
// job: it takes device()/sdlWindow() and submits frames. The swapchain stays sized to the
// window, so drawableSize() reports the current physical size each frame for letterboxing.
//
// Input: the platform samples the game's ActionMap (actions) against every connected device
// each pump, producing the per-slot InputSample the host pushes into the run loop. Every device
// feeds player slot 0 by default; assignGamepad/assignKeyboard opt into multiplayer routing. The
// platform never filters what a game maps — an action is active whenever any of its bound sources
// is.
class SdlPlatform : public Platform {
public:
    // The canonical startup constructor: window from config.window. The default argument reads
    // EngineConfig::active — the set-once active config (seeded by EngineConfig::setActive(); see
    // engine_config.h) — so a bare `SdlPlatform platform;` inherits the host's configured window.
    // The default arg is evaluated at each call, so it reflects the current `active`.
    explicit SdlPlatform(const EngineConfig& config = EngineConfig::active);
    ~SdlPlatform() override;

    SdlPlatform(const SdlPlatform&)            = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    void pumpEvents() override;
    [[nodiscard]] bool quitRequested() const override { return quit_; }
    void clearQuitRequest() noexcept override { quit_ = false; }
    [[nodiscard]] const InputSample& input() const override { return sample_; }
    void pointerCaptured(bool captured) override;
    [[nodiscard]] bool pointerCaptured() const override { return pointerCaptured_; }
    void cursorVisible(bool visible) override;
    [[nodiscard]] bool cursorVisible() const override { return cursorVisible_; }
    [[nodiscard]] PixelSize drawableSize() const override;

    // The window size in logical points (SDL_Get/SetWindowSize) and the window's display's usable
    // area in logical points (SDL_GetDisplayUsableBounds) — together they let the output scaling
    // pick the largest window scale that fits the screen. See the Platform seam for units.
    [[nodiscard]] PixelSize windowSize() const override;
    void windowSize(PixelSize size) override;
    [[nodiscard]] PixelSize usableDisplaySize() const override;

    // Native fullscreen via SDL_SetWindowFullscreen with a NULL fullscreen-mode — SDL3's
    // borderless desktop fullscreen, which on macOS is a real fullscreen Space (the
    // platform-native idiom, not a fake borderless window). Does not make the window freely
    // resizable; the renderer's letterbox/integer-scale blit absorbs the new target size.
    void fullscreen(bool enabled) override;
    [[nodiscard]] bool fullscreen() const override { return fullscreen_; }

    // Suppress / restore the native OS window chrome at runtime via SDL_SetWindowBordered (bordered =
    // !suppress), tracking the state on success only (mirrors fullscreen). The startup config
    // (WindowConfig::suppressNativeWindowChrome) seeds both the borderless window-creation flag and
    // this tracked state. See the Platform seam for the contract.
    void suppressNativeWindowChrome(bool suppress) override;
    [[nodiscard]] bool suppressNativeWindowChrome() const override { return chromeSuppressed_; }

    // Window placement via SDL_Get/SetWindowPosition (logical points). See the Platform seam for the
    // contract.
    [[nodiscard]] Vec2i windowPosition() const override;
    void windowPosition(Vec2i pos) override;

    // Frame pacing: monotonic time (SDL_GetTicksNS), the live display refresh period
    // (SDL_GetCurrentDisplayMode, 60 Hz fallback), and a precise sleep (SDL_DelayPrecise, guarded
    // > 0). See the Platform seam for the contract; the host's deadline arithmetic lives in pacing.h.
    [[nodiscard]] std::chrono::nanoseconds nowMonotonic() const override;
    [[nodiscard]] std::chrono::nanoseconds displayRefreshPeriod() const override;
    void sleepPrecise(std::chrono::nanoseconds duration) override;

    // The live GPU device + window the renderer draws with. Exposed (rather than hidden
    // behind a present method) because the renderer is a separate object that owns the
    // pipeline/viewport and submits frames against this device. SDL types appear here by
    // design (this is the SDL platform).
    [[nodiscard]] SDL_GPUDevice* device() const noexcept { return gpu_; }
    [[nodiscard]] SDL_Window*    sdlWindow() const noexcept { return window_; }

    // The live action bindings: the platform samples whatever value was last handed here, so a game
    // replaces its whole input scheme by editing its own copy and resubmitting it (a rebind screen,
    // a gameplay/menu context switch, a map loaded from a save). Takes effect at the next pump.
    // With no call the map is empty and no actions are ever reported.
    void actions(const ActionMap& map) { actions_ = map; }
    [[nodiscard]] const ActionMap& actions() const noexcept { return actions_; }

    // The live analog processing: dead-zone + stick gate per input (see analog_response.h). Handed here
    // like the action map — the platform samples whatever was last submitted, so a game changes stick
    // feel (a settings screen, a per-context switch) by resubmitting. With no call the defaults apply
    // (Radial dead-zone, no gate). Takes effect at the next pump. One noun, both directions: the
    // AnalogResponse overload submits, the argument-free overload reads back what is live.
    void analogResponse(const AnalogResponse& response) { analogResponse_ = response; }
    [[nodiscard]] const AnalogResponse& analogResponse() const noexcept { return analogResponse_; }

    // Device → player-slot routing. Default: everything feeds slot 0 (single-player games never
    // touch this). assignGamepad routes one connected pad (by its SDL instance id, from
    // connectedGamepads) to a slot; assignKeyboard routes the keyboard+mouse unit. Out-of-range
    // slots clamp into [0, kMaxPlayers). Routing is runtime device state on this platform object —
    // a reconnected pad re-enters at slot 0.
    void assignGamepad(SDL_JoystickID id, int player);
    void assignKeyboard(int player);
    [[nodiscard]] std::vector<GamepadInfo> connectedGamepads() const;

    // Capturing a pressed source — how a controls screen learns which physical input the player
    // presses, bound or not. captureRequest arms a slot and clears its answer; the first press on
    // that slot's devices after the request becomes the answer, and capturedSource reads it — the
    // same value on every call until the slot's next request, empty before a request and while it
    // waits. A press is a down event: a key (repeats are not presses), the left, right or middle
    // mouse button, a pad button by position (padButtonFrom), or a trigger or stick direction rising
    // past its default digital threshold after the live AnalogResponse. Anything already down at the
    // request is not a press. A captured press still drives every action it is bound to. Slots clamp
    // into [0, kMaxPlayers) and capture independently; a key or mouse press belongs to the keyboard's
    // slot, a pad press to the pad's slot.
    void captureRequest(int player = 0);
    [[nodiscard]] std::optional<CapturedSource> capturedSource(int player = 0) const noexcept;

protected:
    // Issue a CHANGED motor state to every pad routed to `player`. Only pads whose SDL rumble / trigger-
    // rumble capability is present are driven (a slot with no capable pad no-ops). uint8 0–255 → SDL
    // Uint16 via × 257; a held value uses an effectively-infinite duration so it does not time out
    // between changes (the flush re-issues on the next diff). See platform.h flushVibration for the diff.
    void emitVibration(int player, const MotorLevels& levels) noexcept override;

private:
    struct OpenPad {
        SDL_Gamepad*   handle;
        SDL_JoystickID id;
        ControllerType family;
        int            slot;
    };

    // What the platform brings up: everything, or the window and the input alone (no GPU device, no
    // audio) for the internal seam in src/sdl_platform_testing.h.
    enum class Devices : std::uint8_t { All, InputOnly };
    SdlPlatform(const EngineConfig& config, Devices devices);
    friend struct detail::SdlPlatformTestAccess;

    void openGamepad(SDL_JoystickID id);
    void closeGamepad(SDL_JoystickID id);
    void buildSample();

    // Hand one event to the capture: a key or mouse button down, or a pad's button down or axis motion
    // with that pad's current readings. Runs only while a slot waits for a press.
    void forwardToCapture(const SDL_Event& event);

    // The OS draggable-region hit-test, installed once at construction with `this` as the userdata. SDL
    // calls it during event processing with a window-space point; it maps that point → viewport (the same
    // blit inversion the cursor uses) and asks the seam's registered dragTest predicate (dragHit) —
    // DRAGGABLE on a hit, NORMAL otherwise. The predicate registration and the OS query both run on the
    // platform thread, so no locking.
    static SDL_HitTestResult SDLCALL hitTest(SDL_Window* window, const SDL_Point* area, void* data);
    void sampleSlot(int slot, const bool* keys,
                    const std::array<std::uint8_t, kMaxActions>& qualifiedMasks);

    SDL_Window*    window_  = nullptr;
    SDL_GPUDevice* gpu_     = nullptr;
    PixelSize      viewport_;           // internal render size — inverts the blit for the cursor map

    ActionMap            actions_;      // the last-submitted map (a replaceable copy; game owns its value)
    AnalogResponse       analogResponse_;  // the last-submitted analog processing (dead-zone + gate)
    std::vector<OpenPad> pads_;         // every connected pad, each routed to a slot
    int                  keyboardSlot_ = 0;  // the slot the keyboard+mouse unit feeds
    InputSample          sample_;       // rebuilt by pumpEvents; served by input()
    std::unique_ptr<detail::SourceCapture> capture_;  // captureRequest / capturedSource

    // ── Per-pump device-activity flags (drive the active-device signal) ──
    bool kbActivityThisPump_ = false;
    std::array<bool, kMaxPlayers>           padActivityThisPump_{};
    std::array<ControllerType, kMaxPlayers> padActivityFamily_{};

    // ── Pointer state (rebuilt by pumpEvents; folded into the keyboard slot's analog) ──
    float frameRawDX_ = 0.0f;   // raw device motion accumulated within THIS pump (reset each pump)
    float frameRawDY_ = 0.0f;
    float frameWheel_ = 0.0f;   // wheel accumulated within this pump
    float mouseWinX_  = 0.0f;   // latest pointer position, window LOGICAL points
    float mouseWinY_  = 0.0f;
    std::uint8_t mouseHeld_ = 0;        // level mask, bit per MouseButton
    bool pointerCaptured_   = false;    // relative (spinner / mouse-look) mode
    bool cursorVisible_     = true;     // host-OS cursor shown (independent of capture)
    bool fullscreen_ = false;  // current fullscreen state (seeded from config at construction)
    bool chromeSuppressed_ = false;  // native OS chrome suppressed (seeded from config at construction)
    bool quit_       = false;
};

}  // namespace retropp
