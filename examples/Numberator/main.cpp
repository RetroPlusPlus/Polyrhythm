// Numberator — a working calculator, styled after the classic Mac OS look, built on the engine to show
// it rendering a non-game UI from the same primitives a game uses. The chrome (window, title bar, sunken
// display well) is a TILEMAP assembled from a map PNG + a catalog; the keys are SPRITES that flip X+Y to
// look pressed; the digits and labels are transparent glyph sprites. Every asset is a PNG baked into the
// binary — no pixel bytes in the source.
//
// Click the keys with the mouse: digits and . build a number, the right column operates, = evaluates,
// C clears, ± negates, % takes a percent, ⌫ deletes a digit. A held key shows sunken until released.
// The window is borderless and the painted title bar is a declared drag handle, so dragging it
// moves the real window — the classic Mac chrome behaving like the real thing.
// This is one of the runnable example hosts; CI compiles and links it on every platform without opening
// the window.

#include <cstdio>
#include <exception>

#include "retropp/analog_input.h"   // MouseButton
#include "retropp/clock.h"          // SteadyClock
#include "retropp/draw_state.h"     // FrameDrawState
#include "retropp/engine_config.h"  // EngineConfig
#include "retropp/input.h"          // InputState
#include "retropp/renderer.h"       // Renderer
#include "retropp/run_loop.h"       // RunLoop
#include "retropp/sdl_platform.h"   // SdlPlatform
#include "retropp/sprite_mask.h"   // SpriteMask — the close button's click test queries the sprite
#include "retropp/viewport.h"       // ViewportResolution
#include "retropp/windowed_host.h"  // WindowedHost

#include "assets.h"
#include "calculator.h"
#include "layout.h"
#include "render.h"

int main() {
    using namespace retropp;
    using namespace numberator;

    // A custom viewport, sized to the calculator window, presented at a 2x integer zoom.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Numberator"},
        .window       = {.title = "Numberator", .suppressNativeWindowChrome = true },
        .viewport     = ViewportResolution{kViewW, kViewH},
        .enhancements = {.windowScale = 2}};
    EngineConfig::setActive(config);  // the bare Renderer below inherits this viewport

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    Assets assets;
    try {
        assets = loadAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("Numberator: could not load an asset: %s\n", e.what());
        return 1;
    }

    // The close box is a SPRITE — its own Platinum-proportioned art, centered in the title bar. The
    // sprite IS the close button: the click test queries its shape, and the drag handles below are
    // laid out around its queried bounds, so its geometry lives on this one value.
    Sprite closeBox{.key = "closebox", .x = 8, .y = (kTitleH - assets.closeBoxSlot.dimensions.height) / 2,
                    .size = assets.closeBoxSlot.dimensions,
                    .atlas = assets.closeBox, .tile = assets.closeBoxSlot.tile,
                    .palette = assets.palette};

    // The window has no OS chrome, so the calculator's own title bar is the drag handle: the strip the
    // chrome map paints, minus the close button's footprint (a press there must reach the app as a
    // CLICK, not start an OS drag). The OS window manager drags the window — the classic Mac title bar,
    // behaving like one.
    const IntRect box = closeBox.mask(Space::Layer).bounds();
    platform.window().dragHandles({
        Region{.key   = "titlebar-left",
               .shape = ShapePoints::rectangle(Point{0.0f, 0.0f}, static_cast<float>(box.x),
                                               static_cast<float>(kTitleH))},
        Region{.key   = "titlebar-right",
               .shape = ShapePoints::rectangle(Point{static_cast<float>(box.x + box.width), 0.0f},
                                               static_cast<float>(kViewW - box.x - box.width),
                                               static_cast<float>(kTitleH))},
    });

    Calculator calc;
    int        pressedKey  = -1;     // the key currently under a held mouse button (for the sunken look)
    bool       closeArmed  = false;  // the press began on the close button; it fires on release over it
    bool       pressedLook = false;  // the close box draws pressed (mirrored + flipped) this frame

    loop.simTick([&](const InputState& in) {
        const Vec2i c = in.cursor();
        const bool  onBox = closeBox.mask(Space::Layer).contains(
            Point{static_cast<float>(c.x) + 0.5f, static_cast<float>(c.y) + 0.5f});

        if (in.mouseJustPressed(MouseButton::Left)) {
            if (onBox) {
                closeArmed = true;  // arm; the button fires on RELEASE, like a real close box
            } else if (const int k = keyAt(c); k >= 0) {
                const Key& key = kKeys[static_cast<std::size_t>(k)];
                calc.press(key.action, key.data);
            }
        }
        if (in.mouseJustReleased(MouseButton::Left)) {
            if (closeArmed && onBox) loop.exitRequest();  // released on the button → close
            closeArmed = false;                           // released elsewhere → canceled
        }

        // The armed button shows pressed — mirrored and flipped, like the keys — while the press is
        // held over it (roll off and it pops back). The flips go on the DRAWN copy only: closeBox
        // stays unflipped as the hit-test geometry, so the click target never moves with the look
        // (the glyph sits off-center in its cell, so a flipped silhouette shifts by a pixel).
        pressedLook = closeArmed && onBox && in.mouseHeld(MouseButton::Left);
        pressedKey     = in.mouseHeld(MouseButton::Left) ? keyAt(c) : -1;
    });

    View           view;
    FrameDrawState frame;
    loop.renderLoop([&]() {
        Sprite drawn = closeBox;   // the drawn copy carries the pressed look; the hit-test never flips
        drawn.flipX  = pressedLook;
        drawn.flipY  = pressedLook;
        view.build(frame, assets, drawn, calc.display(), pressedKey);
        renderer.renderFrame(frame);
    });

    std::printf("Numberator — click the keys to calculate. Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
