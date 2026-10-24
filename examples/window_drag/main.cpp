// Window drag — a borderless window with a game-drawn title bar, movable through the window surface.
//
// The window opens borderless (EngineConfig::window.suppressNativeWindowChrome), so there is no OS
// chrome; the game draws its own title bar as a Region. Making the window movable is then two lines:
//
//     platform.window().dragHandles({titleBar});                // the drawn bar IS the drag handle
//     platform.window().autoMove({.trigger = Action::Grab});    // any input can drag it too
//
// The game never writes movement code — the tick below only handles quit. The window moves because
// it was declared movable:
//
//   • press the title bar with the mouse → the OS window manager drags the window (native drag);
//   • hold the grab action (right mouse button, or a pad's south button) anywhere → the window follows
//     the pointer, the left stick, and the d-pad (the automatic path — how a gamepad drags a window).
//
// Tab (or a pad's north button) switches how the grab drag is served: the engine's automatic
// movement, or the same drag written in game code — reading the pointer/stick/d-pad each tick and
// moving the window through window().position(). Both feel the same; the toggle shows the two ways
// to drive the window surface.
//
// Esc (or a pad's Start) quits. Dev-run only — CI has no display; building it is the compile signal
// for the live SDL_SetWindowHitTest + SDL_Set/GetWindowPosition path.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

enum class Action : std::uint8_t { Grab, Mode, Quit };

constexpr float kTitleBarHeight = 16.0f;  // the title bar strip, in viewport pixels
constexpr float kManualDragRate = 4.0f;   // stick/d-pad points per tick in the game-driven drag

// A solid-color rectangle region — the demo's one drawing primitive.
Region fill(std::string key, float x, float y, float w, float h, Rgba8 colour) {
    return Region{
        .key     = std::move(key),
        .shape   = ShapePoints::rectangle(Point{x, y}, w, h),
        .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = colour}}};
}

}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "WindowDragDemo"},
        .window   = {.title = "Polyrhythm — window drag", .suppressNativeWindowChrome = true}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};
    RunLoop     loop{clock};

    const float vw = static_cast<float>(config.viewport.width);
    const float vh = static_cast<float>(config.viewport.height);

    ActionMap actions{
        {Action::Grab, {MouseButton::Right, PadButton::FaceSouth}},
        {Action::Mode, {SDL_SCANCODE_TAB, PadButton::FaceNorth}},
        {Action::Quit, {SDL_SCANCODE_ESCAPE, PadButton::Start}},
    };
    platform.actions(actions);

    // The window's chrome, built once. The title bar is an ordinary drawn Region — and the drag handle.
    const Region body     = fill("body", 0.0f, kTitleBarHeight, vw, vh - kTitleBarHeight,
                                 Rgba8{28, 30, 44});
    const Region titleBar = fill("titlebar", 0.0f, 0.0f, vw, kTitleBarHeight, Rgba8{70, 96, 150});
    const Region pip0     = fill("pip0", 6.0f, 5.0f, 6.0f, 6.0f, Rgba8{230, 232, 245});
    const Region pip1     = fill("pip1", 16.0f, 5.0f, 6.0f, 6.0f, Rgba8{230, 232, 245});

    // The whole feature: the drawn bar becomes the OS drag handle, and the grab action lets any
    // input — pointer, stick, d-pad — move the window.
    platform.window().dragHandles({titleBar});
    platform.window().autoMove({.trigger = Action::Grab});

    // The mode toggle: the same grab drag, served two ways. Automatic — the engine's declared
    // movement does everything. Manual — automatic movement is declared off (WindowMovement::None)
    // and the game reads the same sources itself, moving the window through window().position().
    bool  manual = false;
    float remX = 0.0f, remY = 0.0f;  // the manual drag's banked sub-point motion

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Quit)) { loop.exitRequest(); }

        if (in.justPressed(Action::Mode)) {
            manual = !manual;
            remX = remY = 0.0f;
            platform.window().autoMove(
                manual ? WindowMovement::None : WindowMovement{.trigger = Action::Grab});
            std::printf("[demo] drag: %s\n",
                        manual ? "manual (game code moves the window)"
                               : "automatic (the engine moves it)");
        }

        // The game-driven drag: pointer delta 1:1, stick and d-pad at a per-tick rate, the fraction
        // banked so slow drags never stall — the same feel as the automatic path, written by hand.
        if (manual && in.isHeld(Action::Grab)) {
            const Vec2 stick = in.stick(Stick::Left);
            const Vec2 pad   = in.dpad();
            remX += in.rawDeltaX() + (stick.x + pad.x) * kManualDragRate;
            remY += in.rawDeltaY() + (stick.y + pad.y) * kManualDragRate;
            const float wholeX = std::trunc(remX);
            const float wholeY = std::trunc(remY);
            remX -= wholeX;
            remY -= wholeY;
            if (wholeX != 0.0f || wholeY != 0.0f) {
                const Vec2i p = platform.window().position();
                platform.window().position(
                    Vec2i{p.x + static_cast<int>(wholeX), p.y + static_cast<int>(wholeY)});
            }
        } else {
            remX = remY = 0.0f;  // a fresh grab starts clean
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "chrome"};
        layer.z       = 0;
        layer.size    = PixelSize{config.viewport.width, config.viewport.height};
        layer.regions = {body, titleBar, pip0, pip1};
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    WindowedHost{loop, platform}.run();
    return 0;
}
