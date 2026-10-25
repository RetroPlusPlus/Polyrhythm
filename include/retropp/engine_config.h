#pragma once

#include <filesystem>
#include <string>

#include "retropp/app_identity.h"  // AppIdentity — the program's identity to the host platform
#include "retropp/output.h"     // SamplingMode
#include "retropp/timing.h"     // TimingProfile
#include "retropp/viewport.h"   // ViewportResolution

namespace retropp {

// Window creation parameters — the host-OS window the platform opens. The window SIZE is not set
// here: it is derived from the viewport and the presentation scale (EnhancementToggles::windowScale)
// so the window is always an integer multiple of the game's native resolution, clamped to the
// display — see EnhancementToggles below.
struct WindowConfig {
    std::string title = "Polyrhythm";

    // Create the window WITHOUT the native OS chrome (title bar / border / decorations). Consumed
    // once at construction — the platform opens the window borderless from the first frame, so the
    // native decorations never flash at launch. Default false = the standard decorated window. For an
    // app that draws its OWN chrome (a custom draggable title bar), this removes the OS chrome that
    // would otherwise stack underneath it. Toggleable live afterward via
    // Platform::suppressNativeWindowChrome (the runtime path uses SDL_SetWindowBordered).
    bool suppressNativeWindowChrome = false;
};

// Presentation-enhancement settings — every user-opt-in enhancement, defaulted so a default config
// plays exactly like the original (faithful color/sampling, windowed) at a sensible default size.
// `fullscreen` + `sampling` default OFF/identity (faithful); `windowScale` is the one sizing field.
// A world zoom factor, an audio-pack selection, and post-process filter selection are appended
// later (additive — never a reshape).
struct EnhancementToggles {
    int          windowScale = 4;                       // window = viewport × this (LOGICAL points),
                                                        // clamped down to fit the display (fitWindowScale)
    bool         fullscreen  = false;                   // native OS fullscreen toggle
    SamplingMode sampling    = SamplingMode::Nearest;   // blit sampler: Nearest (faithful) / Bilinear
    // world zoom factor, audio-pack id, post-process filter selection: appended when they land
};

// The single startup configuration the host hands the platform: window + internal viewport + render
// timing + forward enhancement toggles. Platform-agnostic VALUE types only (no live device
// handles): the platform layer reads `window`; the renderer reads `viewport`; the run loop reads
// `timing`. Every field defaults to the faithful Game Boy Color baseline, so a default-constructed
// EngineConfig reproduces the original behavior.
//
// Input carries NO config field: the game's action map (input_actions.h) is a value handed to the
// platform directly (SdlPlatform::actions) — an unconfigured platform simply reports no actions.
//
// Dynamic vs startup:
//   * STARTUP-ONLY (consumed once at construction): window title, viewport, timing.
//   * RUNTIME-DYNAMIC: the action map (actions on the platform); enhancements — windowScale
//     seeds the initial window size and is then re-applied live via window().size(),
//     fullscreen via window().fullscreen(), sampling seeded from config by setActive() (so the call
//     site need not apply it) and overridable at runtime via Renderer::samplingMode.
struct EngineConfig {
    // The application's identity (app_identity.h) — REQUIRED, and the FIRST member: identity is a
    // typed, first-class field and leads the aggregate, the same law as ObjectKey on every drawable.
    // setActive() throws std::invalid_argument when either field is empty: every program declares
    // who it is before it starts, exactly as every major platform demands of a project.
    // SDL_GetPrefPath resolves the per-user save/settings directory from it (%APPDATA%\<org>\<app>
    // on Windows, ~/Library/Application Support/<org>/<app> on macOS, $XDG_DATA_HOME/<org>/<app>
    // on Linux), and setActive() fans it out to SaveStore::defaultIdentity like the other per-type
    // defaults (see app_identity.h for why no fallback exists).
    AppIdentity        identity{};

    WindowConfig       window{};
    ViewportResolution viewport     = ViewportResolution::GameBoyColor;  // 160×144
    TimingProfile      timing       = TimingProfile::GameBoyColor;
    EnhancementToggles enhancements{};

    // Automatic render interpolation. The renderer eases each layer/sprite between its previous and current
    // simulation-tick state by the run loop's sub-tick factor, so motion stays smooth when the display
    // refresh outpaces the tick rate. Default true (the faithful smooth baseline); seeds
    // Renderer::defaultInterpolation via setActive(), and Renderer::automaticInterpolation overrides it at runtime.
    // False → the renderer composites each submission verbatim.
    bool               interpolation = true;

    // The evaluation grid the analytic render paths use (transformed tiles, effect regions, the sampling
    // effects). Default Viewport (crisp): the geometry evaluates on the viewport grid, so the upscaled image
    // is pixel-identical to the viewport-resolution rasterization while placement stays sub-pixel for steady
    // motion. Output evaluates per output pixel (smooth edges/displacement under upscale). Seeds
    // Renderer::defaultEvaluationGrid via setActive(); Renderer::evaluationGrid overrides it at runtime.
    // (EvaluationGrid lives in output.h beside SamplingMode.)
    EvaluationGrid     evaluationGrid = EvaluationGrid::Viewport;

    // `assetRoot` is the runtime base directory LoadFromPath assets resolve against (via assetPath());
    // setActive resolves it to an ABSOLUTE path once — against the executable directory — and fans it
    // out. Default empty = the executable directory itself, which is where the build copies LoadFromPath
    // assets (at their logical path), so the build and the runtime agree with no configuration. A
    // LoadFromPath routine resolves against this same root (there is no separate routine root).
    //
    // There is NO global embed-policy default: per-asset / per-routine Embed-vs-LoadFromPath is the
    // explicit per-call AssetPolicy argument at the load site, falling through to the loader's per-type
    // default (loadAtlas → LoadFromPath; loadMapPng / loadPaletteImage / chiptune routine → Embed). See
    // assets-and-embedding.md.
    std::filesystem::path      assetRoot{};

    // The set-once active config: the host assigns it once via setActive() (below), and bare
    // ctors then inherit from it instead of every field being threaded to every ctor — RunLoop and
    // Renderer read the per-type static defaults setActive() fans the config out into, and SdlPlatform
    // reads `active` directly (it takes the whole config — window). Per-ctor override
    // stays available. A default-constructed `active` is the faithful Game Boy Color baseline. Self-
    // typed static: declared here (the type is incomplete in-class), defined `inline` just below the
    // struct.
    static EngineConfig active;

    // Requires config.identity (both fields set — throws std::invalid_argument otherwise), then
    // assigns `active = config` AND fans the config out into the per-type SDL-free static defaults so
    // bare ctors inherit them: config.timing → RunLoop / AnimationPlayer / every interpolable
    // TweenPlayer<T> / PathWalker / SpritePath / VibrationPlayer ::defaultTiming; the renderer fields →
    // Renderer::defaultViewport / defaultSamplingMode / defaultInterpolation / defaultEvaluationGrid;
    // config.identity → SaveStore::defaultIdentity; config.assetRoot → the asset root (resolved absolute
    // once). One call at startup. Defined in src/engine_config.cpp — keeping the definition out of
    // this header avoids pulling renderer.h (SDL_gpu) / run_loop.h / animation.h in, so this header stays
    // light and SDL/GPU-free, and it quarantines the one point where the SDL-coupled config layer and the
    // locked SDL-free core loop meet (data flows downward only).
    static void setActive(const EngineConfig& config);
};

// Out-of-class definition: a complete type is required for a static data member definition, so this
// cannot live inside the struct body (where EngineConfig is still incomplete). `inline` makes it a
// single definition across translation units (C++17). Same idiom as TimingProfile::GameBoyColor.
inline EngineConfig EngineConfig::active{};

}  // namespace retropp
