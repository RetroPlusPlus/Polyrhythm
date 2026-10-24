#pragma once

#include <cstdint>

#include "retropp/geometry.h"  // Vec2

namespace retropp {

// ── Configurable analog processing — dead-zone + stick gate ──────────────────────────────────────────
//
// Turns a controller's raw analog readings into the values a game reads. It sits at the platform sample,
// so stick() and the action vector() read (both derive from the same normalized stick) see the configured
// shape. A game hands the config to the platform with SdlPlatform::analogResponse; the untouched
// hardware value stays reachable through stickRaw()/triggerRaw().

// How the dead-zone treats the two stick axes.
//   Radial  — one magnitude threshold on the whole stick; direction is preserved (the default).
//   PerAxis — each axis zeroed independently, so a small off-axis push on one axis is dropped even while
//             the other axis is live.
enum class DeadZoneShape : std::uint8_t { Radial, PerAxis };

// How a stick's round physical throw maps to usable range. A physical stick caps its magnitude at 1, so a
// corner push only reaches ~0.707 on each axis — a game reading x for one control and y for another can
// never get a full diagonal on the raw throw.
//   Round  — the raw radial throw, no remap (correct for radial aim; the default).
//   Square — stretch the throw so corners reach (±1, ±1); cardinals unchanged.
//   Scaled — a middle ground boosting diagonals by `gateScale` short of full square.
// Round == Scaled(1.0); Square is Scaled at the diagonal limit (~sqrt(2)).
enum class GateShape : std::uint8_t { Round, Square, Scaled };

// The dead-zone: a center region zeroed, with motion rescaled so it begins right at the edge (no jump).
// `size` is the fraction of full throw zeroed, in [0, 1); 0 passes the input straight through. The
// default shape is Radial (direction-preserving); PerAxis is the opt-in.
struct DeadZone {
    float         size  = 0.15f;
    DeadZoneShape shape = DeadZoneShape::Radial;
};

// Two-axis stick processing: dead-zone then gate. `gateScale` is read only when `gate == Scaled`.
struct StickResponse {
    DeadZone  deadZone{};
    GateShape gate      = GateShape::Round;
    float     gateScale = 1.0f;  // Scaled boost, 1.0 (Round) .. ~1.414 (Square)
};

// One-dimensional trigger processing: a dead-zone size only — a trigger has no direction to gate.
struct TriggerResponse {
    float deadZone = 0.06f;
};

// The whole controller's analog processing, one entry per input. A default-constructed value is the
// platform's out-of-the-box behavior (Radial dead-zone, no gate).
struct AnalogResponse {
    StickResponse   leftStick{};
    StickResponse   rightStick{};
    TriggerResponse leftTrigger{};
    TriggerResponse rightTrigger{};
};

// The pure resolvers — the whole remap, no platform state, so they are unit-tested against synthesized
// values without a device. Inputs are raw normalized readings (sticks in [-1, 1] per axis, triggers in
// [0, 1]); outputs are the processed values in the same range. sdl_platform's normStick/normTrigger are
// the only callers.
[[nodiscard]] Vec2  applyStickResponse(float rawX, float rawY, const StickResponse& response) noexcept;
[[nodiscard]] float applyTriggerResponse(float raw, const TriggerResponse& response) noexcept;

}  // namespace retropp
