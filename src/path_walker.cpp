#include "retropp/path_walker.h"

#include <algorithm>  // std::clamp, std::min
#include <cmath>      // std::fmod

namespace retropp {

// ── PathPacing named constructors ─────────────────────────────────────────────────────────────────────

PathPacing PathPacing::speed(float pxPerSecond) {
    return PathPacing{.kind = Kind::Speed, .pxPerSecond = pxPerSecond};
}

PathPacing PathPacing::eased(std::chrono::nanoseconds duration, Easing e) {
    return PathPacing{.kind = Kind::Eased, .duration = duration, .easing = e};
}

PathPacing PathPacing::distanceTween(const Tween<float>& distance) {
    return PathPacing{.kind = Kind::DistanceTween, .distance = &distance};
}

// ── sampleWalk ────────────────────────────────────────────────────────────────────────────────────────────

namespace {

// The arc-length for the Speed driver under `mode`, given the raw distance traveled at constant speed and
// the per-tick step (so PlayForDuration can re-derive the distance at the cutoff tick). Sets `finished`.
float speedDistance(float raw, float pxPerTick, float length, std::uint64_t elapsedTicks,
                    const TimingProfile& profile, PlaybackMode mode, bool& finished) noexcept {
    switch (mode.kind) {
        case PlaybackMode::Kind::LoopIndefinitely:
            finished = false;
            return std::fmod(raw, length);  // sawtooth wrap; continuous on a closed curve

        case PlaybackMode::Kind::Single:
            finished = raw >= length;
            return std::min(raw, length);   // hold the endpoint once past it

        case PlaybackMode::Kind::LoopNTimes: {
            if (mode.loopCount == 0) {       // never played → rests at the start
                finished = true;
                return 0.0f;
            }
            const float endRaw = length * static_cast<float>(mode.loopCount);
            if (raw >= endRaw) {
                finished = true;
                return length;               // hold the endpoint after n passes
            }
            finished = false;
            return std::fmod(raw, length);
        }

        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsedTicks >= cut) {
                finished = true;
                // Hold the distance shown at the cutoff — the last tick that played was cut - 1.
                const float rawAt = cut == 0 ? 0.0f : pxPerTick * static_cast<float>(cut - 1);
                return std::fmod(rawAt, length);
            }
            finished = false;
            return std::fmod(raw, length);
        }
    }
    finished = true;  // unreachable — every kind handled
    return 0.0f;
}

// The arc-length for the Eased driver under `mode`. One pass is durTicks (> 0 — the caller handles the
// all-instantaneous case); the mode wraps/holds TICKS exactly as sampleTween does, then
// s = length × ease(easing, posInPass / durTicks). Sets `finished`.
float easedDistance(float length, std::uint64_t elapsedTicks, std::uint64_t durTicks, Easing easing,
                    const TimingProfile& profile, PlaybackMode mode, bool& finished) noexcept {
    const auto at = [&](std::uint64_t posInPass) {
        const float t = static_cast<float>(posInPass) / static_cast<float>(durTicks);
        return length * ease(easing, t);
    };
    switch (mode.kind) {
        case PlaybackMode::Kind::LoopIndefinitely:
            finished = false;
            return at(elapsedTicks % durTicks);

        case PlaybackMode::Kind::Single:
            if (elapsedTicks >= durTicks) {
                finished = true;
                return length;               // the eased endpoint is pinned exactly
            }
            finished = false;
            return at(elapsedTicks);

        case PlaybackMode::Kind::LoopNTimes: {
            if (mode.loopCount == 0) {        // never played → rests at the start
                finished = true;
                return 0.0f;
            }
            const std::uint64_t endTick = durTicks * mode.loopCount;
            if (elapsedTicks >= endTick) {
                finished = true;
                return length;
            }
            finished = false;
            return at(elapsedTicks % durTicks);
        }

        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsedTicks >= cut) {
                finished = true;
                return at(cut == 0 ? 0 : (cut - 1) % durTicks);
            }
            finished = false;
            return at(elapsedTicks % durTicks);
        }
    }
    finished = true;  // unreachable — every kind handled
    return 0.0f;
}

}  // namespace

WalkSample sampleWalk(const ArcLengthTable& table, const PathPacing& pacing, std::uint64_t elapsedTicks,
                  const TimingProfile& profile, PlaybackMode mode) noexcept {
    const float length = table.length();

    // Degenerate geometry: no travel is possible. Position at the start (Vec2{} on a truly empty table),
    // no direction, distance 0. Finished for the finite modes, not for an indefinite loop.
    if (length <= 0.0f) {
        const bool finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
        return WalkSample{.position = table.atDistance(0.0f), .facing = Vec2{}, .distance = 0.0f,
                          .finished = finished};
    }

    float s        = 0.0f;
    bool  finished = false;

    switch (pacing.kind) {
        case PathPacing::Kind::Speed: {
            const float secondsPerTick = static_cast<float>(profile.tickPeriod().count()) * 1e-9f;
            const float pxPerTick      = pacing.pxPerSecond * secondsPerTick;
            const float raw            = pxPerTick * static_cast<float>(elapsedTicks);
            s = speedDistance(raw, pxPerTick, length, elapsedTicks, profile, mode, finished);
            break;
        }
        case PathPacing::Kind::Eased: {
            const std::uint64_t durTicks = profile.ticksForDuration(pacing.duration);
            if (durTicks == 0) {
                // All-instantaneous: the finite modes are immediately finished at the endpoint; an
                // indefinite loop rests there (never a stall) — the mirror of the all-instantaneous tween.
                finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
                s        = length;
                break;
            }
            s = easedDistance(length, elapsedTicks, durTicks, pacing.easing, profile, mode, finished);
            break;
        }
        case PathPacing::Kind::DistanceTween: {
            if (pacing.distance == nullptr) {
                // The parked default: distance 0, finished per the empty-tween rule.
                finished = mode.kind != PlaybackMode::Kind::LoopIndefinitely;
                s        = 0.0f;
                break;
            }
            const TweenSample<float> t = sampleTween(*pacing.distance, elapsedTicks, profile, mode);
            s        = std::clamp(t.value, 0.0f, length);  // a non-monotone track may reverse; clamp it
            finished = t.finished;
            break;
        }
    }

    return WalkSample{.position = table.atDistance(s), .facing = table.tangentAtDistance(s),
                      .distance = s, .finished = finished};
}

// ── PathWalker ────────────────────────────────────────────────────────────────────────────────────────

namespace {

// Resolve at `elapsed` under `mode`, substituting the walker's last real heading when the fresh sample has
// none (hold-last facing). `sample.facing` is only ever written through this path, so it self-perpetuates
// the last non-zero heading with no extra state.
WalkSample resolveHeld(const PathWalker& w, std::uint64_t elapsed, PlaybackMode mode) noexcept {
    WalkSample fresh = sampleWalk(w.table, w.pacing, elapsed, w.profile, mode);
    if (fresh.facing == Vec2{}) {
        fresh.facing = w.sample.facing;
    }
    return fresh;
}

}  // namespace

void PathWalker::advance(PlaybackMode mode, std::uint64_t deltaTicks) noexcept {
    if (playing) {
        elapsedTicks += deltaTicks;
    }
    sample = resolveHeld(*this, elapsedTicks, mode);
}

void PathWalker::play() noexcept { playing = true; }

void PathWalker::pause() noexcept { playing = false; }

void PathWalker::stop() noexcept {
    playing      = false;
    elapsedTicks = 0;
    sample       = resolveHeld(*this, 0, PlaybackMode::loopIndefinitely());
}

void PathWalker::restart() noexcept {
    playing      = true;
    elapsedTicks = 0;
    sample       = resolveHeld(*this, 0, PlaybackMode::loopIndefinitely());
}

void PathWalker::seek(std::chrono::nanoseconds at) noexcept {
    elapsedTicks = profile.ticksForDuration(at);
    sample       = resolveHeld(*this, elapsedTicks, PlaybackMode::loopIndefinitely());
}

}  // namespace retropp
