#include "source_capture.h"

#include <algorithm>
#include <cstddef>

namespace retropp::detail {

namespace {

// The ten analog-backed pseudo-buttons in enumerator order; bit i of a latch is entry i.
constexpr std::array<PadButton, 10> kAnalogPadButtons{
    PadButton::TriggerL,      PadButton::TriggerR,       PadButton::LeftStickUp,
    PadButton::LeftStickDown, PadButton::LeftStickLeft,  PadButton::LeftStickRight,
    PadButton::RightStickUp,  PadButton::RightStickDown, PadButton::RightStickLeft,
    PadButton::RightStickRight,
};

// Each pseudo-button's processed value on one pad: the trigger pull after its dead-zone, or the stick
// direction's axis after the stick's dead-zone and gate.
std::array<float, kAnalogPadButtons.size()> analogValues(const CapturePad& pad,
                                                         const AnalogResponse& response) noexcept {
    const Vec2 left  = applyStickResponse(pad.rawLeftX, pad.rawLeftY, response.leftStick);
    const Vec2 right = applyStickResponse(pad.rawRightX, pad.rawRightY, response.rightStick);
    std::array<float, kAnalogPadButtons.size()> values{};
    for (std::size_t i = 0; i < kAnalogPadButtons.size(); ++i) {
        const PadButton b = kAnalogPadButtons[i];
        if (b == PadButton::TriggerL) {
            values[i] = applyTriggerResponse(pad.rawTriggerL, response.leftTrigger);
        } else if (b == PadButton::TriggerR) {
            values[i] = applyTriggerResponse(pad.rawTriggerR, response.rightTrigger);
        } else {
            values[i] = stickDirValue(b, left.x, left.y, right.x, right.y);
        }
    }
    return values;
}

// Which pseudo-buttons the pad reads down: each value at or past the threshold the sampler gives a
// row of that source with no override.
std::uint16_t analogDown(const std::array<float, kAnalogPadButtons.size()>& values) noexcept {
    std::uint16_t down = 0;
    for (std::size_t i = 0; i < kAnalogPadButtons.size(); ++i) {
        if (values[i] >= sourceThreshold(Source{kAnalogPadButtons[i]})) {
            down = static_cast<std::uint16_t>(down | (1u << i));
        }
    }
    return down;
}

int clampSlot(int slot) noexcept { return std::clamp(slot, 0, kMaxPlayers - 1); }

}  // namespace

void SourceCapture::request(int slot, std::span<const CapturePad> padsOnSlot,
                            const AnalogResponse& response) {
    slot = clampSlot(slot);
    armed_[static_cast<std::size_t>(slot)]   = true;
    answers_[static_cast<std::size_t>(slot)] = std::nullopt;
    for (const CapturePad& pad : padsOnSlot) {
        std::erase_if(latches_, [&](const Latch& l) { return l.id == pad.id; });
        latches_.push_back(Latch{.id = pad.id, .slot = slot,
                                 .down = analogDown(analogValues(pad, response))});
    }
}

std::optional<CapturedSource> SourceCapture::answer(int slot) const noexcept {
    return answers_[static_cast<std::size_t>(clampSlot(slot))];
}

bool SourceCapture::anyArmed() const noexcept {
    return std::any_of(armed_.begin(), armed_.end(), [](bool a) { return a; });
}

void SourceCapture::capture(int slot, const CapturedSource& press) {
    answers_[static_cast<std::size_t>(slot)] = press;
    armed_[static_cast<std::size_t>(slot)]   = false;
    std::erase_if(latches_, [&](const Latch& l) { return l.slot == slot; });
}

void SourceCapture::event(const SDL_Event& e, int keyboardSlot, const CapturePad* pad,
                          const AnalogResponse& response) {
    constexpr ActiveDevice kKeyboardMouse{.kind = DeviceKind::KeyboardMouse,
                                          .family = ControllerType::Unknown};
    keyboardSlot = clampSlot(keyboardSlot);

    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!armed_[static_cast<std::size_t>(keyboardSlot)]) return;
            if (e.key.repeat || e.key.scancode == SDL_SCANCODE_UNKNOWN) return;
            capture(keyboardSlot, CapturedSource{.source = Source{e.key.scancode},
                                                 .device = kKeyboardMouse});
            return;

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (!armed_[static_cast<std::size_t>(keyboardSlot)]) return;
            std::optional<MouseButton> button;
            switch (e.button.button) {
                case SDL_BUTTON_LEFT:   button = MouseButton::Left;   break;
                case SDL_BUTTON_RIGHT:  button = MouseButton::Right;  break;
                case SDL_BUTTON_MIDDLE: button = MouseButton::Middle; break;
                default:                return;
            }
            capture(keyboardSlot, CapturedSource{.source = Source{*button},
                                                 .device = kKeyboardMouse});
            return;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            if (pad == nullptr) return;
            const int slot = clampSlot(pad->slot);
            if (!armed_[static_cast<std::size_t>(slot)]) return;
            const std::optional<PadButton> b =
                padButtonFrom(static_cast<SDL_GamepadButton>(e.gbutton.button));
            if (!b) return;
            capture(slot, CapturedSource{
                              .source = Source{*b},
                              .device = ActiveDevice{.kind = DeviceKind::Gamepad, .family = pad->family}});
            return;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            if (pad == nullptr) return;
            const int slot = clampSlot(pad->slot);
            if (!armed_[static_cast<std::size_t>(slot)]) return;
            const auto          values = analogValues(*pad, response);
            const std::uint16_t down   = analogDown(values);

            const auto latch = std::find_if(latches_.begin(), latches_.end(),
                                            [&](const Latch& l) { return l.id == pad->id; });
            if (latch == latches_.end()) {
                // A pad the request did not see: its current readings are its starting point.
                latches_.push_back(Latch{.id = pad->id, .slot = slot, .down = down});
                return;
            }
            const auto rising = static_cast<std::uint16_t>(down & ~latch->down);
            latch->slot = slot;
            latch->down = down;
            if (rising == 0) return;

            // More than one rising at once (a diagonal): the larger processed value names the press;
            // on an exact tie, the lower enumerator.
            std::size_t pick = kAnalogPadButtons.size();
            for (std::size_t i = 0; i < kAnalogPadButtons.size(); ++i) {
                if ((rising & (1u << i)) == 0) continue;
                if (pick == kAnalogPadButtons.size() || values[i] > values[pick]) pick = i;
            }
            capture(slot, CapturedSource{
                              .source = Source{kAnalogPadButtons[pick]},
                              .device = ActiveDevice{.kind = DeviceKind::Gamepad, .family = pad->family}});
            return;
        }

        default:
            return;
    }
}

}  // namespace retropp::detail
