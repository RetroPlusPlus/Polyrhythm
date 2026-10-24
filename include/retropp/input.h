#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "retropp/analog_input.h"
#include "retropp/geometry.h"

namespace retropp {

// ── The action model ────────────────────────────────────────────────────────────────────────────
//
// A game defines its own input vocabulary as an enum — Jump, Fire, Move, whatever the game means —
// and binds each action to any number of physical sources in an ActionMap (input_actions.h). The
// platform translates devices into actions per player slot each pump; this header is the std-only
// half the game reads from: the per-tick InputState keyed by the game's own enum. The platform holds
// no action vocabulary of its own and never filters what a game may map — unmapped input is
// simply silent.
//
// This header is deliberately SDL-free (the run loop includes it); everything SDL-facing — sources,
// the map, pad vocabulary — lives in input_actions.h.

// The wire form of an action: the bit index into ActionSet. Games never handle these directly —
// every game-facing API is templated on the game's enum and casts here at the surface.
using ActionId = std::uint8_t;

// Capacity, not a filter: 64 simultaneous digital/valued actions per map. Widening is additive (a
// second word) if a game ever needs more.
inline constexpr int kMaxActions = 64;

// Player slots. Every device feeds slot 0 by default, so single-player games never see slots; a
// game opts in by assigning devices to slots on the platform and reading player(n).
inline constexpr int kMaxPlayers = 4;

// The constraint every action-keyed API places on its parameter: the game's own enum (or a plain
// integer id). The cast to ActionId happens at the API surface, never inside the platform.
template <typename A>
concept ActionLike = std::is_enum_v<A> || std::is_integral_v<A>;

template <ActionLike A>
[[nodiscard]] constexpr ActionId actionId(A a) noexcept {
    return static_cast<ActionId>(a);
}

// The actions currently active, packed one bit per ActionId. A value type — cheap to copy, compare,
// and snapshot.
class ActionSet {
public:
    constexpr ActionSet() noexcept = default;

    constexpr void set(ActionId a, bool active) noexcept {
        const std::uint64_t mask = std::uint64_t{1} << a;
        bits_ = active ? (bits_ | mask) : (bits_ & ~mask);
    }

    [[nodiscard]] constexpr bool test(ActionId a) const noexcept {
        return (bits_ & (std::uint64_t{1} << a)) != 0;
    }

    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return bits_; }

    // Union: every action active in EITHER set. The run loop ORs each host frame's held state into a
    // per-tick accumulator (heldUnion), so an action that was down at ANY point between two ticks is
    // seen by the tick — a press is never dropped just because it was released before the tick
    // sampled it (see run_loop.h's per-tick sampling).
    constexpr ActionSet& operator|=(ActionSet other) noexcept {
        bits_ |= other.bits_;
        return *this;
    }
    [[nodiscard]] friend constexpr ActionSet operator|(ActionSet a, ActionSet b) noexcept {
        return a |= b;
    }

    friend constexpr bool operator==(ActionSet, ActionSet) noexcept = default;

private:
    std::uint64_t bits_ = 0;  // one bit per ActionId, 0..kMaxActions-1
};

// The detected physical-controller family. SDL normalizes button POSITIONS across families, so this
// is not needed for input correctness — it drives button-glyph / prompt selection (via the
// active-device signal below) and the per-family resolution of labeled pad sources
// (input_actions.h). "Standard" is SDL's generic well-mapped pad; "Unknown" is no pad /
// unrecognized.
enum class ControllerType : std::uint8_t { Unknown, Xbox, PlayStation, Nintendo, Standard };

// Which kind of device most recently produced input for a player slot.
enum class DeviceKind : std::uint8_t { None, KeyboardMouse, Gamepad };

// The per-slot active-device signal: the glyph layer reads this to flip "Press Ⓐ" / "Press [E]".
// `family` is meaningful when kind == Gamepad (the family of the pad that produced the input).
struct ActiveDevice {
    DeviceKind     kind   = DeviceKind::None;
    ControllerType family = ControllerType::Unknown;

    friend constexpr bool operator==(ActiveDevice, ActiveDevice) noexcept = default;
};

// One player slot's share of a platform sample: the digital action level, the per-action analog
// values (vector reads — sticks contribute their vectors, component-tagged digital sources ±1,
// triggers [0,1] on x; summed and clamped by the platform), the raw analog/pointer surface, and the
// active-device signal. All value semantics.
struct PlayerSample {
    ActionSet                     held{};
    std::array<Vec2, kMaxActions> values{};
    AnalogInput                   analog{};
    ActiveDevice                  device{};
};

// The one value that crosses the platform seam per pump: every player slot's sample. The windowed
// host pushes this into the run loop each iteration; the loop accumulates it per tick (digital
// union, analog relative-sum / absolute-latest) exactly as it accumulated the single-player pair
// before slots existed.
struct InputSample {
    std::array<PlayerSample, kMaxPlayers> players{};
};

// Per-tick input view: action state for the current tick plus edges relative to the previous tick,
// and the analog/pointer surface sampled at the same tick — per player slot. Edges are
// sim-tick-keyed, so they are deterministic and frame-rate-independent — never sampled at render
// cadence.
//
// Single-player reads go straight through the InputState methods (the player(0) shorthand);
// multiplayer reads go through player(n). Both expose the same surface.
class InputState {
    // One slot's tick-keyed state. previous/current are honest levels; pressed is the union of
    // levels seen since the previous tick (press buffering — a sub-tick tap still registers exactly
    // one press). The analog pair drives cursorDelta and the mouse edges.
    struct Slot {
        ActionSet                     previous{};
        ActionSet                     current{};
        ActionSet                     pressed{};
        std::array<Vec2, kMaxActions> values{};
        AnalogInput                   analogPrev{};
        AnalogInput                   analog{};
        ActiveDevice                  device{};
    };

public:
    // A lightweight const view of one player slot. Obtain via player(n); the InputState's direct
    // methods below are player(0) with the slot index elided.
    class Player {
    public:
        // ── Digital actions ──
        template <ActionLike A>
        [[nodiscard]] bool isHeld(A a) const noexcept {  // active this tick (honest level)
            return slot_->current.test(actionId(a));
        }
        template <ActionLike A>
        [[nodiscard]] bool justPressed(A a) const noexcept {  // pressed since the last tick
            // Fires for a press observed since the previous tick that was not already active then —
            // including a sub-tick tap already released by tick time. Press buffering: a quick fire-tap while a
            // direction is held is never dropped.
            return slot_->pressed.test(actionId(a)) && !slot_->previous.test(actionId(a));
        }
        template <ActionLike A>
        [[nodiscard]] bool justReleased(A a) const noexcept {  // active→inactive this tick
            // Honest level falling edge off the current tick — never latched, so nothing sticks.
            return !slot_->current.test(actionId(a)) && slot_->previous.test(actionId(a));
        }

        // ── Valued actions ──
        template <ActionLike A>
        [[nodiscard]] Vec2 vector(A a) const noexcept {  // e.g. a stick, or composed directions
            return slot_->values[actionId(a)];
        }
        template <ActionLike A>
        [[nodiscard]] float axis(A a) const noexcept {  // 1-D value (a trigger pull); the x component
            return slot_->values[actionId(a)].x;
        }

        // ── Active device (glyph selection) ──
        [[nodiscard]] ActiveDevice activeDevice() const noexcept { return slot_->device; }

        // ── Pointer (mouse — meaningful on the slot the keyboard/mouse feeds) ──
        [[nodiscard]] Vec2i cursor() const noexcept { return slot_->analog.cursor; }
        [[nodiscard]] bool  cursorOnScreen() const noexcept { return slot_->analog.cursorOnScreen; }
        [[nodiscard]] Vec2i cursorDelta() const noexcept {
            // Per-tick viewport-space motion: the difference of two latest-at-tick absolute
            // positions. (Raw device delta — rawDeltaX/Y — is the accumulated relative measure a
            // spinner integrates instead.)
            return Vec2i{slot_->analog.cursor.x - slot_->analogPrev.cursor.x,
                         slot_->analog.cursor.y - slot_->analogPrev.cursor.y};
        }
        [[nodiscard]] float rawDeltaX() const noexcept { return slot_->analog.rawDeltaX; }
        [[nodiscard]] float rawDeltaY() const noexcept { return slot_->analog.rawDeltaY; }
        [[nodiscard]] float wheel() const noexcept { return slot_->analog.wheel; }
        [[nodiscard]] bool  mouseHeld(MouseButton b) const noexcept {
            return slot_->analog.mouseDown(b);
        }
        [[nodiscard]] bool mouseJustPressed(MouseButton b) const noexcept {
            return slot_->analog.mouseDown(b) && !slot_->analogPrev.mouseDown(b);
        }
        [[nodiscard]] bool mouseJustReleased(MouseButton b) const noexcept {
            return !slot_->analog.mouseDown(b) && slot_->analogPrev.mouseDown(b);
        }

        // ── Raw gamepad analog (aggregated across the slot's pads) ──
        [[nodiscard]] Vec2 stick(Stick s) const noexcept {
            return s == Stick::Left ? Vec2{slot_->analog.leftX, slot_->analog.leftY}
                                    : Vec2{slot_->analog.rightX, slot_->analog.rightY};
        }
        [[nodiscard]] float trigger(Trigger t) const noexcept {
            return t == Trigger::Left ? slot_->analog.triggerL : slot_->analog.triggerR;
        }
        // The d-pad as a digital unit vector (each axis −1 / 0 / +1; up = −y, the stick convention) —
        // a direction read without binding the d-pad to an action.
        [[nodiscard]] Vec2 dpad() const noexcept {
            return Vec2{slot_->analog.dpadX, slot_->analog.dpadY};
        }
        // The untouched hardware readings, before the configured dead-zone + gate — the escape hatch for a
        // game doing its own processing while stick()/trigger() still return the processed value.
        [[nodiscard]] Vec2 stickRaw(Stick s) const noexcept {
            return s == Stick::Left ? Vec2{slot_->analog.rawLeftX, slot_->analog.rawLeftY}
                                    : Vec2{slot_->analog.rawRightX, slot_->analog.rawRightY};
        }
        [[nodiscard]] float triggerRaw(Trigger t) const noexcept {
            return t == Trigger::Left ? slot_->analog.rawTriggerL : slot_->analog.rawTriggerR;
        }

    private:
        friend class InputState;
        explicit constexpr Player(const Slot* slot) noexcept : slot_(slot) {}
        const Slot* slot_;
    };

    // The view for one player slot; index clamps into [0, kMaxPlayers).
    [[nodiscard]] Player player(int index) const noexcept {
        const int i = index < 0 ? 0 : (index >= kMaxPlayers ? kMaxPlayers - 1 : index);
        return Player{&slots_[static_cast<std::size_t>(i)]};
    }

    // ── The single-player surface: identical to player(0), slot index elided ──
    template <ActionLike A>
    [[nodiscard]] bool isHeld(A a) const noexcept { return player(0).isHeld(a); }
    template <ActionLike A>
    [[nodiscard]] bool justPressed(A a) const noexcept { return player(0).justPressed(a); }
    template <ActionLike A>
    [[nodiscard]] bool justReleased(A a) const noexcept { return player(0).justReleased(a); }
    template <ActionLike A>
    [[nodiscard]] Vec2 vector(A a) const noexcept { return player(0).vector(a); }
    template <ActionLike A>
    [[nodiscard]] float axis(A a) const noexcept { return player(0).axis(a); }
    [[nodiscard]] ActiveDevice activeDevice() const noexcept { return player(0).activeDevice(); }
    [[nodiscard]] Vec2i cursor() const noexcept { return player(0).cursor(); }
    [[nodiscard]] bool  cursorOnScreen() const noexcept { return player(0).cursorOnScreen(); }
    [[nodiscard]] Vec2i cursorDelta() const noexcept { return player(0).cursorDelta(); }
    [[nodiscard]] float rawDeltaX() const noexcept { return player(0).rawDeltaX(); }
    [[nodiscard]] float rawDeltaY() const noexcept { return player(0).rawDeltaY(); }
    [[nodiscard]] float wheel() const noexcept { return player(0).wheel(); }
    [[nodiscard]] bool  mouseHeld(MouseButton b) const noexcept { return player(0).mouseHeld(b); }
    [[nodiscard]] bool  mouseJustPressed(MouseButton b) const noexcept {
        return player(0).mouseJustPressed(b);
    }
    [[nodiscard]] bool mouseJustReleased(MouseButton b) const noexcept {
        return player(0).mouseJustReleased(b);
    }
    [[nodiscard]] Vec2 stick(Stick s) const noexcept { return player(0).stick(s); }
    [[nodiscard]] float trigger(Trigger t) const noexcept { return player(0).trigger(t); }
    [[nodiscard]] Vec2 dpad() const noexcept { return player(0).dpad(); }
    [[nodiscard]] Vec2 stickRaw(Stick s) const noexcept { return player(0).stickRaw(s); }
    [[nodiscard]] float triggerRaw(Trigger t) const noexcept { return player(0).triggerRaw(t); }

    // Platform-internal: advance one tick. `sample` carries each slot's latest level, per-action
    // values, accumulated analog, and active device; `pressedSinceTick` is each slot's UNION of
    // every host-frame level observed since the previous tick (the run loop's heldUnion).
    // justPressed fires for any action in the union that was not active at the previous tick — so a
    // tap shorter than a tick still registers exactly one press. isHeld()/justReleased() stay honest
    // level edges off the sample's level, so a release never sticks.
    //
    // On the first tick previous is the all-inactive default, so an action already active on tick 1
    // reads as justPressed — the correct edge for a press that landed between baseline and tick 1.
    void sampleTick(const InputSample& sample,
                    const std::array<ActionSet, kMaxPlayers>& pressedSinceTick) noexcept;

private:
    std::array<Slot, kMaxPlayers> slots_{};
};

}  // namespace retropp
