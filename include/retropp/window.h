#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "retropp/draw_state.h"  // Region — a drag handle is a drawn Region
#include "retropp/geometry.h"    // Vec2i, PixelSize
#include "retropp/input.h"       // InputSample, ActionLike / actionId — the trigger grammar

namespace retropp {

class Platform;

// The vector-capable input units an automatic window drag can read. A stick contributes its processed
// analog deflection, the d-pad its digital unit vector, the pointer its raw device delta — all
// window-independent quantities, so following them never feedback-jitters (moving the window does not
// change what they report, unlike a window-relative cursor position).
enum class MotionSource : std::uint8_t { Pointer, LeftStick, RightStick, Dpad };

// A game action by reference: any game enum (or integer id), converted at the surface exactly as every
// action-keyed API converts it. Exists so a designated-init aggregate can carry a `.trigger =
// Action::Grab` field without templating the aggregate itself. Default-constructed it refers to no
// action — the state a default WindowMovement carries.
struct ActionRef {
    ActionId id;

    constexpr ActionRef() noexcept : id(kMaxActions) {}  // no action
    template <ActionLike A>
    constexpr ActionRef(A a) noexcept : id(actionId(a)) {}
};

// The automatic-movement declaration — the game action that means "grab the window", and the input
// sources that drive the window while it is held:
//
//   MotionSource::Pointer     — the pointer's raw device delta, 1:1 (mouse hold-and-drag)
//   MotionSource::LeftStick / RightStick — the stick's deflection
//   MotionSource::Dpad        — the d-pad's unit vector
//
// `motion` is the COMPLETE set — specifying it replaces the default wholesale, so a set without
// Pointer means the mouse does not drive this drag (a stick-cursor interface names the stick the
// cursor is NOT on). WindowMovement::None is the OFF value: declaring it turns automatic movement
// off (the movement in effect is: none).
struct WindowMovement {
    ActionRef                 trigger = {};  // no action — the state WindowMovement::None carries
    std::vector<MotionSource> motion  = {MotionSource::Pointer, MotionSource::LeftStick,
                                         MotionSource::Dpad};

    static const WindowMovement None;  // no movement — declare it to turn automatic movement off
};

// The aggregate window declaration, submitted through platform.window(WindowState{...}). Every field
// is optional: an engaged field is applied through the matching Window setter, an omitted field is
// untouched.
struct WindowState {
    std::optional<Vec2i>               position;
    std::optional<PixelSize>           size;
    std::optional<bool>                fullscreen;
    std::optional<std::vector<Region>> dragHandles;
    std::optional<WindowMovement>      autoMove;
};

// The one window, as an object — returned by platform.window() (the platform is single-window by
// design; multiple windows are separate renderers). Noun setter/getter pairs; a setter is a no-op
// unless its value differs from the last one set through this surface, and it touches nothing else,
// so repeated calls never re-assert window state against a native drag or a user resize.
//
// Two complementary paths move the window, one declaration each:
//
//   • NATIVE — dragHandles({...}): a real mouse press inside any declared region hands the window to
//     the OS window manager, which performs the drag (pixel-perfect, zero per-frame work).
//   • AUTOMATIC — autoMove(movement): while the declared trigger action is held, the window follows
//     the declared motion sources (update() applies the per-frame delta). The path for gamepad-driven
//     interfaces, where no OS mouse press exists to hit-test.
//
// A drag handle IS a drawn Region: the game builds its title bar as a Region, draws it in its frame,
// and hands the SAME value here — painted and dragged, so the handle and the bar agree to the pixel
// by construction. Containment matches the drawn region exactly (the region gate's CPU mirror, curve
// boundaries included).
class Window {
public:
    // Registers this object as the platform's drag hit-test predicate; the platform's OS hit-test
    // asks it for every drag query. Owned by the Platform — one window per platform.
    explicit Window(Platform& platform);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    // The window's top-left corner in logical points, signed (a window can sit at negative
    // coordinates on a multi-monitor desktop). position(pos) places the window — center on launch,
    // snap, restore a saved position; position() reads where it actually is.
    [[nodiscard]] Vec2i position() const;
    void position(Vec2i pos);

    // The window's size in logical points. size(s) resizes it (the OS may clamp to its min/max);
    // size() reads the actual current size, user resizes included.
    [[nodiscard]] PixelSize size() const;
    void size(PixelSize size);

    // OS-native fullscreen. fullscreen(true) enters it, fullscreen(false) leaves it; fullscreen()
    // reads the actual current state.
    [[nodiscard]] bool fullscreen() const;
    void fullscreen(bool enabled);

    // Declare the drag handles: an array of drawn Regions; a mouse press inside any of them drags the
    // window via the OS. One call declares the whole set ({titleBar} — or {titleBar, dockTab} for
    // several handles) and replaces the previous set; an empty set clears it. The regions' shapes are
    // what the hit-test consults; their effects are the drawing side's concern and play no part here.
    void dragHandles(std::span<const Region> regions);
    void dragHandles(std::initializer_list<Region> regions) {
        dragHandles(std::span<const Region>{regions.begin(), regions.size()});
    }
    [[nodiscard]] std::span<const Region> dragHandles() const { return dragRegions_; }

    // Declare the automatic movement: the grab action + the motion sources that drive the window
    // while it is held. One declaration replaces the movement in effect. WindowMovement::None turns
    // automatic movement off — the getter reads none in effect, the same state as never declaring
    // (off is the default). See WindowMovement.
    void autoMove(WindowMovement movement) {
        if (movement.trigger.id >= kMaxActions) {
            movement_.reset();  // a no-trigger movement (None) can never fire — declared off
            return;
        }
        movement_ = std::move(movement);
    }
    [[nodiscard]] const std::optional<WindowMovement>& autoMove() const { return movement_; }

    // Whether a viewport point lies inside any declared drag handle — the predicate the platform's OS
    // hit-test asks (tested at the pixel's center, matching how the drawn region resolves it).
    [[nodiscard]] bool hitsDragRegion(Vec2i viewportPos) const;

    // Platform-internal: WindowedHost calls it once per frame with the fresh sample and the frame
    // period — evaluates the automatic-movement trigger and applies this frame's window motion (whole
    // points; the fractional remainder is banked so slow drags never stall). A game never calls it.
    void update(const InputSample& sample, std::chrono::nanoseconds frameTime);

private:
    static bool dragTestThunk(void* user, Vec2i viewportPos);  // the C-shaped predicate the seam stores

    Platform&                     platform_;
    std::vector<Region>           dragRegions_;  // the declared drag handles
    std::optional<WindowMovement> movement_;
    std::optional<Vec2i>          lastPosition_;    // last value set through position(Vec2i) — the diff base
    std::optional<PixelSize>      lastSize_;        // last value set through size(PixelSize)
    std::optional<bool>           lastFullscreen_;  // last value set through fullscreen(bool)
    Vec2                          moveRemainder_{};  // sub-point motion carried between frames
};

}  // namespace retropp
