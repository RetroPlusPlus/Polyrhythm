#include "retropp/input_actions.h"

#include <array>

namespace retropp {

ActionMap::ActionMap(std::initializer_list<ActionRow> rows) {
    for (const ActionRow& row : rows) {
        for (const Source& source : row.sources) {
            rows_.push_back(ActionBinding{row.action, source});
        }
    }
}

void ActionMap::bindId(ActionId action, Source source) {
    rows_.push_back(ActionBinding{action, source});
}

void ActionMap::unbindId(ActionId action, Source source) {
    std::erase_if(rows_, [&](const ActionBinding& row) {
        return row.action == action && row.source == source;
    });
}

void ActionMap::clearId(ActionId action) {
    std::erase_if(rows_, [&](const ActionBinding& row) { return row.action == action; });
}

void ActionMap::add(const ActionMap& bundle) {
    rows_.insert(rows_.end(), bundle.rows_.begin(), bundle.rows_.end());
}

namespace presets {

ActionMap directionalIds(ActionId up, ActionId down, ActionId left, ActionId right) {
    ActionMap m;
    m.bindId(up, SDL_SCANCODE_UP);
    m.bindId(up, SDL_SCANCODE_W);
    m.bindId(up, PadButton::DpadUp);
    m.bindId(down, SDL_SCANCODE_DOWN);
    m.bindId(down, SDL_SCANCODE_S);
    m.bindId(down, PadButton::DpadDown);
    m.bindId(left, SDL_SCANCODE_LEFT);
    m.bindId(left, SDL_SCANCODE_A);
    m.bindId(left, PadButton::DpadLeft);
    m.bindId(right, SDL_SCANCODE_RIGHT);
    m.bindId(right, SDL_SCANCODE_D);
    m.bindId(right, PadButton::DpadRight);
    return m;
}

ActionMap directionalVectorId(ActionId move) {
    ActionMap m;
    m.bindId(move, PadStick::Left);
    m.bindId(move, asComponent(SDL_SCANCODE_UP, Dir::Up));
    m.bindId(move, asComponent(SDL_SCANCODE_W, Dir::Up));
    m.bindId(move, asComponent(PadButton::DpadUp, Dir::Up));
    m.bindId(move, asComponent(SDL_SCANCODE_DOWN, Dir::Down));
    m.bindId(move, asComponent(SDL_SCANCODE_S, Dir::Down));
    m.bindId(move, asComponent(PadButton::DpadDown, Dir::Down));
    m.bindId(move, asComponent(SDL_SCANCODE_LEFT, Dir::Left));
    m.bindId(move, asComponent(SDL_SCANCODE_A, Dir::Left));
    m.bindId(move, asComponent(PadButton::DpadLeft, Dir::Left));
    m.bindId(move, asComponent(SDL_SCANCODE_RIGHT, Dir::Right));
    m.bindId(move, asComponent(SDL_SCANCODE_D, Dir::Right));
    m.bindId(move, asComponent(PadButton::DpadRight, Dir::Right));
    return m;
}

}  // namespace presets

SDL_GamepadButton resolvePadButton(PadButton b, ControllerType family) noexcept {
    const bool nintendo = (family == ControllerType::Nintendo);
    switch (b) {
        case PadButton::FaceSouth: return SDL_GAMEPAD_BUTTON_SOUTH;
        case PadButton::FaceEast:  return SDL_GAMEPAD_BUTTON_EAST;
        case PadButton::FaceWest:  return SDL_GAMEPAD_BUTTON_WEST;
        case PadButton::FaceNorth: return SDL_GAMEPAD_BUTTON_NORTH;
        // The printed letters migrate between families: Nintendo transposes A/B and X/Y versus the
        // Xbox layout. Families without letter labels resolve to the Xbox-convention position.
        case PadButton::FaceLabelA:
            return nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        case PadButton::FaceLabelB:
            return nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        case PadButton::FaceLabelX:
            return nintendo ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST;
        case PadButton::FaceLabelY:
            return nintendo ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH;
        case PadButton::DpadUp:      return SDL_GAMEPAD_BUTTON_DPAD_UP;
        case PadButton::DpadDown:    return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
        case PadButton::DpadLeft:    return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        case PadButton::DpadRight:   return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        case PadButton::ShoulderL:   return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
        case PadButton::ShoulderR:   return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        case PadButton::StickClickL: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
        case PadButton::StickClickR: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
        case PadButton::Start:       return SDL_GAMEPAD_BUTTON_START;
        case PadButton::Select:      return SDL_GAMEPAD_BUTTON_BACK;
        case PadButton::Guide:       return SDL_GAMEPAD_BUTTON_GUIDE;
        case PadButton::Share:       return SDL_GAMEPAD_BUTTON_MISC1;
        case PadButton::TriggerL:
        case PadButton::TriggerR:
        case PadButton::LeftStickUp:
        case PadButton::LeftStickDown:
        case PadButton::LeftStickLeft:
        case PadButton::LeftStickRight:
        case PadButton::RightStickUp:
        case PadButton::RightStickDown:
        case PadButton::RightStickLeft:
        case PadButton::RightStickRight:
            return SDL_GAMEPAD_BUTTON_INVALID;  // analog-backed: read an axis, not a button
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

namespace {

// The PadButton names a button press answers as: every digital enumerator except the four
// printed-letter aliases. A pad control added to resolvePadButton is added here in the same edit.
constexpr std::array kPressablePadButtons{
    PadButton::FaceSouth, PadButton::FaceEast,  PadButton::FaceWest,    PadButton::FaceNorth,
    PadButton::DpadUp,    PadButton::DpadDown,  PadButton::DpadLeft,    PadButton::DpadRight,
    PadButton::ShoulderL, PadButton::ShoulderR, PadButton::StickClickL, PadButton::StickClickR,
    PadButton::Start,     PadButton::Select,    PadButton::Guide,       PadButton::Share,
};

}  // namespace

std::optional<PadButton> padButtonFrom(SDL_GamepadButton button) noexcept {
    if (button == SDL_GAMEPAD_BUTTON_INVALID) return std::nullopt;
    for (const PadButton b : kPressablePadButtons) {
        if (resolvePadButton(b, ControllerType::Standard) == button) return b;
    }
    return std::nullopt;
}

bool padButtonIsAnalog(PadButton b) noexcept {
    switch (b) {
        case PadButton::TriggerL:
        case PadButton::TriggerR:
        case PadButton::LeftStickUp:
        case PadButton::LeftStickDown:
        case PadButton::LeftStickLeft:
        case PadButton::LeftStickRight:
        case PadButton::RightStickUp:
        case PadButton::RightStickDown:
        case PadButton::RightStickLeft:
        case PadButton::RightStickRight:
            return true;
        default:
            return false;
    }
}

float sourceThreshold(const Source& s) noexcept {
    if (s.threshold > 0.0f) return s.threshold;
    if (s.kind == Source::Kind::Stick) return kStickDirThreshold;
    if (s.kind == Source::Kind::Pad &&
        (s.pad == PadButton::TriggerL || s.pad == PadButton::TriggerR)) {
        return kTriggerThreshold;
    }
    return kStickDirThreshold;  // stick-direction pseudo-buttons (the remaining analog-backed kind)
}

std::uint8_t qualifiedFamilyMask(std::span<const ActionBinding> rows, ActionId action) noexcept {
    std::uint8_t mask = 0;
    for (const ActionBinding& row : rows) {
        if (row.action != action || !row.source.family.has_value()) continue;
        if (row.source.kind != Source::Kind::Pad && row.source.kind != Source::Kind::Stick) continue;
        mask |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(*row.source.family));
    }
    return mask;
}

bool padRowAppliesTo(const Source& source, ControllerType padFamily,
                     std::uint8_t qualifiedMask) noexcept {
    if (source.family.has_value()) {
        return *source.family == padFamily;
    }
    const auto familyBit = static_cast<std::uint8_t>(1u << static_cast<unsigned>(padFamily));
    return (qualifiedMask & familyBit) == 0;
}

ControllerType controllerTypeFrom(SDL_GamepadType type) noexcept {
    switch (type) {
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return ControllerType::Xbox;
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return ControllerType::PlayStation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return ControllerType::Nintendo;
        case SDL_GAMEPAD_TYPE_STANDARD:
            return ControllerType::Standard;
        default:
            return ControllerType::Unknown;
    }
}

}  // namespace retropp
