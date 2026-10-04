// Device-free coverage for capturing a pressed source: padButtonFrom (the positional PadButton an SDL
// button is) and the capture state machine behind SdlPlatform::captureRequest / capturedSource —
// what counts as a press, the analog crossing under the live AnalogResponse, first press wins,
// routing by slot. Events are built by hand; SDL is never initialized.
#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include "retropp/input_actions.h"
#include "src/source_capture.h"

namespace retropp {
namespace {

using detail::CapturePad;
using detail::SourceCapture;

constexpr ActiveDevice kKeyboardMouse{.kind = DeviceKind::KeyboardMouse,
                                      .family = ControllerType::Unknown};
const AnalogResponse kDefaultResponse{};

SDL_Event blankEvent(SDL_EventType type) {
    SDL_Event e;
    std::memset(&e, 0, sizeof e);
    e.type = type;
    return e;
}

SDL_Event keyDown(SDL_Scancode scancode, bool repeat = false) {
    SDL_Event e    = blankEvent(SDL_EVENT_KEY_DOWN);
    e.key.scancode = scancode;
    e.key.down     = true;
    e.key.repeat   = repeat;
    return e;
}

SDL_Event keyUp(SDL_Scancode scancode) {
    SDL_Event e    = blankEvent(SDL_EVENT_KEY_UP);
    e.key.scancode = scancode;
    return e;
}

SDL_Event mouseDown(Uint8 button) {
    SDL_Event e     = blankEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
    e.button.button = button;
    e.button.down   = true;
    return e;
}

SDL_Event mouseUp(Uint8 button) {
    SDL_Event e     = blankEvent(SDL_EVENT_MOUSE_BUTTON_UP);
    e.button.button = button;
    return e;
}

SDL_Event padDown(const CapturePad& pad, SDL_GamepadButton button) {
    SDL_Event e      = blankEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    e.gbutton.which  = pad.id;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down   = true;
    return e;
}

SDL_Event padUp(const CapturePad& pad, SDL_GamepadButton button) {
    SDL_Event e      = blankEvent(SDL_EVENT_GAMEPAD_BUTTON_UP);
    e.gbutton.which  = pad.id;
    e.gbutton.button = static_cast<Uint8>(button);
    return e;
}

// An axis event for `pad`. The capture reads the pad's readings, not the event's value, so the event
// carries only which axis moved.
SDL_Event padAxis(const CapturePad& pad, SDL_GamepadAxis axis) {
    SDL_Event e   = blankEvent(SDL_EVENT_GAMEPAD_AXIS_MOTION);
    e.gaxis.which = pad.id;
    e.gaxis.axis  = static_cast<Uint8>(axis);
    return e;
}

std::span<const CapturePad> one(const CapturePad& pad) { return {&pad, 1}; }

// ── padButtonFrom ─────────────────────────────────────────────────────────────────────────────────

TEST(PadButtonFrom, EveryDigitalButtonRoundTripsThroughResolve) {
    // Every PadButton value resolvePadButton answers a button for, except the four printed-letter
    // aliases, comes back as itself — so a control added to resolvePadButton and not to padButtonFrom
    // fails here.
    int visited = 0;
    for (int v = 0; v <= 255; ++v) {
        const auto b = static_cast<PadButton>(v);
        if (b == PadButton::FaceLabelA || b == PadButton::FaceLabelB || b == PadButton::FaceLabelX ||
            b == PadButton::FaceLabelY) {
            continue;
        }
        const SDL_GamepadButton sdl = resolvePadButton(b, ControllerType::Standard);
        if (sdl == SDL_GAMEPAD_BUTTON_INVALID) continue;
        ++visited;
        EXPECT_EQ(padButtonFrom(sdl), std::optional<PadButton>{b}) << "PadButton value " << v;
    }
    EXPECT_EQ(visited, 16);  // the four faces, the d-pad, shoulders, stick clicks, Start, Select, Guide, Share
}

TEST(PadButtonFrom, AnswersThePositionNeverALabel) {
    EXPECT_EQ(padButtonFrom(SDL_GAMEPAD_BUTTON_SOUTH), std::optional<PadButton>{PadButton::FaceSouth});
    EXPECT_EQ(padButtonFrom(SDL_GAMEPAD_BUTTON_EAST), std::optional<PadButton>{PadButton::FaceEast});
    EXPECT_EQ(padButtonFrom(SDL_GAMEPAD_BUTTON_WEST), std::optional<PadButton>{PadButton::FaceWest});
    EXPECT_EQ(padButtonFrom(SDL_GAMEPAD_BUTTON_NORTH), std::optional<PadButton>{PadButton::FaceNorth});
}

TEST(PadButtonFrom, AButtonOutsideTheVocabularyAnswersNothing) {
    EXPECT_FALSE(padButtonFrom(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1).has_value());
    EXPECT_FALSE(padButtonFrom(SDL_GAMEPAD_BUTTON_TOUCHPAD).has_value());
    EXPECT_FALSE(padButtonFrom(SDL_GAMEPAD_BUTTON_INVALID).has_value());
}

// ── The capture ───────────────────────────────────────────────────────────────────────────────────

TEST(SourceCapture, NothingIsCapturedUntilRequested) {
    SourceCapture cap;
    EXPECT_FALSE(cap.anyArmed());
    cap.event(keyDown(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    cap.event(mouseDown(SDL_BUTTON_LEFT), 0, nullptr, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());
    EXPECT_FALSE(cap.anyArmed());
}

TEST(SourceCapture, AKeyPressAfterTheRequestIsCaptured) {
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);
    EXPECT_TRUE(cap.anyArmed());
    EXPECT_FALSE(cap.answer(0).has_value());  // armed, no press yet

    cap.event(keyDown(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    const auto press = cap.answer(0);
    ASSERT_TRUE(press.has_value());
    EXPECT_EQ(press->source, Source{SDL_SCANCODE_F9});
    EXPECT_EQ(press->device, kKeyboardMouse);
    EXPECT_FALSE(cap.anyArmed());
}

TEST(SourceCapture, AKeyRepeatIsNotAPress) {
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_F9, /*repeat=*/true), 0, nullptr, kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_UNKNOWN), 0, nullptr, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());
    EXPECT_TRUE(cap.anyArmed());

    cap.event(keyDown(SDL_SCANCODE_G), 0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{SDL_SCANCODE_G});
}

TEST(SourceCapture, TheFirstPressWinsAndTheAnswerStays) {
    CapturePad pad{.id = 3, .slot = 0, .family = ControllerType::Xbox};
    SourceCapture cap;
    cap.request(0, one(pad), kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_A), 0, nullptr, kDefaultResponse);
    cap.event(padDown(pad, SDL_GAMEPAD_BUTTON_SOUTH), 0, &pad, kDefaultResponse);

    const auto first = cap.answer(0);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->source, Source{SDL_SCANCODE_F9});
    EXPECT_EQ(cap.answer(0)->source, first->source);  // reading it takes nothing away
    EXPECT_EQ(cap.answer(0)->device, first->device);
}

TEST(SourceCapture, ANewRequestClearsTheAnswer) {
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());

    cap.request(0, {}, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());
    cap.event(keyDown(SDL_SCANCODE_A), 0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{SDL_SCANCODE_A});
}

TEST(SourceCapture, AMouseButtonIsCaptured) {
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);
    cap.event(mouseDown(SDL_BUTTON_X1), 0, nullptr, kDefaultResponse);  // outside MouseButton
    EXPECT_FALSE(cap.answer(0).has_value());

    cap.event(mouseDown(SDL_BUTTON_RIGHT), 0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{MouseButton::Right});
    EXPECT_EQ(cap.answer(0)->device, kKeyboardMouse);
}

TEST(SourceCapture, APadButtonIsCapturedByPositionWithItsFamilyOnTheDevice) {
    CapturePad pad{.id = 7, .slot = 0, .family = ControllerType::Nintendo};
    SourceCapture cap;
    cap.request(0, one(pad), kDefaultResponse);
    cap.event(padDown(pad, SDL_GAMEPAD_BUTTON_EAST), 0, &pad, kDefaultResponse);

    const auto press = cap.answer(0);
    ASSERT_TRUE(press.has_value());
    EXPECT_EQ(press->source, Source{PadButton::FaceEast});  // the position, not the printed A
    EXPECT_FALSE(press->source.family.has_value());
    EXPECT_EQ(press->device,
              (ActiveDevice{.kind = DeviceKind::Gamepad, .family = ControllerType::Nintendo}));
}

TEST(SourceCapture, ATriggerIsCapturedOnItsRisingCrossing) {
    // A zero dead-zone passes the raw pull through, so the readings sit exactly on either side of
    // kTriggerThreshold.
    AnalogResponse response{};
    response.rightTrigger.deadZone = 0.0f;
    CapturePad pad{.id = 4, .slot = 0, .family = ControllerType::Xbox};
    SourceCapture cap;
    cap.request(0, one(pad), response);

    pad.rawTriggerR = 0.29f;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, &pad, response);
    EXPECT_FALSE(cap.answer(0).has_value());

    pad.rawTriggerR = kTriggerThreshold;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, &pad, response);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{PadButton::TriggerR});
}

TEST(SourceCapture, ATriggerAlreadyPulledAtTheRequestIsNotAPress) {
    CapturePad pad{.id = 4, .slot = 0, .family = ControllerType::Xbox, .rawTriggerR = 1.0f};
    SourceCapture cap;
    cap.request(0, one(pad), kDefaultResponse);

    pad.rawLeftX = 0.05f;  // another axis moves while the trigger stays pulled
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0, &pad, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());

    pad.rawTriggerR = 0.0f;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, &pad, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());  // a release is not a press

    pad.rawTriggerR = 1.0f;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, &pad, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{PadButton::TriggerR});
}

TEST(SourceCapture, AStickDirectionIsCapturedPastItsThreshold) {
    CapturePad pad{.id = 5, .slot = 0, .family = ControllerType::PlayStation};
    SourceCapture cap;
    cap.request(0, one(pad), kDefaultResponse);

    pad.rawLeftX = -0.3f;  // past the dead-zone, short of kStickDirThreshold once rescaled
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0, &pad, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());

    pad.rawLeftX = -1.0f;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0, &pad, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{PadButton::LeftStickLeft});

    pad.rawLeftX = 0.0f;
    cap.request(0, one(pad), kDefaultResponse);
    pad.rawRightY = 1.0f;
    cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), 0, &pad, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{PadButton::RightStickDown});
}

TEST(SourceCapture, TheLargerDeflectionNamesADiagonal) {
    // Both directions cross in one event each time; the larger one names the press, whichever
    // enumerator comes first.
    const auto diagonal = [](float x, float y) {
        CapturePad pad{.id = 6, .slot = 0, .family = ControllerType::Xbox};
        SourceCapture cap;
        cap.request(0, one(pad), kDefaultResponse);
        pad.rawLeftX = x;
        pad.rawLeftY = y;
        cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), 0, &pad, kDefaultResponse);
        return cap.answer(0);
    };

    const auto mostlyLeft = diagonal(-0.9f, -0.6f);
    ASSERT_TRUE(mostlyLeft.has_value());
    EXPECT_EQ(mostlyLeft->source, Source{PadButton::LeftStickLeft});

    const auto mostlyUp = diagonal(-0.6f, -0.9f);
    ASSERT_TRUE(mostlyUp.has_value());
    EXPECT_EQ(mostlyUp->source, Source{PadButton::LeftStickUp});

    const auto even = diagonal(-0.8f, -0.8f);  // an exact tie: the lower enumerator
    ASSERT_TRUE(even.has_value());
    EXPECT_EQ(even->source, Source{PadButton::LeftStickUp});
}

TEST(SourceCapture, TheAnalogResponseDecidesTheCrossing) {
    // One raw reading: past kStickDirThreshold under the default dead-zone (0.15), short of it under
    // a dead-zone of 0.5.
    AnalogResponse wide{};
    wide.leftStick.deadZone.size = 0.5f;
    const auto pushLeft = [](const AnalogResponse& response) {
        CapturePad pad{.id = 8, .slot = 0, .family = ControllerType::Xbox};
        SourceCapture cap;
        cap.request(0, one(pad), response);
        pad.rawLeftX = -0.7f;
        cap.event(padAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0, &pad, response);
        return cap.answer(0);
    };

    const auto underDefault = pushLeft(kDefaultResponse);
    ASSERT_TRUE(underDefault.has_value());
    EXPECT_EQ(underDefault->source, Source{PadButton::LeftStickLeft});
    EXPECT_FALSE(pushLeft(wide).has_value());
}

TEST(SourceCapture, APressIsRoutedByItsDevicesSlot) {
    CapturePad padOnOne{.id = 9, .slot = 1, .family = ControllerType::Xbox};
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);

    cap.event(padDown(padOnOne, SDL_GAMEPAD_BUTTON_SOUTH), 0, &padOnOne, kDefaultResponse);
    cap.event(keyDown(SDL_SCANCODE_F9), /*keyboardSlot=*/1, nullptr, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());
    EXPECT_TRUE(cap.anyArmed());

    cap.event(keyDown(SDL_SCANCODE_F10), /*keyboardSlot=*/0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{SDL_SCANCODE_F10});

    cap.request(1, one(padOnOne), kDefaultResponse);
    cap.event(padDown(padOnOne, SDL_GAMEPAD_BUTTON_SOUTH), 0, &padOnOne, kDefaultResponse);
    ASSERT_TRUE(cap.answer(1).has_value());
    EXPECT_EQ(cap.answer(1)->source, Source{PadButton::FaceSouth});
}

TEST(SourceCapture, SlotsCaptureIndependently) {
    CapturePad padOnOne{.id = 10, .slot = 1, .family = ControllerType::PlayStation};
    SourceCapture cap;
    cap.request(0, {}, kDefaultResponse);
    cap.request(1, one(padOnOne), kDefaultResponse);

    cap.event(padDown(padOnOne, SDL_GAMEPAD_BUTTON_NORTH), 0, &padOnOne, kDefaultResponse);
    ASSERT_TRUE(cap.answer(1).has_value());
    EXPECT_EQ(cap.answer(1)->source, Source{PadButton::FaceNorth});
    EXPECT_FALSE(cap.answer(0).has_value());
    EXPECT_TRUE(cap.anyArmed());  // slot 0 still waits

    cap.event(keyDown(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    ASSERT_TRUE(cap.answer(0).has_value());
    EXPECT_EQ(cap.answer(0)->source, Source{SDL_SCANCODE_F9});
    EXPECT_EQ(cap.answer(1)->source, Source{PadButton::FaceNorth});
    EXPECT_FALSE(cap.anyArmed());
}

TEST(SourceCapture, AReleaseIsNotAPress) {
    CapturePad pad{.id = 11, .slot = 0, .family = ControllerType::Xbox};
    SourceCapture cap;
    cap.request(0, one(pad), kDefaultResponse);
    cap.event(keyUp(SDL_SCANCODE_F9), 0, nullptr, kDefaultResponse);
    cap.event(mouseUp(SDL_BUTTON_LEFT), 0, nullptr, kDefaultResponse);
    cap.event(padUp(pad, SDL_GAMEPAD_BUTTON_SOUTH), 0, &pad, kDefaultResponse);
    EXPECT_FALSE(cap.answer(0).has_value());
    EXPECT_TRUE(cap.anyArmed());
}

}  // namespace
}  // namespace retropp
