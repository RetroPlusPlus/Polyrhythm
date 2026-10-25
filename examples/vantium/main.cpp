// ============================================================================================
//  VANTIUM — a Uridium-style dreadnought strafer on Polyrhythm, and a broad renderer showcase.
//
//  A fast, BIDIRECTIONAL scrolling shooter flown over the deck of a 2560px capital ship: inertia
//  flight with a facing-lookahead camera, raised superstructure that kills a level Manta and
//  yields to a ROLLED one (hold B — guns cold, hitbox thin), fighter squadrons swooping on baked
//  curves in rotating liveries, homing mines, shootable fuel pods — and when the wave quota
//  falls, a LAND NOW touchdown on the strip that scuttles the dreadnought and flies you on to
//  the next, faster one. An ORIGINAL homage to the era (original art, layouts, names, SFX; no
//  Graftgold/Hewson content).
//
//  RENDERER FEATURES ON SHOW (the point of this example, beyond the game):
//    • parallax           — the starfield layer scrolls at half the deck's rate.
//    • finite worlds      — the deck is a Blank-wrapped tilemap whose index-0 holes reveal stars.
//    • tile orientation   — ONE hazard/corner/pipe art serves every edge via flips + Rot90.
//    • frame animation    — the Manta's bank/turn frames; pooled 4-frame explosion clips
//                           (AnimationPlayer, single()).
//    • palette animation  — the fuel-pod glow breathes and the starfield twinkles (the art never
//                           changes); fighter waves recolor by livery palette.
//    • per-sprite Transform — the mines spin; popups shrink through a scale transform.
//    • regions + blends   — an Add thrust glow, a Multiply destruct dim, and a Ripple confined
//                           to the deck band while a scuttled ship groans.
//    • Curve + ArcLengthTable — squadron conga paths walked at constant speed.
//    • EvaluationGrid     — START toggles Viewport (crisp) ↔ Output (smooth) evaluation LIVE, so
//                           the two philosophies can be compared mid-flight.
//
//  Multi-file: layout.h (constants + legend) · deck (section templates → hull) · assets ·
//  game (sim + events) · waves (squadrons/mines/shots) · feel (tweens + anim cursors) · audio
//  (7 embedded SFX) · render · this entry point. ALL assets Embed — the binary is self-contained.
//
// The demo never auto-launches a window; you run it yourself. QUIT: close the window.
//
//  CI: instantiates SdlPlatform + Renderer + AudioSystem for real, so the live GPU + image +
//  audio + curve + animation path keeps compiling/linking on every platform (CI opens no window).
// ============================================================================================

#include <cstdio>
#include <exception>

#include "retropp/clock.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"  // ActionMap, PadButton, presets — the binding surface
#include "retropp/output.h"       // EvaluationGrid — the START toggle
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/timing.h"
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

#include "assets.h"
#include "audio.h"
#include "feel.h"
#include "game.h"
#include "render.h"

int main() {
    using namespace retropp;

    // A raw 640×480 viewport at 60 Hz; engine interpolation stays ON (stable keys throughout,
    // deliberate teleports re-keyed to mount-snap).
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Vantium"},
        .window   = {.title = "Polyrhythm — Vantium (640×480, 60 Hz)"},
        .viewport = ViewportResolution{640, 480},
        .timing   = TimingProfile{TickPeriodNs::Hz60}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The flight vocabulary: the directional preset covers arrows + WASD + d-pad for the four
    // thrust actions; X or the pad's south face fires, Z or the east face rolls, Return/Start
    // starts (and flips the evaluation grid in play), Backspace/Select toggles fullscreen.
    ActionMap actions{
        {vant::Action::Fire,       {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {vant::Action::Roll,       {SDL_SCANCODE_Z, PadButton::FaceEast}},
        {vant::Action::Start,      {SDL_SCANCODE_RETURN, PadButton::Start}},
        {vant::Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    actions.add(presets::directional(vant::Action::Up, vant::Action::Down, vant::Action::Left,
                                     vant::Action::Right));
    platform.actions(actions);

    vant::VantAssets assets;
    try {
        assets = vant::loadVantAssets(renderer);
    } catch (const std::exception& e) {
        std::printf("vantium: could not load assets: %s\n", e.what());
        return 1;
    }

    vant::VantGame     game;
    vant::VantRenderer vantRenderer;
    vant::VantAudio    audio;         // after the platform: the auto-owned sinks need SDL audio
    vant::VantFeel     feel{assets};

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(vant::Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());
        // Start (outside the title screen, where it starts the game): flip the evaluation grid —
        // Viewport = crisp squares (the faithful default), Output = smooth per-output-pixel
        // evaluation. A live A/B of the two rendering philosophies.
        if (in.justPressed(vant::Action::Start) && game.state == vant::GameState::Playing) {
            const bool toOutput = renderer.evaluationGrid() == EvaluationGrid::Viewport;
            renderer.evaluationGrid(toOutput ? EvaluationGrid::Output : EvaluationGrid::Viewport);
            std::printf("[dev] evaluation grid: %s\n", toOutput ? "Output (smooth)" : "Viewport (crisp)");
        }
        game.tick(in, assets);
        for (const vant::GameEvent& e : game.events()) {
            audio.onEvent(e.kind);
            feel.onEvent(e);
        }
        feel.tick();
    });
    loop.renderLoop([&] { vantRenderer.render(renderer, game, assets, feel); });

    std::printf("Vantium (640×480, 60 Hz) — ENTER to fly; arrows steer (inertia), A fires, hold B "
                "to roll through the one-cell gaps; clear the waves, then slow down over the strip "
                "and press Down to land. START toggles crisp/smooth evaluation, SELECT fullscreen. "
                "Close the window to quit.\n");
    WindowedHost{loop, platform}.run();
    return 0;
}
