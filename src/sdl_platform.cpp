#include "retropp/sdl_platform.h"

#include <SDL3/SDL_main.h>  // SDL_SetMainReady — the engine owns the entry-point handshake (SDL_MAIN_HANDLED)

#include "sdl_platform_testing.h"
#include "source_capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace retropp {

namespace {
[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string{what} + ": " + SDL_GetError());
}

// Raw normalized readings: a stick axis (SDL Sint16, [-32768, 32767]) to [-1, 1]; a trigger to [0, 1].
// These are the untouched hardware values — the configured AnalogResponse (dead-zone + gate) is applied
// on top, by applyStickResponse / applyTriggerResponse. Y follows SDL: down is positive.
float rawStickAxis(Sint16 raw) noexcept {
    return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
}
float rawTriggerAxis(Sint16 raw) noexcept {
    return std::clamp(static_cast<float>(raw) / 32767.0f, 0.0f, 1.0f);
}

// The raw-axis magnitude past which a gamepad axis event counts as device ACTIVITY for the
// active-device signal (matches the stick dead-zone: 0.15 × 32767).
constexpr Sint16 kAxisActivity = 4915;

// Keep the larger-magnitude value — the per-axis aggregation across a slot's pads for the raw
// stick/trigger reads (deterministic; no "first pad wins" surprise).
float maxMagnitude(float a, float b) noexcept {
    return std::abs(b) > std::abs(a) ? b : a;
}

using detail::stickDirValue;

// One open pad as the capture reads it: its routing, its family, and its six raw analog readings.
detail::CapturePad capturePad(SDL_Gamepad* handle, SDL_JoystickID id, int slot,
                              ControllerType family) noexcept {
    return detail::CapturePad{
        .id          = id,
        .slot        = slot,
        .family      = family,
        .rawLeftX    = rawStickAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFTX)),
        .rawLeftY    = rawStickAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFTY)),
        .rawRightX   = rawStickAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHTX)),
        .rawRightY   = rawStickAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHTY)),
        .rawTriggerL = rawTriggerAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
        .rawTriggerR = rawTriggerAxis(SDL_GetGamepadAxis(handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)),
    };
}

// Fold one down/valued source into a per-action value: a plain digital source contributes 1 on x, a
// trigger its pull on x; a component-tagged source contributes ±magnitude on its axis (up = -y, the
// stick convention).
void contribute(Vec2& value, Dir component, float magnitude) noexcept {
    switch (component) {
        case Dir::None:  value.x += magnitude; break;
        case Dir::Up:    value.y -= magnitude; break;
        case Dir::Down:  value.y += magnitude; break;
        case Dir::Left:  value.x -= magnitude; break;
        case Dir::Right: value.x += magnitude; break;
    }
}
}  // namespace

SdlPlatform::SdlPlatform(const EngineConfig& config) : SdlPlatform(config, Devices::All) {}

std::unique_ptr<SdlPlatform> detail::SdlPlatformTestAccess::inputOnly(const EngineConfig& config) {
    return std::unique_ptr<SdlPlatform>(new SdlPlatform(config, SdlPlatform::Devices::InputOnly));
}

SdlPlatform::SdlPlatform(const EngineConfig& config, Devices devices)
    : viewport_{config.viewport.width, config.viewport.height},
      capture_{std::make_unique<detail::SourceCapture>()} {
    // The engine owns SDL's entry-point handshake: with SDL_MAIN_HANDLED defined (engine build), a game
    // writes a plain main() and the engine acknowledges main-thread readiness here, once, before SDL_Init.
    [[maybe_unused]] static const bool mainReady = [] { SDL_SetMainReady(); return true; }();
    const SDL_InitFlags subsystems = devices == Devices::InputOnly
                                         ? SDL_INIT_VIDEO | SDL_INIT_GAMEPAD
                                         : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO;
    if (!SDL_Init(subsystems)) {
        fail("SDL_Init failed");
    }

    // Window size = viewport × windowScale in LOGICAL points, clamped down so it fits the primary
    // display's usable area (fitWindowScale) — so the window is always an integer multiple of the
    // game's native resolution and never opens larger than the screen, even for a large viewport.
    // The usable bounds are queried before the window exists, off the primary display; a query
    // failure passes a degenerate {0,0} so fitWindowScale falls back to the unclamped target.
    PixelSize usable{};
    if (const SDL_DisplayID disp = SDL_GetPrimaryDisplay(); disp != 0) {
        if (SDL_Rect bounds{}; SDL_GetDisplayUsableBounds(disp, &bounds)) {
            usable = PixelSize{bounds.w, bounds.h};
        }
    }
    const PixelSize vp{config.viewport.width, config.viewport.height};
    const int scale = fitWindowScale(vp, usable, config.enhancements.windowScale);

    // HIGH_PIXEL_DENSITY: the window's drawable is created at the display's PHYSICAL pixel
    // resolution (on a 2× Retina panel, a 640-logical-point window has a 1280-pixel drawable).
    // drawableSize() reports SDL_GetWindowSizeInPixels (true physical pixels) and the blit fills the
    // drawable at the largest integer scale that fits, so the art renders crisp at native resolution.
    // BORDERLESS is OR-ed in at creation (never applied after the fact) when the config asks for it, so
    // an app drawing its own chrome never sees the native title bar/border flash for even one frame.
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.window.suppressNativeWindowChrome) {
        flags |= SDL_WINDOW_BORDERLESS;
        chromeSuppressed_ = true;
    }
    window_ = SDL_CreateWindow(config.window.title.c_str(), vp.width * scale, vp.height * scale, flags);
    if (!window_) {
        SDL_Quit();
        fail("SDL_CreateWindow failed");
    }

    // The window and the input are all an input-only platform has: no GPU device, nothing drawn.
    if (devices == Devices::InputOnly) {
        SDL_SetWindowHitTest(window_, &SdlPlatform::hitTest, this);
        return;
    }

    // Created with every shader format the supported backends accept so SDL picks an
    // available backend (Vulkan/SPIRV, D3D12/DXIL, Metal/MSL). The device requires a
    // valid format set at creation even before any pipeline is bound.
    gpu_ = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!gpu_) {
        SDL_DestroyWindow(window_);
        SDL_Quit();
        fail("SDL_CreateGPUDevice failed");
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu_, window_)) {
        SDL_DestroyGPUDevice(gpu_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
        fail("SDL_ClaimWindowForGPUDevice failed");
    }

    // Pace presentation to the display refresh. VSYNC is SDL_GPU's default and is
    // guaranteed supported, but it is set explicitly because the host loop has no
    // sleep of its own — SDL's present block IS the frame pacer. Without vsync, the
    // pump → advance → present loop would busy-spin a core. A configurable present
    // mode (uncapped / mailbox) is a later concern; the faithful default is vsync.
    SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);

    // Install the draggable-region hit-test once, with `this` as the userdata. The OS window manager
    // consults it during event processing and drags the window when a press lands in a game-declared
    // region. draggableRegions_ is empty until the game declares a set, so this is inert by default.
    SDL_SetWindowHitTest(window_, &SdlPlatform::hitTest, this);

    // Apply the startup fullscreen toggle once. Default (false) leaves the faithful windowed
    // baseline untouched; a host that opts in opens straight into fullscreen.
    if (config.enhancements.fullscreen) {
        fullscreen(true);
    }
}

SdlPlatform::~SdlPlatform() {
    // Reverse construction order. Zero any held rumble first so a long-duration hold never outlives the
    // loop — the internal safety the immediate-mode surface needs (a game never issues a shutdown stop).
    for (OpenPad& pad : pads_) {
        SDL_RumbleGamepad(pad.handle, 0, 0, 0);
        SDL_RumbleGamepadTriggers(pad.handle, 0, 0, 0);
        SDL_CloseGamepad(pad.handle);
    }
    if (gpu_ && window_) SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    if (gpu_) SDL_DestroyGPUDevice(gpu_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void SdlPlatform::openGamepad(SDL_JoystickID id) {
    for (const OpenPad& pad : pads_) {
        if (pad.id == id) return;  // already open
    }
    if (SDL_Gamepad* handle = SDL_OpenGamepad(id)) {
        // Detect the family so labelled sources, family-qualified rows, and glyph prompts adapt.
        // Every pad enters at slot 0 (the all-devices-to-player-0 default); assignGamepad re-routes.
        pads_.push_back(OpenPad{handle, id, controllerTypeFrom(SDL_GetGamepadType(handle)), 0});
    }
}

void SdlPlatform::closeGamepad(SDL_JoystickID id) {
    std::erase_if(pads_, [&](OpenPad& pad) {
        if (pad.id != id) return false;
        SDL_CloseGamepad(pad.handle);
        return true;
    });
}

void SdlPlatform::assignGamepad(SDL_JoystickID id, int player) {
    const int slot = std::clamp(player, 0, kMaxPlayers - 1);
    for (OpenPad& pad : pads_) {
        if (pad.id == id) pad.slot = slot;
    }
}

void SdlPlatform::assignKeyboard(int player) {
    keyboardSlot_ = std::clamp(player, 0, kMaxPlayers - 1);
}

std::vector<GamepadInfo> SdlPlatform::connectedGamepads() const {
    std::vector<GamepadInfo> out;
    out.reserve(pads_.size());
    for (const OpenPad& pad : pads_) {
        out.push_back(GamepadInfo{pad.id, pad.family, pad.slot});
    }
    return out;
}

void SdlPlatform::captureRequest(int player) {
    const int slot = std::clamp(player, 0, kMaxPlayers - 1);
    std::vector<detail::CapturePad> onSlot;
    for (const OpenPad& pad : pads_) {
        if (pad.slot == slot) onSlot.push_back(capturePad(pad.handle, pad.id, pad.slot, pad.family));
    }
    capture_->request(slot, onSlot, analogResponse_);
}

std::optional<CapturedSource> SdlPlatform::capturedSource(int player) const noexcept {
    return capture_->answer(std::clamp(player, 0, kMaxPlayers - 1));
}

void SdlPlatform::forwardToCapture(const SDL_Event& event) {
    SDL_JoystickID which = 0;
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            capture_->event(event, keyboardSlot_, nullptr, analogResponse_);
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: which = event.gbutton.which; break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: which = event.gaxis.which;   break;
        default: return;
    }
    for (const OpenPad& pad : pads_) {
        if (pad.id != which) continue;
        const detail::CapturePad readings = capturePad(pad.handle, pad.id, pad.slot, pad.family);
        capture_->event(event, keyboardSlot_, &readings, analogResponse_);
        return;
    }
}

void SdlPlatform::emitVibration(int player, const MotorLevels& levels) noexcept {
    // Effectively "until changed": the flush re-issues on the next diff, and a stop is a declared-silence
    // diff, so a held value must not time out on its own. SDL_RumbleGamepad cancels-and-replaces
    // atomically, so a value change is seamless (no stop-then-start gap).
    constexpr Uint32 kHoldMs = 0xFFFFFFFFu;
    const auto scale = [](std::uint8_t v) noexcept -> Uint16 {
        return static_cast<Uint16>(static_cast<int>(v) * 257);  // 0..255 → 0..65535 exactly
    };
    for (const OpenPad& pad : pads_) {
        if (pad.slot != player) continue;
        const SDL_PropertiesID props = SDL_GetGamepadProperties(pad.handle);
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
            SDL_RumbleGamepad(pad.handle, scale(levels.low), scale(levels.high), kHoldMs);
        }
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)) {
            SDL_RumbleGamepadTriggers(pad.handle, scale(levels.triggerLeft), scale(levels.triggerRight),
                                      kHoldMs);
        }
    }
}

void SdlPlatform::pumpEvents() {
    // Relative quantities are per-PUMP: reset, accumulate from this pump's events, and the run loop
    // sums them across pumps between ticks (so motion is never lost on a zero-tick frame). The
    // device-activity flags are per-pump too — the active-device signal persists on the sample and
    // only moves when a device actually produces input.
    frameRawDX_ = 0.0f;
    frameRawDY_ = 0.0f;
    frameWheel_ = 0.0f;
    kbActivityThisPump_ = false;
    padActivityThisPump_.fill(false);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit_ = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                closeGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                // Device activity for the active-device signal. A button press always counts; axis
                // motion counts past the dead-zone (so stick drift doesn't steal the glyphs).
                if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                    std::abs(static_cast<int>(event.gaxis.value)) > kAxisActivity) {
                    const SDL_JoystickID which = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                                                     ? event.gbutton.which
                                                     : event.gaxis.which;
                    for (const OpenPad& pad : pads_) {
                        if (pad.id == which) {
                            padActivityThisPump_[static_cast<std::size_t>(pad.slot)] = true;
                            padActivityFamily_[static_cast<std::size_t>(pad.slot)]   = pad.family;
                            break;
                        }
                    }
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                kbActivityThisPump_ = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                // Absolute position (window logical points) for the cursor map; relative motion (raw
                // device delta) accumulated for the spinner. In relative-capture mode SDL keeps the
                // position pinned and reports only the deltas — exactly what a spinner integrates.
                mouseWinX_  = event.motion.x;
                mouseWinY_  = event.motion.y;
                frameRawDX_ += event.motion.xrel;
                frameRawDY_ += event.motion.yrel;
                kbActivityThisPump_ = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                // The event's own position updates the cursor too: over an OS-drag (hit-test) region
                // the system suppresses motion events, so a click there would otherwise be judged at
                // wherever the cursor last moved inside app territory — not where it happened.
                mouseWinX_ = event.button.x;
                mouseWinY_ = event.button.y;
                const bool down = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                std::uint8_t bit = 0;
                switch (event.button.button) {
                    case SDL_BUTTON_LEFT:   bit = std::uint8_t{1} << static_cast<int>(MouseButton::Left);   break;
                    case SDL_BUTTON_RIGHT:  bit = std::uint8_t{1} << static_cast<int>(MouseButton::Right);  break;
                    case SDL_BUTTON_MIDDLE: bit = std::uint8_t{1} << static_cast<int>(MouseButton::Middle); break;
                    default: break;
                }
                if (down) {
                    mouseHeld_ |= bit;
                    kbActivityThisPump_ = true;
                } else {
                    mouseHeld_ = static_cast<std::uint8_t>(mouseHeld_ & ~bit);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                frameWheel_ += event.wheel.y;
                kbActivityThisPump_ = true;
                break;
            default:
                break;
        }
        if (capture_->anyArmed()) forwardToCapture(event);
    }

    // Poll the absolute pointer position once per pump: the OS suppresses pointer events over
    // chrome-claimed areas (hit-test drag regions), so the event stream alone leaves the cursor
    // frozen at its last delivered position there. The global query minus the window origin is
    // ground truth every pump, in the same logical points the events carry. Skipped while captured —
    // the OS position is pinned in relative mode and the deltas are the signal.
    if (!pointerCaptured_) {
        float gx = 0.0f;
        float gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
        int wx = 0;
        int wy = 0;
        SDL_GetWindowPosition(window_, &wx, &wy);
        mouseWinX_ = gx - static_cast<float>(wx);
        mouseWinY_ = gy - static_cast<float>(wy);
    }
    buildSample();
}

void SdlPlatform::buildSample() {
    const bool* keys = SDL_GetKeyboardState(nullptr);

    // The suppression rule's per-action family masks, computed once per pump: which families have
    // family-qualified Pad/Stick rows for each action (see input_actions.h).
    std::array<std::uint8_t, kMaxActions> qualifiedMasks{};
    for (const ActionBinding& row : actions_.rows()) {
        if (!row.source.family.has_value()) continue;
        if (row.source.kind != Source::Kind::Pad && row.source.kind != Source::Kind::Stick) continue;
        qualifiedMasks[row.action] |= static_cast<std::uint8_t>(
            1u << static_cast<unsigned>(*row.source.family));
    }

    for (int slot = 0; slot < kMaxPlayers; ++slot) {
        sampleSlot(slot, keys, qualifiedMasks);
    }
}

void SdlPlatform::sampleSlot(int slot, const bool* keys,
                             const std::array<std::uint8_t, kMaxActions>& qualifiedMasks) {
    PlayerSample& out = sample_.players[static_cast<std::size_t>(slot)];
    out.held = ActionSet{};
    out.values.fill(Vec2{0.0f, 0.0f});

    // ── The raw analog/pointer surface for this slot ──
    AnalogInput a;
    if (slot == keyboardSlot_) {
        a.rawDeltaX = frameRawDX_;
        a.rawDeltaY = frameRawDY_;
        a.wheel     = frameWheel_;
        a.mouseHeld = mouseHeld_;

        // Map the pointer into viewport space by inverting the renderer's blit. The blit destination
        // is recomputed from the same pure function and inputs the renderer uses (drawableSize() =
        // swapchain size, viewport_), so the mapped coordinate matches what is actually drawn.
        // drawableSize() is PHYSICAL pixels (HIGH_PIXEL_DENSITY window); the mouse event position is
        // LOGICAL points, so it is scaled by the window's pixel density before inversion.
        const PixelSize draw = drawableSize();
        const IntRect blit = integerScaleToFitRect(draw, viewport_);
        const float density = SDL_GetWindowPixelDensity(window_);
        const Vec2i winPx{static_cast<int>(mouseWinX_ * density),
                          static_cast<int>(mouseWinY_ * density)};
        const ViewportHit hit = windowToViewport(winPx, blit, viewport_);
        a.cursor = hit.pos;
        // There is no meaningful absolute cursor while captured (the OS position is pinned); report
        // off-screen so a consumer doesn't draw a stale reticle during a spinner level.
        a.cursorOnScreen = hit.inside && !pointerCaptured_;
    }
    // Raw sticks/triggers aggregate across the slot's pads by max magnitude per axis; the configured
    // dead-zone + gate then resolve each whole stick (a two-axis operation) once on the aggregate.
    for (const OpenPad& pad : pads_) {
        if (pad.slot != slot) continue;
        a.rawLeftX  = maxMagnitude(a.rawLeftX,  rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX)));
        a.rawLeftY  = maxMagnitude(a.rawLeftY,  rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY)));
        a.rawRightX = maxMagnitude(a.rawRightX, rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTX)));
        a.rawRightY = maxMagnitude(a.rawRightY, rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTY)));
        a.rawTriggerL = std::max(a.rawTriggerL, rawTriggerAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)));
        a.rawTriggerR = std::max(a.rawTriggerR, rawTriggerAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
        // The d-pad as a digital unit vector (up = -y, the stick convention), aggregated like the sticks.
        const float dx = (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1.0f : 0.0f) -
                         (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 1.0f : 0.0f);
        const float dy = (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1.0f : 0.0f) -
                         (SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_DPAD_UP) ? 1.0f : 0.0f);
        a.dpadX = maxMagnitude(a.dpadX, dx);
        a.dpadY = maxMagnitude(a.dpadY, dy);
    }
    const Vec2 leftProcessed  = applyStickResponse(a.rawLeftX, a.rawLeftY, analogResponse_.leftStick);
    const Vec2 rightProcessed = applyStickResponse(a.rawRightX, a.rawRightY, analogResponse_.rightStick);
    a.leftX = leftProcessed.x;   a.leftY = leftProcessed.y;
    a.rightX = rightProcessed.x; a.rightY = rightProcessed.y;
    a.triggerL = applyTriggerResponse(a.rawTriggerL, analogResponse_.leftTrigger);
    a.triggerR = applyTriggerResponse(a.rawTriggerR, analogResponse_.rightTrigger);
    out.analog = a;

    // ── Sample the map: one pass over the rows; any active source sets its action's bit and folds
    //    into the action's value. Nothing is filtered — a row the game wrote is a row that samples. ──
    for (const ActionBinding& row : actions_.rows()) {
        const Source& src = row.source;
        switch (src.kind) {
            case Source::Kind::Key:
                if (slot == keyboardSlot_ && keys && keys[src.key]) {
                    out.held.set(row.action, true);
                    contribute(out.values[row.action], src.component, 1.0f);
                }
                break;
            case Source::Kind::Mouse:
                if (slot == keyboardSlot_ &&
                    (mouseHeld_ & (std::uint8_t{1} << static_cast<int>(src.mouse))) != 0) {
                    out.held.set(row.action, true);
                    contribute(out.values[row.action], src.component, 1.0f);
                }
                break;
            case Source::Kind::Pad:
                for (const OpenPad& pad : pads_) {
                    if (pad.slot != slot) continue;
                    if (!padRowAppliesTo(src, pad.family, qualifiedMasks[row.action])) continue;
                    if (src.pad == PadButton::TriggerL || src.pad == PadButton::TriggerR) {
                        const auto axis = (src.pad == PadButton::TriggerL)
                                              ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                              : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        const auto trig =
                            (src.pad == PadButton::TriggerL) ? analogResponse_.leftTrigger
                                                             : analogResponse_.rightTrigger;
                        const float v = applyTriggerResponse(
                            rawTriggerAxis(SDL_GetGamepadAxis(pad.handle, axis)), trig);
                        if (v > 0.0f) contribute(out.values[row.action], src.component, v);
                        if (v >= sourceThreshold(src)) out.held.set(row.action, true);
                    } else if (padButtonIsAnalog(src.pad)) {
                        // A stick-direction pseudo-button: digital past the threshold on that pad's own
                        // processed axis; contributes as a unit while down.
                        const Vec2 lv = applyStickResponse(
                            rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX)),
                            rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY)),
                            analogResponse_.leftStick);
                        const Vec2 rv = applyStickResponse(
                            rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTX)),
                            rawStickAxis(SDL_GetGamepadAxis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTY)),
                            analogResponse_.rightStick);
                        if (stickDirValue(src.pad, lv.x, lv.y, rv.x, rv.y) >= sourceThreshold(src)) {
                            out.held.set(row.action, true);
                            contribute(out.values[row.action], src.component, 1.0f);
                        }
                    } else if (SDL_GetGamepadButton(pad.handle,
                                                    resolvePadButton(src.pad, pad.family))) {
                        out.held.set(row.action, true);
                        contribute(out.values[row.action], src.component, 1.0f);
                    }
                }
                break;
            case Source::Kind::Stick:
                for (const OpenPad& pad : pads_) {
                    if (pad.slot != slot) continue;
                    if (!padRowAppliesTo(src, pad.family, qualifiedMasks[row.action])) continue;
                    const bool left = (src.stick == PadStick::Left);
                    const Vec2 v = applyStickResponse(
                        rawStickAxis(SDL_GetGamepadAxis(
                            pad.handle, left ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX)),
                        rawStickAxis(SDL_GetGamepadAxis(
                            pad.handle, left ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY)),
                        left ? analogResponse_.leftStick : analogResponse_.rightStick);
                    out.values[row.action].x += v.x;
                    out.values[row.action].y += v.y;
                    const float threshold = sourceThreshold(src);
                    if (v.x * v.x + v.y * v.y >= threshold * threshold) {
                        out.held.set(row.action, true);
                    }
                }
                break;
        }
    }

    // Contributions sum, then clamp to the unit box (a stick plus a held key never overdrives).
    for (Vec2& v : out.values) {
        v.x = std::clamp(v.x, -1.0f, 1.0f);
        v.y = std::clamp(v.y, -1.0f, 1.0f);
    }

    // ── Active device: moves only when a device produced input this pump; persists otherwise.
    //    Pads are evaluated first so simultaneous activity resolves to the keyboard deterministically. ──
    if (padActivityThisPump_[static_cast<std::size_t>(slot)]) {
        out.device = ActiveDevice{DeviceKind::Gamepad,
                                  padActivityFamily_[static_cast<std::size_t>(slot)]};
    }
    if (kbActivityThisPump_ && slot == keyboardSlot_) {
        out.device = ActiveDevice{DeviceKind::KeyboardMouse, ControllerType::Unknown};
    }
}

// ── SdlAudioSink ──────────────────────────────────────────────────────────────────────────────────

SdlAudioSink::~SdlAudioSink() { stop(); }

void SdlAudioSink::audioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount,
                                 int /*totalAmount*/) {
    auto* self = static_cast<SdlAudioSink*>(userdata);
    if (additionalAmount <= 0 || !self->pull_) {
        return;
    }
    // additionalAmount is bytes; a stereo S16 frame is sizeof(AudioFrame) (4) bytes. Pull in fixed
    // chunks, silence-filling any underflow, and feed each chunk to the stream.
    constexpr int kFrameBytes = static_cast<int>(sizeof(AudioFrame));
    int framesNeeded = additionalAmount / kFrameBytes;
    std::array<AudioFrame, 512> buf{};
    while (framesNeeded > 0) {
        const int n = std::min(framesNeeded, static_cast<int>(buf.size()));
        const std::size_t got = self->pull_(std::span<AudioFrame>(buf.data(), static_cast<std::size_t>(n)));
        for (std::size_t i = got; i < static_cast<std::size_t>(n); ++i) {
            buf[i] = AudioFrame{};  // underflow → silence
        }
        SDL_PutAudioStreamData(stream, buf.data(), n * kFrameBytes);
        framesNeeded -= n;
    }
}

void SdlAudioSink::start(unsigned rate, int channels, AudioPullFn pull) {
    stop();  // idempotent restart
    pull_ = std::move(pull);
    // Request a small device buffer so a freshly-played note reaches the speakers promptly. A note's
    // onset latency is dominated by the device buffer (the ring is empty while idle), so SDL's larger
    // default would make presses feel late; ~256 frames ≈ 5 ms at 48 kHz. The ring still absorbs jitter
    // behind it. (CoreAudio's own output latency remains a floor below this.)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;  // native-endian signed 16-bit — matches AudioFrame's int16 L/R
    spec.channels = channels;
    spec.freq = static_cast<int>(rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &audioCallback, this);
    if (!stream_) {
        pull_ = nullptr;
        fail("SDL_OpenAudioDeviceStream failed");
    }
    SDL_ResumeAudioStreamDevice(stream_);  // the device starts paused; begin draining
}

void SdlAudioSink::stop() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);  // guarantees the callback is not running afterwards
        stream_ = nullptr;
    }
    pull_ = nullptr;  // safe to clear: the callback cannot fire past the destroy
}

void SdlPlatform::pointerCaptured(bool captured) {
    // SDL relative-mouse mode: hides + confines the OS cursor and reports unbounded relative motion.
    // On failure the tracked state stays as it was (the window mode is unchanged).
    if (SDL_SetWindowRelativeMouseMode(window_, captured)) {
        pointerCaptured_ = captured;
    }
}

void SdlPlatform::cursorVisible(bool visible) {
    // SDL show/hide of the OS cursor — independent of relative-mouse capture (which hides the cursor as
    // a side effect of confining it). Absolute cursor tracking is unaffected: input() keeps reporting
    // the position. On failure the tracked state is unchanged.
    if (visible ? SDL_ShowCursor() : SDL_HideCursor()) {
        cursorVisible_ = visible;
    }
}

PixelSize SdlPlatform::drawableSize() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return PixelSize{width, height};
}

PixelSize SdlPlatform::windowSize() const {
    // Logical points (SDL window size is logical); user resizes and OS clamps are reflected here.
    int width  = 0;
    int height = 0;
    SDL_GetWindowSize(window_, &width, &height);
    return PixelSize{width, height};
}

void SdlPlatform::windowSize(PixelSize size) {
    // Logical points (SDL window size is logical); the drawable follows at × the display density.
    SDL_SetWindowSize(window_, size.width, size.height);
}

PixelSize SdlPlatform::usableDisplaySize() const {
    SDL_Rect bounds{};
    const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
    if (disp != 0 && SDL_GetDisplayUsableBounds(disp, &bounds)) {
        return PixelSize{bounds.w, bounds.h};
    }
    return drawableSize();  // safe fallback when the display can't be queried
}

void SdlPlatform::fullscreen(bool enabled) {
    // NULL fullscreen-mode = SDL3 borderless desktop fullscreen (a real macOS fullscreen Space;
    // a borderless desktop fill elsewhere). The window's fullscreen display mode is left unset, so
    // SDL keeps the desktop resolution; the renderer's letterbox/integer-scale blit absorbs the
    // new drawable size. On failure the tracked state stays as it was (the window is unchanged).
    if (SDL_SetWindowFullscreen(window_, enabled)) {
        fullscreen_ = enabled;
    }
}

void SdlPlatform::suppressNativeWindowChrome(bool suppress) {
    // SDL_SetWindowBordered adds/removes the native decorations live (bordered = !suppress). Runtime
    // toggle only — the no-flash-at-launch guarantee is the BORDERLESS creation flag in the ctor. On
    // failure the tracked state stays as it was (the window is unchanged), mirroring fullscreen.
    if (SDL_SetWindowBordered(window_, /*bordered=*/!suppress)) {
        chromeSuppressed_ = suppress;
    }
}

// ── Window placement + draggable regions ──────────────────────────────────────────

Vec2i SdlPlatform::windowPosition() const {
    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(window_, &x, &y);  // logical points; top-left corner
    return Vec2i{x, y};
}

void SdlPlatform::windowPosition(Vec2i pos) {
    SDL_SetWindowPosition(window_, pos.x, pos.y);  // logical points, handed to the OS as given
}

SDL_HitTestResult SDLCALL SdlPlatform::hitTest(SDL_Window* /*window*/, const SDL_Point* area, void* data) {
    auto* self = static_cast<SdlPlatform*>(data);
    // The hit-test point arrives in window LOGICAL points, exactly like a mouse-motion event. Map it into
    // viewport space through the same blit inversion the cursor uses: scale by the window's pixel density
    // to physical pixels, then invert the letterbox/integer-scale blit. A press in the letterbox bars maps
    // off-content (hit.inside == false) and never drags; a press in the content asks the registered drag
    // predicate (WindowLoop's shape containment) through the seam's dragHit.
    const PixelSize draw    = self->drawableSize();
    const IntRect   blit    = integerScaleToFitRect(draw, self->viewport_);
    const float     density = SDL_GetWindowPixelDensity(self->window_);
    const Vec2i     winPx{static_cast<int>(static_cast<float>(area->x) * density),
                          static_cast<int>(static_cast<float>(area->y) * density)};
    const ViewportHit hit = windowToViewport(winPx, blit, self->viewport_);
    return (hit.inside && self->dragHit(hit.pos)) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

// ── Frame pacing ────────────────────────────────────────────────────────────────

std::chrono::nanoseconds SdlPlatform::nowMonotonic() const {
    // SDL_GetTicksNS: monotonic nanoseconds since SDL init — the same clock domain SDL_DelayPrecise
    // sleeps in, so the host's deadline arithmetic and its sleep agree.
    return std::chrono::nanoseconds{static_cast<std::int64_t>(SDL_GetTicksNS())};
}

std::chrono::nanoseconds SdlPlatform::displayRefreshPeriod() const {
    // Live query of the display the window is currently on, so dragging the window to a different-refresh
    // monitor re-paces with no event handling. Period = 1 / refresh_rate; fall back to 60 Hz when the
    // rate is unknown (0) or the query fails, reusing the canonical Hz60 period constant.
    if (const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_); disp != 0) {
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(disp);
            mode && mode->refresh_rate > 0.0f) {
            return std::chrono::nanoseconds{
                static_cast<std::int64_t>(1'000'000'000.0 / mode->refresh_rate + 0.5)};
        }
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(TickPeriodNs::Hz60)};
}

void SdlPlatform::sleepPrecise(std::chrono::nanoseconds duration) {
    // Guard > 0 before the Uint64 cast — a non-positive remainder must not underflow into a multi-century
    // sleep. SDL_DelayPrecise high-resolution-sleeps the bulk and busy-waits only the sub-ms tail.
    if (duration > std::chrono::nanoseconds::zero()) {
        SDL_DelayPrecise(static_cast<Uint64>(duration.count()));
    }
}

}  // namespace retropp
