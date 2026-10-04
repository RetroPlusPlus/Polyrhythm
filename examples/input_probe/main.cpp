// Input probe — a live diagnostic for the action-based input system. It binds one of everything the
// binding surface offers — every face button (positional AND printed-letter), d-pad, shoulders,
// triggers, stick clicks, Start/Select, both sticks, keyboard keys, and mouse buttons — and shows it
// working against real hardware, so a keyboard + any controller (Xbox / PlayStation / Nintendo /
// generic) can be verified by hand:
//
//   • two rows of blocks, one per digital action — lit while the action is held, from ANY of its
//     sources at once;
//   • a dot driven by the Move VECTOR — the left stick and the arrows/WASD/d-pad feed the same
//     read (stick = analog deflection, keys = unit steps);
//   • an amber Aim pointer orbiting the dot on the right stick — the twin-stick pairing;
//   • a throttle bar driven by the left trigger's AXIS value;
//   • an active-device swatch — white while the keyboard/mouse last produced input, a family
//     colour while a pad did (green Xbox, blue PlayStation, red Nintendo, orange generic) — the
//     signal a game's glyph layer reads;
//   • a mouse marker at the viewport cursor while the pointer is over the drawn area;
//   • a gate box plotting the left stick in throw space — a dim dot for the raw throw and a bright dot
//     for the processed value, with M cycling the left-stick gate (Round / Square / Scaled) so the
//     circle→square remap is visible (the bright dot reaches the corners under Square);
//   • a vibration mode (R toggles it; top-right swatch lights magenta while ON) that declares the pad's
//     motor state from the live analog inputs each tick — left stick → the big motor, right stick → the
//     small motor, each trigger → its own trigger motor — so the OUTPUT half of the pad is felt, not
//     just drawn (the whole input→output loop in one object);
//   • a rebind mode: an amber outline sits on one digital block (F1 / F3 move it); F2 asks the
//     platform to capture the next press and turns the block magenta while it listens; the key, mouse
//     button, pad button, trigger or stick direction pressed next replaces that action's bindings, so
//     the block lights from the new source alone — and a direction block's new source moves the dot
//     that way in place of its old keys.
//
// Every action edge and device change also prints to the console, so each physical press can be
// matched to the action it landed on. Swap controllers mid-run: bindings keep working (device-class
// binding), the printed-letter aliases re-resolve to the new family, and the swatch follows.
//
// Close the window to quit.
//
// `input_probe --verify` drives the capture through a live SdlPlatform instead, with synthetic SDL
// events and a virtual gamepad, prints one line per check and exits nonzero on any miss. Where no GPU
// device can be created it prints the reason and exits 77, which CTest reports as skipped.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/vibration.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 320, kViewH = 240;
constexpr int kMapW = 40, kMapH = 30;  // 40×30 8-px tiles cover the 320×240 viewport

// The probe's vocabulary: the first fifteen are the digital blocks (two display rows, in order);
// Move/Aim/Throttle are the valued reads.
enum class Action : std::uint8_t {
    Up, Down, Left, Right, Confirm, Cancel, LabelX, LabelY,          // row 1
    Fire, ShoulderL, ShoulderR, ClickL, ClickR, Select, Pause, GuideBtn, ShareBtn,  // row 2
    Move, Aim, Throttle,
    CycleGate,  // M — cycles the left-stick gate (Round / Square / Scaled) to show it in the gate box
    ToggleRumble,  // R — toggles the vibration mode: the live analog inputs drive the pad's motors
    SelectNext,    // F1 — moves the rebind selection to the next digital block
    Rebind,        // F2 — captures the next press and binds it to the selected block's action
    SelectPrev,    // F3 — moves the rebind selection to the previous digital block
};
constexpr int kDigitalCount = 17;
constexpr int kRowOneCount  = 8;
constexpr std::array<const char*, kDigitalCount> kActionNames{
    "Up", "Down", "Left", "Right", "Confirm(A)", "Cancel(B)", "LabelX", "LabelY",
    "Fire", "ShoulderL", "ShoulderR", "ClickL", "ClickR", "Select", "Pause", "Guide", "Share"};

[[nodiscard]] ScreenSpaceEffect solidFill(Rgba8 colour) {
    return ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour};
}

[[nodiscard]] const char* familyName(ControllerType family) {
    switch (family) {
        case ControllerType::Xbox:        return "Xbox";
        case ControllerType::PlayStation: return "PlayStation";
        case ControllerType::Nintendo:    return "Nintendo";
        case ControllerType::Standard:    return "Standard";
        case ControllerType::Unknown:     return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] Rgba8 deviceColour(ActiveDevice device) {
    if (device.kind == DeviceKind::KeyboardMouse) return Rgba8{235, 235, 235};  // white
    if (device.kind == DeviceKind::Gamepad) {
        switch (device.family) {
            case ControllerType::Xbox:        return Rgba8{60, 200, 80};    // green
            case ControllerType::PlayStation: return Rgba8{70, 110, 245};   // blue
            case ControllerType::Nintendo:    return Rgba8{230, 60, 60};    // red
            default:                          return Rgba8{240, 150, 40};   // orange (generic)
        }
    }
    return Rgba8{60, 64, 80};  // none yet — dark slate
}

[[nodiscard]] const char* padButtonName(PadButton b) {
    switch (b) {
        case PadButton::FaceSouth:       return "FaceSouth";
        case PadButton::FaceEast:        return "FaceEast";
        case PadButton::FaceWest:        return "FaceWest";
        case PadButton::FaceNorth:       return "FaceNorth";
        case PadButton::FaceLabelA:      return "FaceLabelA";
        case PadButton::FaceLabelB:      return "FaceLabelB";
        case PadButton::FaceLabelX:      return "FaceLabelX";
        case PadButton::FaceLabelY:      return "FaceLabelY";
        case PadButton::DpadUp:          return "DpadUp";
        case PadButton::DpadDown:        return "DpadDown";
        case PadButton::DpadLeft:        return "DpadLeft";
        case PadButton::DpadRight:       return "DpadRight";
        case PadButton::ShoulderL:       return "ShoulderL";
        case PadButton::ShoulderR:       return "ShoulderR";
        case PadButton::TriggerL:        return "TriggerL";
        case PadButton::TriggerR:        return "TriggerR";
        case PadButton::StickClickL:     return "StickClickL";
        case PadButton::StickClickR:     return "StickClickR";
        case PadButton::Start:           return "Start";
        case PadButton::Select:          return "Select";
        case PadButton::Guide:           return "Guide";
        case PadButton::Share:           return "Share";
        case PadButton::LeftStickUp:     return "LeftStickUp";
        case PadButton::LeftStickDown:   return "LeftStickDown";
        case PadButton::LeftStickLeft:   return "LeftStickLeft";
        case PadButton::LeftStickRight:  return "LeftStickRight";
        case PadButton::RightStickUp:    return "RightStickUp";
        case PadButton::RightStickDown:  return "RightStickDown";
        case PadButton::RightStickLeft:  return "RightStickLeft";
        case PadButton::RightStickRight: return "RightStickRight";
    }
    return "?";
}

// A source in words, for the console line a rebind prints.
[[nodiscard]] std::string sourceName(const Source& s) {
    switch (s.kind) {
        case Source::Kind::Key:   return std::string{"key "} + SDL_GetScancodeName(s.key);
        case Source::Kind::Pad:   return std::string{"pad "} + padButtonName(s.pad);
        case Source::Kind::Mouse:
            return s.mouse == MouseButton::Left    ? "mouse Left"
                   : s.mouse == MouseButton::Right ? "mouse Right"
                                                   : "mouse Middle";
        case Source::Kind::Stick: return s.stick == PadStick::Left ? "stick Left" : "stick Right";
    }
    return "?";
}

[[nodiscard]] std::string deviceName(ActiveDevice device) {
    if (device.kind == DeviceKind::Gamepad) return std::string{"gamepad ("} + familyName(device.family) + ")";
    return "keyboard/mouse";
}

[[nodiscard]] EngineConfig probeConfig() {
    return EngineConfig{
        .identity = {.organization = "Retro++", .application = "Input Probe"},
        .window   = {.title = "Polyrhythm — input probe (keyboard + any controller)"},
        .viewport = ViewportResolution{kViewW, kViewH}};
}

// One of everything — every pad control is bound to a visible block, so no press on any family's pad
// is silent. Label aliases (Confirm/Cancel/LabelX/LabelY follow the pad's PRINTED letters), a trigger
// as a digital source (Fire past the threshold), shoulders/stick-clicks/Select/Start by their neutral
// names, mouse buttons beside keys and pad on the same actions, and the valued reads (Move + Aim
// vectors — the twin-stick pairing — and the Throttle axis). The digital directional preset and the
// vector preset coexist on purpose: one W press lights the Up block AND moves the dot.
[[nodiscard]] ActionMap probeMap() {
    ActionMap map{
        {Action::Confirm,   {SDL_SCANCODE_RETURN, PadButton::FaceLabelA}},
        {Action::Cancel,    {SDL_SCANCODE_BACKSPACE, PadButton::FaceLabelB, MouseButton::Right}},
        {Action::LabelX,    {SDL_SCANCODE_X, PadButton::FaceLabelX}},
        {Action::LabelY,    {SDL_SCANCODE_Y, PadButton::FaceLabelY}},
        {Action::Fire,      {SDL_SCANCODE_SPACE, MouseButton::Left, PadButton::TriggerR}},
        {Action::ShoulderL, {SDL_SCANCODE_Q, PadButton::ShoulderL}},
        {Action::ShoulderR, {SDL_SCANCODE_E, PadButton::ShoulderR}},
        {Action::ClickL,    {SDL_SCANCODE_Z, PadButton::StickClickL}},
        {Action::ClickR,    {SDL_SCANCODE_C, PadButton::StickClickR}},
        {Action::Select,    {SDL_SCANCODE_TAB, PadButton::Select}},
        {Action::Pause,     {SDL_SCANCODE_P, PadButton::Start}},
        {Action::GuideBtn,  {SDL_SCANCODE_G, PadButton::Guide}},
        {Action::ShareBtn,  {SDL_SCANCODE_V, PadButton::Share}},
        {Action::Aim,       {PadStick::Right}},
        {Action::Throttle,  {PadButton::TriggerL}},
        {Action::CycleGate,    {SDL_SCANCODE_M}},
        {Action::ToggleRumble, {SDL_SCANCODE_R}},
        {Action::SelectNext,   {SDL_SCANCODE_F1}},
        {Action::Rebind,       {SDL_SCANCODE_F2}},
        {Action::SelectPrev,   {SDL_SCANCODE_F3}},
    };
    map.add(presets::directional(Action::Up, Action::Down, Action::Left, Action::Right));
    map.add(presets::directionalVector(Action::Move));
    return map;
}

// The direction a direction block's action names on the Move vector; Dir::None for every other action.
[[nodiscard]] Dir directionOf(Action action) {
    switch (action) {
        case Action::Up:    return Dir::Up;
        case Action::Down:  return Dir::Down;
        case Action::Left:  return Dir::Left;
        case Action::Right: return Dir::Right;
        default:            return Dir::None;
    }
}

// The rebind itself: the captured source replaces every binding the action had. A direction block's
// action shares its keys with the dot's Move action, so rebinding a direction moves it on Move too —
// Move's rows tagged with that direction give way to the captured source, tagged the same way, and
// the whole-stick row stays.
void rebind(ActionMap& map, Action action, const CapturedSource& press) {
    map.clearAction(action);
    map.bind(action, press.source);

    if (const Dir direction = directionOf(action); direction != Dir::None) {
        std::vector<Source> replaced;
        for (const ActionBinding& row : map.rows()) {
            if (row.action == actionId(Action::Move) && row.source.component == direction) {
                replaced.push_back(row.source);
            }
        }
        for (const Source& source : replaced) map.unbind(Action::Move, source);
        map.bind(Action::Move, asComponent(press.source, direction));
    }
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────
// The capture driven through a live SdlPlatform. Keyboard and mouse presses are pushed onto SDL's
// event queue; pad input comes from a virtual gamepad, whose buttons and axes reach the platform's
// pump as a real pad's do. Each check pumps the platform and reads capturedSource / input().

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("input_probe --verify: the platform captures the next press, from synthetic SDL input\n\n");

    std::unique_ptr<SdlPlatform> platform;
    try {
        platform = std::make_unique<SdlPlatform>(probeConfig());
    } catch (const std::runtime_error& e) {
        std::printf("skipped: %s\n", e.what());
        return 77;
    }

    const auto push = [](SDL_Event e) {
        e.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&e);
    };
    const auto blank = [](SDL_EventType type) {
        SDL_Event e;
        std::memset(&e, 0, sizeof e);
        e.type = type;
        return e;
    };
    const auto pushKey = [&](SDL_Scancode scancode, bool repeat) {
        SDL_Event e    = blank(SDL_EVENT_KEY_DOWN);
        e.key.scancode = scancode;
        e.key.key      = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        e.key.down     = true;
        e.key.repeat   = repeat;
        push(e);
    };
    const auto pushMouse = [&](Uint8 button, bool down) {
        SDL_Event e     = blank(down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP);
        e.button.button = button;
        e.button.down   = down;
        push(e);
    };
    // Pump until `ready` holds, a few pumps at most; answer whether it held. The pump it held on is
    // the platform's latest, so input() reads that pump's sample.
    const auto pumpUntil = [&](auto ready) {
        for (int i = 0; i < 8; ++i) {
            platform->pumpEvents();
            if (ready()) return true;
        }
        return false;
    };
    const auto pumpSome = [&] {
        for (int i = 0; i < 4; ++i) platform->pumpEvents();
    };
    const auto answerIs = [&](int player, const Source& source) {
        const auto press = platform->capturedSource(player);
        return press.has_value() && press->source == source;
    };

    // ── Keyboard and mouse ──
    pushKey(SDL_SCANCODE_F9, false);
    pumpSome();
    check(!platform->capturedSource().has_value(), "with no request, a key press is not captured");

    platform->captureRequest();
    pushKey(SDL_SCANCODE_F9, false);
    pumpUntil([&] { return platform->capturedSource().has_value(); });
    const std::optional<CapturedSource> f9 = platform->capturedSource();
    check(answerIs(0, Source{SDL_SCANCODE_F9}) &&
              f9->device == ActiveDevice{.kind = DeviceKind::KeyboardMouse, .family = ControllerType::Unknown},
          "after captureRequest, the next key press is the answer, from the keyboard");

    pushKey(SDL_SCANCODE_A, false);
    pumpSome();
    check(answerIs(0, Source{SDL_SCANCODE_F9}), "a second key press does not replace it");
    platform->pumpEvents();
    check(answerIs(0, Source{SDL_SCANCODE_F9}), "and it reads the same on a later pump");

    platform->captureRequest();
    check(!platform->capturedSource().has_value(), "a new request clears the answer");
    pushKey(SDL_SCANCODE_F10, /*repeat=*/true);
    pumpSome();
    check(!platform->capturedSource().has_value(), "a key repeat is not a press");

    ActionMap map = probeMap();
    rebind(map, Action::Fire, *f9);
    int fireRows = 0;
    bool fireIsF9 = true;
    for (const ActionBinding& row : map.rows()) {
        if (row.action != actionId(Action::Fire)) continue;
        ++fireRows;
        fireIsF9 = fireIsF9 && row.source == Source{SDL_SCANCODE_F9};
    }
    check(fireRows == 1 && fireIsF9, "rebind leaves Fire one row, the captured key");

    rebind(map, Action::Up, *f9);
    int  upRows = 0, moveUpRows = 0, moveDownRows = 0;
    bool upIsF9 = true, moveUpIsF9 = true, moveKeepsStick = false;
    for (const ActionBinding& row : map.rows()) {
        if (row.action == actionId(Action::Up)) {
            ++upRows;
            upIsF9 = upIsF9 && row.source == Source{SDL_SCANCODE_F9};
        }
        if (row.action != actionId(Action::Move)) continue;
        if (row.source.component == Dir::Up) {
            ++moveUpRows;
            moveUpIsF9 = moveUpIsF9 && row.source == asComponent(SDL_SCANCODE_F9, Dir::Up);
        }
        if (row.source.component == Dir::Down) ++moveDownRows;
        if (row.source == Source{PadStick::Left}) moveKeepsStick = true;
    }
    check(upRows == 1 && upIsF9 && moveUpRows == 1 && moveUpIsF9 && moveDownRows == 3 && moveKeepsStick,
          "rebinding Up moves the dot's up to the captured key too; its other directions and the stick stay");

    // ── A virtual gamepad ──
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes    = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name     = "input_probe virtual gamepad";
    const SDL_JoystickID padId = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick* const  joy   = padId != 0 ? SDL_OpenJoystick(padId) : nullptr;
    const bool opened = joy != nullptr && pumpUntil([&] {
        for (const GamepadInfo& pad : platform->connectedGamepads()) {
            if (pad.id == padId) return true;
        }
        return false;
    });
    check(opened, "a virtual gamepad attaches and the platform opens it");

    if (opened) {
        const auto button = [&](SDL_GamepadButton b, bool down) {
            SDL_SetJoystickVirtualButton(joy, static_cast<int>(b), down);
        };
        const auto axis = [&](SDL_GamepadAxis a, Sint16 value) {
            SDL_SetJoystickVirtualAxis(joy, static_cast<int>(a), value);
        };

        platform->captureRequest();
        button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        pumpUntil([&] { return platform->capturedSource().has_value(); });
        const std::optional<CapturedSource> south = platform->capturedSource();
        check(answerIs(0, Source{PadButton::FaceSouth}) && !south->source.family.has_value() &&
                  south->device.kind == DeviceKind::Gamepad,
              "the pad's south button is FaceSouth, with no family on the source, from a gamepad");
        button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        pumpSome();

        axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
        pumpSome();
        platform->captureRequest();
        axis(SDL_GAMEPAD_AXIS_LEFTX, 8000);
        pumpSome();
        check(!platform->capturedSource().has_value(),
              "a trigger already pulled at the request is not a press when another axis moves");
        axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        pumpSome();
        axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
        pumpUntil([&] { return platform->capturedSource().has_value(); });
        check(answerIs(0, Source{PadButton::TriggerR}), "released and pulled again, it is TriggerR");
        axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
        pumpSome();

        platform->captureRequest();
        axis(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
        pumpUntil([&] { return platform->capturedSource().has_value(); });
        check(answerIs(0, Source{PadButton::LeftStickLeft}), "the left stick pushed full left is LeftStickLeft");
        axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
        pumpSome();

        // Slots, and the captured press still driving its action.
        platform->actions(ActionMap{
            {Action::Fire,   {PadButton::FaceSouth}},
            {Action::Cancel, {MouseButton::Right}},
        });
        platform->assignGamepad(padId, 1);
        platform->captureRequest(0);
        button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        pumpSome();
        check(!platform->capturedSource(0).has_value(),
              "a pad routed to slot 1 is not seen by slot 0's request");
        button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        pumpSome();

        platform->captureRequest(1);
        button(SDL_GAMEPAD_BUTTON_SOUTH, true);
        pumpUntil([&] { return platform->capturedSource(1).has_value(); });
        const PlayerSample& slotOne = platform->input().players[1];
        check(answerIs(1, Source{PadButton::FaceSouth}), "slot 1's request answers its pad's press");
        check(slotOne.held.test(actionId(Action::Fire)) && slotOne.device.kind == DeviceKind::Gamepad,
              "the captured press still drives Fire and moves slot 1's active device, on the same pump");
        button(SDL_GAMEPAD_BUTTON_SOUTH, false);
        pumpSome();

        platform->captureRequest(0);
        pushMouse(SDL_BUTTON_RIGHT, true);
        pumpUntil([&] { return platform->capturedSource(0).has_value(); });
        check(answerIs(0, Source{MouseButton::Right}) &&
                  platform->input().players[0].held.test(actionId(Action::Cancel)),
              "a captured right mouse button is the answer and still drives Cancel, on the same pump");
        pushMouse(SDL_BUTTON_RIGHT, false);
        pumpSome();
    }

    if (joy != nullptr) SDL_CloseJoystick(joy);
    if (padId != 0) SDL_DetachVirtualJoystick(padId);

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config = probeConfig();
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map = probeMap();
    platform.actions(map);

    // An opaque dim-grid backdrop (in-code art — the probe ships no assets).
    std::array<std::uint8_t, 64> gridArt{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            gridArt[static_cast<std::size_t>(y) * 8 + x] = (x == 0 || y == 0) ? 2 : 1;
    const AtlasId gridAtlas = renderer.uploadAtlas(gridArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 3> gridPal{{{0, 0, 0}, {26, 28, 40}, {38, 42, 60}}};
    const PaletteId gridPalId = renderer.uploadPalette(std::span<const Rgba8>(gridPal));
    const std::vector<TileCell> gridCells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = gridAtlas, .tile = 0, .palette = gridPalId});

    // The left-stick gate cycles Round → Square → Scaled on the M key, so the gate box shows the same
    // raw throw mapped three ways. Scaled sits partway to Square (gateScale below).
    constexpr std::array<GateShape, 3> kGates{GateShape::Round, GateShape::Square, GateShape::Scaled};
    constexpr std::array<const char*, 3> kGateNames{"Round", "Square", "Scaled(1.3)"};
    int gateMode = 0;
    auto applyGate = [&] {
        AnalogResponse response{};
        response.leftStick.gate      = kGates[static_cast<std::size_t>(gateMode)];
        response.leftStick.gateScale = 1.3f;  // read only for Scaled
        platform.analogResponse(response);
    };
    applyGate();

    // Probe state the tick writes and the render reads.
    std::array<bool, kDigitalCount> held{};
    Vec2  dotPos{80.0f, 160.0f};
    Vec2  aim{};
    float throttle    = 0.0f;
    Vec2  stickProc{};   // left stick after the gate — reaches the box corners under Square
    Vec2  stickRawL{};   // left stick raw — stays on the inscribed circle
    ActiveDevice device{};
    Vec2i cursor{};
    bool  cursorOn    = false;
    ActiveDevice lastPrinted{};
    std::size_t  lastPadCount = 0;
    bool         rumbleOn     = false;  // vibration mode: while ON, the live analog inputs drive the motors
    int          selected     = 0;      // the digital block the rebind mode targets
    bool         listening    = false;  // a capture is requested and its press has not arrived

    loop.simTick([&](const InputState& in) {
        // Digital blocks + console edges.
        for (int i = 0; i < kDigitalCount; ++i) {
            const auto a = static_cast<Action>(i);
            held[static_cast<std::size_t>(i)] = in.isHeld(a);
            if (in.justPressed(a)) std::printf("pressed  %s\n", kActionNames[static_cast<std::size_t>(i)]);
            if (in.justReleased(a)) std::printf("released %s\n", kActionNames[static_cast<std::size_t>(i)]);
        }

        // The Move vector drives the dot (stick deflection is proportional; keys are unit steps);
        // the dot stays inside its box.
        const Vec2 move = in.vector(Action::Move);
        dotPos.x = std::clamp(dotPos.x + move.x * 2.0f, 12.0f, 148.0f);
        dotPos.y = std::clamp(dotPos.y + move.y * 2.0f, 96.0f, 228.0f);

        // Cycle the left-stick gate and reapply the config.
        if (in.justPressed(Action::CycleGate)) {
            gateMode = (gateMode + 1) % 3;
            applyGate();
            std::printf("left-stick gate -> %s\n", kGateNames[static_cast<std::size_t>(gateMode)]);
        }
        // The processed left stick (post-gate) beside its raw throw — the gate box plots both.
        stickProc = in.stick(Stick::Left);
        stickRawL = in.stickRaw(Stick::Left);

        aim      = in.vector(Action::Aim);
        throttle = in.axis(Action::Throttle);
        device   = in.activeDevice();
        cursor   = in.cursor();
        cursorOn = in.cursorOnScreen();

        // Vibration mode: toggle on R, and while ON declare the pad's motor state from the live analog
        // inputs every tick — the whole input→output loop in one object (feel the same values drawn).
        // Left-stick deflection → the big (low) motor, right-stick → the small (high) motor, each
        // analog trigger → its own trigger motor. While OFF the probe declares nothing (the no-call
        // tick = silence path), so releasing the toggle stops the motors on the next tick.
        if (in.justPressed(Action::ToggleRumble)) {
            rumbleOn = !rumbleOn;
            std::printf("vibration mode -> %s\n", rumbleOn ? "ON" : "OFF");
        }
        if (rumbleOn) {
            const auto magnitude = [](Vec2 v) {
                return std::clamp(std::sqrt(v.x * v.x + v.y * v.y), 0.0f, 1.0f);
            };
            const auto toByte = [](float f) {
                return static_cast<std::uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f);
            };
            platform.gamepad(0).vibration({
                .low          = toByte(magnitude(in.stick(Stick::Left))),
                .high         = toByte(magnitude(in.stick(Stick::Right))),
                .triggerLeft  = toByte(in.trigger(Trigger::Left)),
                .triggerRight = toByte(in.trigger(Trigger::Right)),
            });
        }

        // Rebind mode. While listening, the press the platform captured replaces every binding of the
        // selected action; otherwise F1 / F3 move the selection and F2 asks for a capture. The F2 press
        // itself is already down when the request is made, so it is never the answer.
        if (listening) {
            if (const std::optional<CapturedSource> press = platform.capturedSource()) {
                rebind(map, static_cast<Action>(selected), *press);
                platform.actions(map);
                listening = false;
                std::printf("rebound %s -> %s, from %s\n", kActionNames[static_cast<std::size_t>(selected)],
                            sourceName(press->source).c_str(), deviceName(press->device).c_str());
            }
        } else {
            if (in.justPressed(Action::SelectNext)) selected = (selected + 1) % kDigitalCount;
            if (in.justPressed(Action::SelectPrev)) selected = (selected + kDigitalCount - 1) % kDigitalCount;
            if (in.justPressed(Action::Rebind)) {
                platform.captureRequest();
                listening = true;
                std::printf("rebind %s: press a key, mouse button, pad button, trigger or stick direction\n",
                            kActionNames[static_cast<std::size_t>(selected)]);
            }
        }

        // Console: device transitions + pad connect/disconnect.
        if (device != lastPrinted) {
            if (device.kind == DeviceKind::KeyboardMouse) {
                std::printf("active device -> keyboard/mouse\n");
            } else if (device.kind == DeviceKind::Gamepad) {
                std::printf("active device -> gamepad (%s)\n", familyName(device.family));
            }
            lastPrinted = device;
        }
        const auto pads = platform.connectedGamepads();
        if (pads.size() != lastPadCount) {
            std::printf("connected gamepads: %zu\n", pads.size());
            for (const GamepadInfo& pad : pads) {
                std::printf("  id %u — %s — player %d\n", static_cast<unsigned>(pad.id),
                            familyName(pad.family), pad.slot);
            }
            lastPadCount = pads.size();
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer bg{.key = "backgroundGrid"};
        bg.z       = -10;
        bg.size    = PixelSize{kViewW, kViewH};
        bg.content = TileContent{.widthInTiles = kMapW, .heightInTiles = kMapH,
                                 .cells = std::span<const TileCell>(gridCells)};
        frame.layers.push_back(bg);

        frame.regions.clear();

        // The active-device swatch (top-left).
        frame.regions.push_back(Region{.key = "device",
                                       .shape = ShapePoints::rectangle(Point{8, 6}, 40, 12),
                                       .effects = {solidFill(deviceColour(device))}});

        // The vibration-mode indicator (top-right): magenta while ON (motors driven by the analog
        // inputs), dark slate while OFF. Press R to toggle.
        frame.regions.push_back(
            Region{.key = "rumble",
                   .shape = ShapePoints::rectangle(Point{kViewW - 48, 6}, 40, 12),
                   .effects = {solidFill(rumbleOn ? Rgba8{235, 60, 200} : Rgba8{52, 56, 74})}});

        // The digital blocks, two rows: dim slate when idle, bright cyan while held, magenta while the
        // rebind mode listens for the selected one's new source. An amber outline marks the selection.
        static constexpr std::array<const char*, kDigitalCount> kBlockKeys{
            "blkUp", "blkDown", "blkLeft", "blkRight", "blkConfirm", "blkCancel", "blkLX", "blkLY",
            "blkFire", "blkShL", "blkShR", "blkClkL", "blkClkR", "blkSelect", "blkPause",
            "blkGuide", "blkShare"};
        const auto blockOrigin = [](int i) {
            const bool rowOne = i < kRowOneCount;
            const int  column = rowOne ? i : i - kRowOneCount;
            return Point{8.0f + static_cast<float>(column) * 26.0f, rowOne ? 26.0f : 52.0f};
        };
        for (int i = 0; i < kDigitalCount; ++i) {
            Rgba8 color = held[static_cast<std::size_t>(i)] ? Rgba8{40, 220, 255} : Rgba8{52, 56, 74};
            if (listening && i == selected) color = Rgba8{235, 60, 200};
            frame.regions.push_back(Region{.key = kBlockKeys[static_cast<std::size_t>(i)],
                                           .shape = ShapePoints::rectangle(blockOrigin(i), 20, 20),
                                           .effects = {solidFill(color)}});
        }
        const Point selectedAt = blockOrigin(selected);
        ShapePoints selection  = ShapePoints::rectangle(Point{selectedAt.x - 3.0f, selectedAt.y - 3.0f}, 26, 26);
        selection.strokeWidth  = 2.0f;
        frame.regions.push_back(Region{.key = "selection", .shape = selection,
                                       .effects = {solidFill(Rgba8{255, 180, 40})}});

        // The Move box + dot + Aim pointer.
        ShapePoints moveBox = ShapePoints::rectangle(Point{8, 92}, 144, 140);
        moveBox.strokeWidth = 2.0f;
        frame.regions.push_back(Region{.key = "moveBox", .shape = moveBox,
                                       .effects = {solidFill(Rgba8{70, 76, 100})}});
        frame.regions.push_back(
            Region{.key = "dot",
                   .shape = ShapePoints::rectangle(Point{dotPos.x - 3.0f, dotPos.y - 3.0f}, 6, 6),
                   .effects = {solidFill(Rgba8{255, 255, 255})}});
        // The Aim vector (right stick): an amber pointer orbiting the move dot in the aimed
        // direction — the twin-stick pairing (Move walks, Aim points the guns).
        if (aim.x * aim.x + aim.y * aim.y > 0.02f) {
            frame.regions.push_back(
                Region{.key = "aim",
                       .shape = ShapePoints::rectangle(
                           Point{dotPos.x + aim.x * 22.0f - 2.0f, dotPos.y + aim.y * 22.0f - 2.0f}, 4, 4),
                       .effects = {solidFill(Rgba8{255, 180, 40})}});
        }

        // The throttle bar (left trigger): a fixed track + a fill proportional to the pull.
        ShapePoints track = ShapePoints::rectangle(Point{170, 92}, 142, 14);
        track.strokeWidth = 2.0f;
        frame.regions.push_back(Region{.key = "throttleTrack", .shape = track,
                                       .effects = {solidFill(Rgba8{70, 76, 100})}});
        const int fillW = static_cast<int>(throttle * 138.0f);
        if (fillW > 0) {
            frame.regions.push_back(Region{.key = "throttleFill",
                                           .shape = ShapePoints::rectangle(Point{172, 94}, fillW, 10),
                                           .effects = {solidFill(Rgba8{255, 180, 40})}});
        }

        // The gate box: a square throw-space plot of the left stick. The dim dot is the raw throw (it
        // stays on the inscribed circle, corners unreachable); the bright dot is the processed value —
        // under Square it reaches the box corners, under Round it tracks the raw dot. A swatch names the
        // current gate (grey Round, green Square, amber Scaled). Press M to cycle.
        constexpr float kGateCx = 250.0f, kGateCy = 170.0f, kGateHalf = 48.0f;
        ShapePoints gateBox = ShapePoints::rectangle(
            Point{kGateCx - kGateHalf, kGateCy - kGateHalf}, kGateHalf * 2.0f, kGateHalf * 2.0f);
        gateBox.strokeWidth = 2.0f;
        frame.regions.push_back(
            Region{.key = "gateBox", .shape = gateBox, .effects = {solidFill(Rgba8{70, 76, 100})}});
        const std::array<Rgba8, 3> kGateSwatch{{{120, 124, 140}, {60, 200, 80}, {240, 150, 40}}};
        frame.regions.push_back(
            Region{.key = "gateSwatch",
                   .shape = ShapePoints::rectangle(Point{kGateCx - kGateHalf, kGateCy - kGateHalf - 12.0f}, 14, 8),
                   .effects = {solidFill(kGateSwatch[static_cast<std::size_t>(gateMode)])}});
        frame.regions.push_back(
            Region{.key = "gateRaw",
                   .shape = ShapePoints::rectangle(
                       Point{kGateCx + stickRawL.x * kGateHalf - 2.0f, kGateCy + stickRawL.y * kGateHalf - 2.0f}, 4, 4),
                   .effects = {solidFill(Rgba8{90, 96, 120})}});
        frame.regions.push_back(
            Region{.key = "gateProc",
                   .shape = ShapePoints::rectangle(
                       Point{kGateCx + stickProc.x * kGateHalf - 3.0f, kGateCy + stickProc.y * kGateHalf - 3.0f}, 6, 6),
                   .effects = {solidFill(Rgba8{40, 220, 255})}});

        // The mouse marker, while the pointer is over the drawn viewport.
        if (cursorOn) {
            frame.regions.push_back(Region{.key = "mouse",
                                           .shape = ShapePoints::rectangle(
                                               Point{static_cast<float>(cursor.x - 2),
                                                     static_cast<float>(cursor.y - 2)}, 4, 4),
                                           .effects = {solidFill(Rgba8{120, 255, 120})}});
        }

        renderer.renderFrame(frame);
    });

    std::printf(
        "input probe — a block per digital action (lit while held; row 1 above row 2), a dot on the\n"
        "Move vector, an amber Aim pointer on the right stick, a throttle bar on the left trigger,\n"
        "an active-device swatch (white = keyboard/mouse; green = Xbox pad, blue = PlayStation,\n"
        "red = Nintendo, orange = generic), and a marker on the mouse cursor. Bindings:\n"
        "  row 1: Up Down Left Right   arrows + WASD + d-pad\n"
        "         Confirm              Return + the pad button PRINTED A\n"
        "         Cancel               Backspace + right mouse button + the pad button PRINTED B\n"
        "         LabelX / LabelY      X / Y keys + the pad buttons PRINTED X / Y\n"
        "  row 2: Fire                 Space + left mouse button + right trigger (past threshold)\n"
        "         ShoulderL/R          Q / E + the pad shoulders (LB·L1·L / RB·R1·R)\n"
        "         ClickL/R             Z / C + the stick clicks (L3 / R3)\n"
        "         Select               Tab + the pad's Select/Back/Minus\n"
        "         Pause                P + Start/Menu/Plus\n"
        "         Guide                G + the Xbox guide / PS button / Switch Home (the OS or\n"
        "                              Steam may intercept it before the engine ever sees it)\n"
        "         Share                V + Share (Xbox) / Create (PS5) / Capture (Switch)\n"
        "  Move (dot)                  left stick + arrows/WASD/d-pad as a vector\n"
        "  Aim (amber pointer)         right stick — the twin-stick pairing with Move\n"
        "  Throttle (bar)              left trigger, analog\n"
        "  Gate box (right)            left stick in throw space — dim dot raw, bright dot processed\n"
        "  M                           cycle the left-stick gate: Round / Square / Scaled\n"
        "  R                           toggle vibration mode (top-right swatch): while ON, the left\n"
        "                              stick drives the big motor, the right stick the small motor,\n"
        "                              and each trigger its own trigger motor — feel what you move\n"
        "  F1 / F3                     move the rebind selection (amber outline) to the next /\n"
        "                              previous block\n"
        "  F2                          rebind the selected block: it turns magenta, and the next key,\n"
        "                              mouse button, pad button, trigger or stick direction you press\n"
        "                              becomes its only binding (a direction block's new source moves\n"
        "                              the dot that way, in place of its old keys)\n"
        "Every edge and device change prints here. Swap controllers mid-run — everything keeps\n"
        "working. Close the window to quit.\n\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
