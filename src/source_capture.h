#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>

#include "retropp/analog_response.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"

namespace retropp::detail {

// The signed axis value a stick-direction pseudo-button reads on one pad (positive = pressed
// direction). Up is -y in SDL's convention. The sampler and the capture both read a stick direction
// through this one definition.
[[nodiscard]] constexpr float stickDirValue(PadButton b, float leftX, float leftY, float rightX,
                                            float rightY) noexcept {
    switch (b) {
        case PadButton::LeftStickUp:     return -leftY;
        case PadButton::LeftStickDown:   return leftY;
        case PadButton::LeftStickLeft:   return -leftX;
        case PadButton::LeftStickRight:  return leftX;
        case PadButton::RightStickUp:    return -rightY;
        case PadButton::RightStickDown:  return rightY;
        case PadButton::RightStickLeft:  return -rightX;
        case PadButton::RightStickRight: return rightX;
        default:                         return 0.0f;
    }
}

// One pad as the capture sees it: its SDL instance id, the slot it feeds, its family, and its six
// untouched analog readings (sticks in [-1, 1] per axis, triggers in [0, 1]).
struct CapturePad {
    SDL_JoystickID id     = 0;
    int            slot   = 0;
    ControllerType family = ControllerType::Unknown;
    float rawLeftX    = 0.0f;
    float rawLeftY    = 0.0f;
    float rawRightX   = 0.0f;
    float rawRightY   = 0.0f;
    float rawTriggerL = 0.0f;
    float rawTriggerR = 0.0f;
};

// The capture behind SdlPlatform::captureRequest / capturedSource, as a device-free state machine: it
// reads plain values and SDL_Event values and never touches a device, so every rule runs without SDL
// initialized.
//
// A request arms one slot and clears its answer. The first press on that slot's devices that follows
// becomes the answer and the slot disarms; the answer stays until the slot's next request. A press is
// a down event: a key (repeats and SDL_SCANCODE_UNKNOWN are not presses), the left, right or middle
// mouse button, a pad button padButtonFrom names, or a trigger or stick direction rising past its
// default digital threshold after the AnalogResponse. A key or mouse press belongs to the keyboard's
// slot; a pad press to the pad's slot. Out-of-range slots clamp into [0, kMaxPlayers).
class SourceCapture {
public:
    // Arm `slot`, clear its answer, and take each of its pads' current analog state as the starting
    // point, so a trigger already pulled or a stick already pushed is not a press.
    void request(int slot, std::span<const CapturePad> padsOnSlot, const AnalogResponse& response);

    // The slot's captured press: empty before a request and while the request waits for a press.
    [[nodiscard]] std::optional<CapturedSource> answer(int slot) const noexcept;

    // Whether any slot is waiting for a press.
    [[nodiscard]] bool anyArmed() const noexcept;

    // One event. `pad` is the event's own pad with its current readings, or null for a keyboard or
    // mouse event; `keyboardSlot` is the slot the keyboard+mouse unit feeds.
    void event(const SDL_Event& e, int keyboardSlot, const CapturePad* pad,
               const AnalogResponse& response);

private:
    // One pad's analog pseudo-buttons, one bit each, set while the pad reads them down.
    struct Latch {
        SDL_JoystickID id   = 0;
        int            slot = 0;
        std::uint16_t  down = 0;
    };

    void capture(int slot, const CapturedSource& press);

    std::array<bool, kMaxPlayers>                          armed_{};
    std::array<std::optional<CapturedSource>, kMaxPlayers> answers_{};
    std::vector<Latch>                                     latches_;
};

}  // namespace retropp::detail
