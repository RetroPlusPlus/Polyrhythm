#pragma once

#include <array>
#include <chrono>

#include "retropp/analog_input.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/vibration.h"  // MotorLevels — the gamepad output value type
#include "retropp/window.h"     // Window + WindowState — the window surface platform.window() exposes

namespace retropp {

class Platform;

// A per-player gamepad OUTPUT handle. Obtained from platform.gamepad(player), it is a cheap value that
// forwards the declared state to the platform. Vibration is the channel today; LED color and adaptive
// triggers can join it on the same handle. Input and output are named differently on purpose:
// input.player(n) reads device-agnostically, gamepad(n) sends to a specific gamepad (a keyboard has
// nothing to vibrate, so the handle for a non-gamepad slot no-ops).
class GamepadOutput {
public:
    // Declare THIS tick's complete motor state for the pad — strictly declarative and immediate, the
    // same declarative model as renderer.renderFrame(FrameDrawState): resubmit every tick; a tick with
    // no call is silence. The platform diffs the declaration against the last value it flushed to the
    // device and touches the device only on a change (see Platform::flushVibration). A slot with no pad no-ops.
    void vibration(const MotorLevels& levels) const noexcept;  // defined below (needs Platform complete)

private:
    friend class Platform;
    constexpr GamepadOutput(Platform* platform, int player) noexcept
        : platform_(platform), player_(player) {}
    Platform* platform_;
    int       player_;
};

// The host-OS boundary: window + GPU present + input + lifecycle, expressed as an
// abstract seam so the platform's scheduling and input-translation logic never depends
// on a live device. The production implementation is SdlPlatform; tests drive a
// MockPlatform, keeping the windowed-host driver verifiable headlessly. It is to the
// platform what the run loop's injectable Clock is to time — the same seam discipline.
//
// No hardware-register or scanline idioms cross this boundary: input is one sampled
// InputSample (per-slot action state + analog/pointer surface), and a frame is
// presented whole. There are no per-line, per-register, or mid-frame hooks.
class Platform {
public:
    virtual ~Platform() = default;

    // Drain the OS event queue: refresh the input sample and latch a quit request
    // (window-close button or OS quit). Called once per host iteration.
    virtual void pumpEvents() = 0;

    // True once the user has asked to close the window (the OS window-close button / OS quit). A
    // one-shot latch: it becomes true when the close event arrives and stays true until cleared. The
    // windowed host routes it through the run loop's exit guard rather than stopping directly.
    [[nodiscard]] virtual bool quitRequested() const = 0;

    // Clear the quit latch. The windowed host calls this when the exit guard VETOES a window-close, so
    // quitRequested() stops reporting the (already-answered) close — otherwise it would re-raise the
    // exit every frame and the guard could never be answered "No". A backend whose latch is not set
    // no-ops.
    virtual void clearQuitRequest() noexcept = 0;

    // The input sample as of the most recent pumpEvents(): per player slot, the active action set,
    // per-action values, the analog/pointer surface, and the active-device signal. Cursor is in
    // VIEWPORT pixels (the platform inverts its own letterbox/integer-scale blit so the coordinate
    // matches what is drawn); the relative quantities (rawDelta, wheel) are this FRAME's accumulated
    // motion, which the run loop sums between ticks.
    [[nodiscard]] virtual const InputSample& input() const = 0;

    // Enter / leave relative-pointer (capture) mode: the OS cursor is hidden and confined, and motion
    // arrives as unbounded relative deltas (rawDeltaX/Y) — the authentic rotary-spinner / mouse-look
    // feel. While captured there is no meaningful absolute cursor. A game toggles this per context (on
    // for a spinner level, off for a menu). Host-OS-agnostic; a backend without it no-ops.
    virtual void pointerCaptured(bool captured) = 0;

    // Whether the pointer is currently captured (relative mode).
    [[nodiscard]] virtual bool pointerCaptured() const = 0;

    // Show or hide the host-OS hardware cursor — INDEPENDENTLY of pointer capture. A game that draws
    // its own cursor (a reticle, a paddle the mouse drives) hides the OS arrow while keeping absolute
    // cursor tracking live: analog().cursor and cursorOnScreen still update. This is distinct from
    // pointerCaptured, which hides AND confines the cursor and switches motion to relative-only —
    // the two are orthogonal knobs (a game may hide the cursor without capturing, or capture without
    // caring about visibility). The OS cursor starts visible. Host-OS-agnostic; a backend without a
    // cursor no-ops.
    virtual void cursorVisible(bool visible) = 0;

    // Whether the host-OS cursor is currently shown (the explicit cursorVisible state, independent
    // of capture).
    [[nodiscard]] virtual bool cursorVisible() const = 0;

    // The window's current drawable size in physical pixels. The renderer reads this
    // each frame to letterbox the internal viewport into the window; it tracks window
    // resizes (the swapchain is kept sized to the window by the platform). On-screen
    // output itself is the renderer's job — the platform owns the window/device/input,
    // not the drawing.
    [[nodiscard]] virtual PixelSize drawableSize() const = 0;

    // Seam primitive behind Window::size — the window's size in LOGICAL points. Logical points (not
    // physical pixels) so the perceived size is the same on any display density; the drawable the
    // renderer fills is this × the display's pixel density. The OS may clamp a resize to its min/max
    // window size; the read reports the realized size. The developer surface is window().size().
    // Host-OS-agnostic — a backend with no resizable window reports its fixed size and no-ops the
    // resize.
    [[nodiscard]] virtual PixelSize windowSize() const = 0;
    virtual void windowSize(PixelSize size) = 0;

    // The usable area of the display the window is on, in LOGICAL points (the desktop work area minus
    // OS chrome — menu bar / dock / taskbar). Same units as windowSize, so the scaling logic can
    // pick the largest window scale that fits the screen and never overflow it (see fitWindowScale in
    // geometry.h). A backend without a queryable display returns a safe fallback.
    [[nodiscard]] virtual PixelSize usableDisplaySize() const = 0;

    // Seam primitive behind Window::fullscreen — OS-native fullscreen. The production platform uses
    // the host's real fullscreen affordance (on macOS a fullscreen Space, not a fake borderless
    // window). Fullscreen does NOT make the window freely resizable; the existing letterbox /
    // integer-scale blit handles the new target size. The developer surface is window().fullscreen().
    // Host-OS-agnostic — a future touch/mobile backend implements it per its OS or no-ops.
    virtual void fullscreen(bool enabled) = 0;

    // Whether the platform is currently in fullscreen.
    [[nodiscard]] virtual bool fullscreen() const = 0;

    // Suppress (or restore) the native OS window chrome — the title bar / border / decorations the
    // operating system draws around the window. For an app that draws its own chrome (a custom
    // draggable title bar), true removes the OS decorations that would otherwise stack underneath it.
    // The primary path is the startup config (WindowConfig::suppressNativeWindowChrome), which creates
    // the window borderless from the first frame so the native chrome never flashes; this runtime knob
    // flips it live afterward. Noun-submit: the argument overload submits, the argument-free overload
    // reads the current state. Host-OS-agnostic — a backend without decorations no-ops. The production
    // platform uses SDL_SetWindowBordered.
    virtual void suppressNativeWindowChrome(bool suppress) = 0;
    [[nodiscard]] virtual bool suppressNativeWindowChrome() const = 0;

    // ── The window surface ────────────────────────────────────────────────────────
    // The one window, as an object: noun setter/getter pairs for position/size/fullscreen, the drawn
    // drag handles, and the automatic movement. See window.h for the full surface.
    [[nodiscard]] Window& window() noexcept { return window_; }

    // The aggregate window declaration: applies exactly the engaged fields of `s` through the same
    // Window setters; an omitted field is untouched.
    void window(const WindowState& s) {
        if (s.position) window_.position(*s.position);
        if (s.size) window_.size(*s.size);
        if (s.fullscreen) window_.fullscreen(*s.fullscreen);
        if (s.dragHandles) {
            window_.dragHandles(std::span<const Region>{s.dragHandles->data(), s.dragHandles->size()});
        }
        if (s.autoMove) window_.autoMove(*s.autoMove);
    }

    // Seam primitive behind Window::position — the window's on-screen position, its top-left corner,
    // in LOGICAL points (signed, because a window can sit at negative coordinates on a multi-monitor
    // desktop). windowPosition(pos) places the window, handed to the OS as given; windowPosition()
    // reads it. The developer surface is window().position(). Host-OS-agnostic — a backend with no
    // positionable window reports the origin and no-ops the move.
    [[nodiscard]] virtual Vec2i windowPosition() const = 0;
    virtual void windowPosition(Vec2i pos) = 0;

    // ── Drag hit-test ─────────────────────────────────────────────────────────────
    // The predicate behind OS-native window dragging: "does this VIEWPORT point sit in a drag region?"
    // Window registers its shape containment here; the production platform's OS hit-test callback
    // maps an incoming window point → viewport (the same inversion the cursor uses) and asks dragHit(),
    // handing the window to the OS window manager on a hit. The shapes themselves are draw-state
    // vocabulary and never cross this seam — only the predicate does. One stored implementation on the
    // base (the flushVibration model); no predicate registered (the default) = never a hit = an
    // ordinary window.
    using DragTest = bool (*)(void* user, Vec2i viewportPos);
    void dragTest(DragTest test, void* user) noexcept {
        dragTest_     = test;
        dragTestUser_ = user;
    }

    // Run the registered predicate against a viewport point (false when none is registered). The
    // production platform's OS hit-test calls this; a test calls it to simulate an OS drag query.
    [[nodiscard]] bool dragHit(Vec2i viewportPos) const {
        return dragTest_ != nullptr && dragTest_(dragTestUser_, viewportPos);
    }

    // ── Frame pacing ────────────────────────────────────────────────────────────
    // The OS-coupled primitives the windowed host uses to pace each iteration to a monotonic frame
    // deadline (the deadline arithmetic itself is pure — see pacing.h). They let the host enforce the
    // display cadence directly, independent of whether the vsync present blocks (which not every
    // platform reliably does while the window is idle).

    // Current monotonic time. Distinct from RunLoop's injected Clock (private to the loop, drives sim
    // ticks) — the host needs its own read, in the SAME clock domain as sleepPrecise(), to compute the
    // remainder to the next frame deadline. A backend with no clock returns a monotonically increasing
    // value of its choosing (tests inject a controllable one).
    [[nodiscard]] virtual std::chrono::nanoseconds nowMonotonic() const = 0;

    // The refresh period (1 / refresh_rate, in ns) of the display the window is on, or a safe 60 Hz
    // fallback when the rate is unavailable. The host paces to this so the loop runs at the monitor's
    // cadence. Queried live each iteration (one trivial call per frame) so a window dragged to a
    // different-refresh display re-paces with no event handling.
    [[nodiscard]] virtual std::chrono::nanoseconds displayRefreshPeriod() const = 0;

    // Precise-sleep the calling thread for `duration` (no-op if <= 0). Called once per host iteration,
    // after the frame is presented, for the host's computed remainder-to-deadline. The production
    // platform uses a high-resolution sleep; a test platform records the request and does not wait, so
    // the host suite stays instant and deterministic.
    virtual void sleepPrecise(std::chrono::nanoseconds duration) = 0;

    // ── Gamepad output ──────────────────────────────────────────────────────────
    // Output to the pad. Input is read via input.player(n); output is declared through this
    // handle. Everything below is device-independent: the per-slot diff lives here so it has ONE
    // implementation, and only emitVibration() — the change that actually reaches the device — is
    // backend-specific (SdlPlatform issues the SDL rumble; a backend without haptics no-ops).

    // The per-player gamepad output handle (vibration today; LED / adaptive-trigger channels later).
    // Slot 0 is the default, so single-player games write gamepad().vibration(...). The index clamps
    // into [0, kMaxPlayers).
    [[nodiscard]] GamepadOutput gamepad(int player = 0) noexcept {
        const int i = player < 0 ? 0 : (player >= kMaxPlayers ? kMaxPlayers - 1 : player);
        return GamepadOutput{this, i};
    }

    // Platform-internal (the windowed host calls it after each advance() that ran ≥ 1 tick): reconcile
    // each slot's declared motor state against the last value flushed to its device and emit ONLY the
    // changes, then clear the per-frame declaration so a later frame with no vibration() call reads as
    // silence. A constant rumble declared every tick therefore costs one emit on the tick it changed,
    // then zero device traffic until it changes again. A ZERO-tick host frame must NOT call this — the
    // game had no tick to declare in, so a held rumble must survive rather than be reset to silence.
    void flushVibration() noexcept {
        for (int i = 0; i < kMaxPlayers; ++i) {
            const auto s = static_cast<std::size_t>(i);
            if (declaredVibration_[s] != lastFlushedVibration_[s]) {
                emitVibration(i, declaredVibration_[s]);
                lastFlushedVibration_[s] = declaredVibration_[s];
            }
            declaredVibration_[s] = MotorLevels{};  // next frame starts silent; re-declared each tick that rumbles
        }
    }

protected:
    // The game's per-tick declaration for a slot, routed here from GamepadOutput::vibration. Stored,
    // not emitted, so a multi-tick host frame lets the last tick's declaration win and flushVibration
    // diffs it. GamepadOutput is the only caller — the surface is gamepad(player).vibration(...).
    void declareVibration(int player, const MotorLevels& levels) noexcept {
        declaredVibration_[static_cast<std::size_t>(player)] = levels;
    }

    // Emit a CHANGED motor state to the actual device — the one imperative hook, called by
    // flushVibration only on a diff (never for an unchanged value, so an implementation need not diff).
    // Default no-op so a backend without haptics silently ignores it; SdlPlatform issues the SDL
    // rumble; a test platform records the flushed value.
    virtual void emitVibration(int player, const MotorLevels& levels) noexcept {
        (void)player;
        (void)levels;
    }

private:
    friend class GamepadOutput;
    std::array<MotorLevels, kMaxPlayers> declaredVibration_{};     // this frame's per-slot declaration (reset at flush)
    std::array<MotorLevels, kMaxPlayers> lastFlushedVibration_{};  // last value emitted per slot (the diff base)
    DragTest dragTest_     = nullptr;  // the registered drag predicate (Window's shape containment)
    void*    dragTestUser_ = nullptr;  // its context pointer, handed back on every query
    Window   window_{*this};  // the one window; declared after the drag seam it registers into
};

// Defined here — GamepadOutput must be complete for gamepad() above, and Platform must be complete for
// this forward to declareVibration (a protected member GamepadOutput reaches as a friend).
inline void GamepadOutput::vibration(const MotorLevels& levels) const noexcept {
    platform_->declareVibration(player_, levels);
}

}  // namespace retropp
