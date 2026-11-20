#include "retropp/engine_config.h"

#include <stdexcept>

#include <SDL3/SDL_filesystem.h>  // SDL_GetBasePath — the one place the executable dir is consulted

#include "retropp/animation.h"        // AnimationPlayer::defaultTiming
#include "retropp/asset_registry.h"   // setAssetRoot
#include "retropp/path_walker.h"      // PathWalker::defaultTiming
#include "retropp/renderer.h"         // Renderer::defaultViewport
#include "retropp/save_store.h"       // SaveStore::defaultIdentity
#include "retropp/sprite_path.h"      // SpritePath::defaultTiming
#include "retropp/run_loop.h"         // RunLoop::defaultTiming
#include "retropp/tween.h"            // TweenPlayer<T>::defaultTiming (the interpolable T's)
#include "retropp/vibration.h"        // VibrationPlayer::defaultTiming
#include "retropp/worker_pool.h"      // WorkerPool::defaultWorkers

namespace retropp {

namespace {
// Resolve the configured asset root to an ABSOLUTE path. An absolute path is used as-is; a relative
// one (empty by default → the executable directory itself) is joined onto the executable directory
// (SDL_GetBasePath — cached by SDL3, not freed). This is the single place the executable/base dir is
// consulted: the loaders then resolve LoadFromPath assets against the fanned-out absolute root via
// assetPath().
std::filesystem::path resolveAssetRoot(const std::filesystem::path& configured) {
    if (configured.is_absolute()) return configured;
    const char* base = SDL_GetBasePath();
    if (base == nullptr) return configured;  // base dir unavailable → leave it relative to the cwd
    return std::filesystem::path(base) / configured;
}
}  // namespace

// The single point where the SDL/GPU-coupled config layer and the SDL-free core loop meet — in a .cpp,
// in the config layer, with the data flowing DOWNWARD only: this reaches into the core's own static
// defaults to store plain SDL-free values (TimingProfile, ViewportResolution), so the core headers
// never reach UP into engine_config.h. The values
// crossing the boundary are the same ones `RunLoop loop{clock, config.timing}` / `Renderer{...,
// config.viewport}` take by argument — stored as defaults instead of threaded per call.
//
// This also seeds the playback-cadence defaults — AnimationPlayer::defaultTiming, every interpolable
// TweenPlayer<T>::defaultTiming, PathWalker::defaultTiming, SpritePath::defaultTiming, and
// VibrationPlayer::defaultTiming — the blit sampling default, and WorkerPool::defaultWorkers, so one
// startup call covers timing, viewport, sampling, animation cadence, tween cadence, the path-movement
// cadences, the vibration cadence, and the worker count (a game need not set any of them separately).
void EngineConfig::setActive(const EngineConfig& config) {
    // The identity is required. No major platform lets a project exist without one — an IDE
    // demands it before the first hello-world builds — and an engine that starts anonymously
    // would resolve a wrong (colliding) save location the moment anything persists. Refused
    // here, at the one startup call, so the failure is immediate and names its fix.
    if (config.identity.organization.empty() || config.identity.application.empty()) {
        throw std::invalid_argument(
            "EngineConfig::setActive: config.identity is required — set config.identity = "
            "{.organization = \"YourOrg\", .application = \"YourGame\"} before calling "
            "setActive");
    }
    active                         = config;
    RunLoop::defaultTiming         = config.timing;
    Renderer::defaultViewport      = config.viewport;
    Renderer::defaultSamplingMode  = config.enhancements.sampling;
    Renderer::defaultInterpolation = config.interpolation;
    Renderer::defaultEvaluationGrid = config.evaluationGrid;
    AnimationPlayer::defaultTiming = config.timing;
    // TweenPlayer<T>::defaultTiming is a per-T template static, so the fan-out seeds each of the engine's
    // interpolable tween types (the T's that lerp() supports). A game using only some of them still gets
    // every one seeded — harmless, and it means a bare TweenPlayer<Vec3> inherits the cadence too.
    TweenPlayer<float>::defaultTiming = config.timing;
    TweenPlayer<Vec2>::defaultTiming  = config.timing;
    TweenPlayer<Vec3>::defaultTiming  = config.timing;
    TweenPlayer<Vec4>::defaultTiming  = config.timing;
    PathWalker::defaultTiming         = config.timing;
    SpritePath::defaultTiming         = config.timing;
    VibrationPlayer::defaultTiming    = config.timing;
    // The application identity: a bare SaveStore resolves its per-user save directory from
    // this (and refuses an empty one — see app_identity.h for why no fallback exists).
    SaveStore::defaultIdentity        = config.identity;
    // The worker count a WorkerPool made without a count of its own starts; unset leaves it to the
    // machine.
    WorkerPool::defaultWorkers = config.workers;
    // Asset root: resolve to an absolute path ONCE (here, the SDL-coupled meeting point) so LoadFromPath
    // assets resolve the same way everywhere via assetPath(). There is no separate routine root —
    // LoadFromPath routines resolve against this same assetRoot(). Embed-vs-load carries no global
    // default: it is the per-call AssetPolicy argument > the loader's per-type default.
    setAssetRoot(resolveAssetRoot(config.assetRoot));
}

}  // namespace retropp
