#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include "retropp/input.h"

namespace retropp {

// ── The binding surface ─────────────────────────────────────────────────────────────────────────
//
// The SDL-coupled half of the action model (see input.h for the std-only read side). A game builds
// an ActionMap — rows of (its own action ← a physical source) — and hands the value to the platform
// (SdlPlatform::actions). The map is a plain value the game owns: updating the live bindings is
// editing your copy and resubmitting it wholesale; the platform keeps only a replaceable copy of the
// last submission. There is no platform-side registry, no stored configuration, and no filtering —
// every row a game writes is live.

// The digital pad vocabulary, device-class not device-instance: a binding targets "a pad's south
// button", so swapping controllers mid-game just works. Three naming layers over the same buttons:
//
//   * Positional cardinals (FaceSouth/East/West/North) — the ground truth. SDL reports positions,
//     so these resolve identically on every pad family.
//   * Label aliases (FaceLabelA/B/X/Y) — "the button PRINTED that letter on the connected pad".
//     The same four letters sit at different positions on Xbox vs Nintendo pads; a label alias
//     resolves per pad family at sample time (resolvePadButton below). Families without letter
//     labels (PlayStation, generic) resolve to the Xbox-convention position — the industry default
//     a PlayStation player expects when a game says "Press A".
//   * Sony symbol synonyms (FaceCross/Circle/Square/Triangle) — equal-value aliases of the
//     cardinals. Sony's symbols are positionally fixed on every pad Sony has shipped, so these are
//     pure spelling, no resolution.
//
// Shoulders, triggers, and stick clicks have one neutral name each (RB ≡ R1 ≡ R is ShoulderR); the
// per-family printed name is a glyph concern, read off the active-device signal. TriggerL/R and the
// stick-direction entries are analog-backed: bound to a digital action they cross a threshold
// (kTriggerThreshold / kStickDirThreshold, overridable per row via withThreshold).
enum class PadButton : std::uint8_t {
    FaceSouth,
    FaceEast,
    FaceWest,
    FaceNorth,
    FaceCross    = FaceSouth,   // Sony's ✕ — always south
    FaceCircle   = FaceEast,    // ○ — always east
    FaceSquare   = FaceWest,    // □ — always west
    FaceTriangle = FaceNorth,   // △ — always north
    FaceLabelA = 4,
    FaceLabelB,
    FaceLabelX,
    FaceLabelY,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    ShoulderL,      // LB · L1 · L
    ShoulderR,      // RB · R1 · R
    TriggerL,       // LT · L2 · ZL — analog-backed
    TriggerR,       // RT · R2 · ZR — analog-backed
    StickClickL,    // L3
    StickClickR,    // R3
    Start,
    Select,
    Guide,
    Share,          // Share (Xbox Series) · Create (PS5) · Capture (Switch) — SDL's MISC1
    LeftStickUp,    // analog-backed stick directions, digital via threshold
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    RightStickUp,
    RightStickDown,
    RightStickLeft,
    RightStickRight,
};

// A 2-D vector source: the whole stick. Bound to an action, it contributes its dead-zoned [-1,1]²
// vector to the action's vector() read; on a digital read it counts as past-threshold magnitude.
enum class PadStick : std::uint8_t { Left, Right };

// How a digital source contributes to a vector-valued action: ±1 on the tagged axis while down.
// Set by the directional presets (and available directly via asComponent). Up is -y, matching the
// stick convention (SDL: down is positive).
enum class Dir : std::uint8_t { None, Up, Down, Left, Right };

// Default digital thresholds for analog-backed sources (post-dead-zone values).
inline constexpr float kTriggerThreshold  = 0.30f;
inline constexpr float kStickDirThreshold = 0.50f;

// One physical source. Implicitly constructible from each payload type, so the braced row form
// reads as a plain list: {Action::Fire, {SDL_SCANCODE_X, PadButton::FaceLabelA, MouseButton::Left}}.
// `family` qualifies a Pad/Stick source to one pad family (see onPad and the suppression rule on
// ActionMap); `threshold` overrides the per-kind digital threshold (0 = default); `component` tags a
// digital source's vector contribution.
struct Source {
    enum class Kind : std::uint8_t { Key, Pad, Mouse, Stick };

    Kind         kind;
    SDL_Scancode key   = SDL_SCANCODE_UNKNOWN;
    PadButton    pad   = PadButton::FaceSouth;
    MouseButton  mouse = MouseButton::Left;
    PadStick     stick = PadStick::Left;

    std::optional<ControllerType> family{};   // empty = applies on every pad family
    float threshold = 0.0f;                   // 0 = the per-kind default
    Dir   component = Dir::None;              // vector-read contribution axis for digital sources

    constexpr Source(SDL_Scancode k) noexcept : kind(Kind::Key), key(k) {}
    constexpr Source(PadButton b) noexcept : kind(Kind::Pad), pad(b) {}
    constexpr Source(MouseButton m) noexcept : kind(Kind::Mouse), mouse(m) {}
    constexpr Source(PadStick s) noexcept : kind(Kind::Stick), stick(s) {}

    friend constexpr bool operator==(const Source&, const Source&) noexcept = default;
};

// Family-qualify a pad source: the row applies ONLY when the connected pad is that family, and it
// SUPPRESSES the action's unqualified pad rows on that family (see ActionMap). This is how a game
// names each family's button explicitly on one action.
[[nodiscard]] constexpr Source onPad(ControllerType family, PadButton b) noexcept {
    Source s{b};
    s.family = family;
    return s;
}
[[nodiscard]] constexpr Source onPad(ControllerType family, PadStick st) noexcept {
    Source s{st};
    s.family = family;
    return s;
}

// Override the digital threshold of an analog-backed source for this row.
[[nodiscard]] constexpr Source withThreshold(Source s, float threshold) noexcept {
    s.threshold = threshold;
    return s;
}

// Tag a digital source's contribution to a vector-valued action (±1 on the axis while down).
[[nodiscard]] constexpr Source asComponent(Source s, Dir d) noexcept {
    s.component = d;
    return s;
}

// One row of an ActionMap: action ← source. Multi-source is simply multiple rows per action.
struct ActionBinding {
    ActionId action;
    Source   source;
};

// A declarative row for ActionMap's list constructor: the game's action plus its source list.
struct ActionRow {
    ActionId            action;
    std::vector<Source> sources;

    template <ActionLike A>
    ActionRow(A a, std::initializer_list<Source> list)
        : action(actionId(a)), sources(list) {}
};

// The game's whole input scheme as one value: a flat list of (action ← source) rows. Build it
// declaratively, with bind(), or both; merge preset bundles in with add(); hand it to the platform
// with SdlPlatform::actions. Empty map = no action input (the platform has no vocabulary of its
// own).
//
// Family-qualified suppression rule: for a given action sampled against a pad of family F, if any
// Pad/Stick row for that action is qualified with F (onPad), ONLY those qualified rows apply on that
// pad — the action's unqualified Pad/Stick rows are suppressed for F. Unqualified rows serve every
// family without qualified rows; Key/Mouse rows are never affected. This makes an explicit
// per-family Confirm/Cancel swap total instead of double-firing, and it makes FaceLabelA literally
// shorthand for {onPad(Nintendo, FaceEast), FaceSouth}.
class ActionMap {
public:
    ActionMap() = default;
    ActionMap(std::initializer_list<ActionRow> rows);

    template <ActionLike A>
    void bind(A action, Source source) {
        bindId(actionId(action), source);
    }
    template <ActionLike A>
    void unbind(A action, Source source) {  // removes rows matching (action, source) exactly
        unbindId(actionId(action), source);
    }
    template <ActionLike A>
    void clearAction(A action) {  // removes every row for the action
        clearId(actionId(action));
    }

    // Append another map's rows — how a preset bundle merges in. The bundle's rows are
    // indistinguishable from hand-written ones afterwards.
    void add(const ActionMap& bundle);

    [[nodiscard]] std::span<const ActionBinding> rows() const noexcept { return rows_; }

    // The untemplated cores (the templated methods above cast and forward).
    void bindId(ActionId action, Source source);
    void unbindId(ActionId action, Source source);
    void clearId(ActionId action);

private:
    std::vector<ActionBinding> rows_;
};

// A press the platform captured for a controls screen (SdlPlatform::captureRequest /
// capturedSource): the source the player pressed and the device that produced it. `source` carries
// no family, so binding it as returned serves every pad family; `device.family` names the pad's
// family for a game that wants the family-qualified row instead
// (onPad(press.device.family, press.source.pad)).
struct CapturedSource {
    Source       source;
    ActiveDevice device{};
};

// Preset bundles: conventional source rows for actions the CALLER names. A preset cannot know the
// game's vocabulary — it contributes the source side; the game supplies the action side. Presets
// never construct the game's map: merge them in with ActionMap::add. Zero enforcement — the result
// is ordinary rows the game could have written by hand.
namespace presets {

// Digital movement: arrows + WASD + d-pad on four caller-named actions.
[[nodiscard]] ActionMap directionalIds(ActionId up, ActionId down, ActionId left, ActionId right);

// Vector movement on one caller-named action: the left stick plus arrows/WASD/d-pad
// component-tagged, so stick and keys feed the same vector() read (keys give unit directions).
[[nodiscard]] ActionMap directionalVectorId(ActionId move);

template <ActionLike A>
[[nodiscard]] ActionMap directional(A up, A down, A left, A right) {
    return directionalIds(actionId(up), actionId(down), actionId(left), actionId(right));
}
template <ActionLike A>
[[nodiscard]] ActionMap directionalVector(A move) {
    return directionalVectorId(actionId(move));
}

}  // namespace presets

// ── Resolution helpers (pure; the platform samples through these, tests pin them headlessly) ─────

// The SDL button a digital PadButton reads on a pad of `family`. Label aliases resolve here — the
// sole heir of the per-family layout knowledge (Nintendo transposes the printed A/B/X/Y versus the
// Xbox layout; everything else, PlayStation included, follows the Xbox-convention positions).
// Analog-backed entries (triggers, stick directions) return SDL_GAMEPAD_BUTTON_INVALID — they read
// axes, not buttons (padButtonIsAnalog).
[[nodiscard]] SDL_GamepadButton resolvePadButton(PadButton b, ControllerType family) noexcept;

// The positional PadButton an SDL button is, on every pad family: SOUTH is FaceSouth, MISC1 is
// Share. Derived from resolvePadButton, so the layout is written in one place. A button outside the
// vocabulary (a paddle, the touchpad, SDL_GAMEPAD_BUTTON_INVALID) answers nothing.
[[nodiscard]] std::optional<PadButton> padButtonFrom(SDL_GamepadButton button) noexcept;

// Whether the PadButton reads an axis (triggers + stick directions) instead of an SDL button.
[[nodiscard]] bool padButtonIsAnalog(PadButton b) noexcept;

// The row's effective digital threshold: its explicit override, or the per-kind default.
[[nodiscard]] float sourceThreshold(const Source& s) noexcept;

// One bit per ControllerType value: which families have family-qualified Pad/Stick rows for the
// action. Feeds the suppression rule below; the platform computes this once per pump per action.
[[nodiscard]] std::uint8_t qualifiedFamilyMask(std::span<const ActionBinding> rows,
                                               ActionId action) noexcept;

// The suppression rule for one Pad/Stick source against one pad: a qualified source applies only on
// its family; an unqualified source applies only when the action has NO qualified rows for the
// pad's family (the qualifiedMask bit).
[[nodiscard]] bool padRowAppliesTo(const Source& source, ControllerType padFamily,
                                   std::uint8_t qualifiedMask) noexcept;

// Collapse SDL's fine-grained gamepad type into the platform family. Tested against SDL enum values
// with no live device.
[[nodiscard]] ControllerType controllerTypeFrom(SDL_GamepadType type) noexcept;

}  // namespace retropp
