#pragma once

#include <chrono>
#include <cstdint>

#include "retropp/animation.h"  // PlaybackMode — reused verbatim for walker playback policy
#include "retropp/curve.h"      // ArcLengthTable — the baked geometry a walker queries
#include "retropp/geometry.h"   // Vec2 — position + facing
#include "retropp/timing.h"     // TimingProfile — px/s → px/tick, durations → ticks
#include "retropp/tween.h"      // Easing, Tween<float>, sampleTween — the distance-driver forms

namespace retropp {

// ── Path walking: PathPacing + PathWalker ─────────────────────────────────────────────────────────────
//
// The movement-player layer over a curve, completing the data→player family: where AnimationPlayer
// resolves elapsed ticks → a frame and TweenPlayer resolves elapsed ticks → a value, PathWalker resolves
// elapsed ticks → a POSITION and a FACING along a curve. The geometry (Curve / ArcLengthTable) answers
// "where is arc-length s, and which way does travel point there"; it carries no clock. Time enters the
// geometry HERE, and only here — as a pacing driver that converts elapsed ticks into a distance along the
// path. The platform provides the pure stateless resolver (sampleWalk, the analog of sampleAnimation / sampleTween);
// the game owns the cursor (PathWalker) and writes the resolved position + facing into whatever it likes.
//
// A walker is content-agnostic: it returns raw Vec2 geometry and never touches a Sprite. A camera on a
// track, a projectile arc, a particle, or a UI rail all use it bare.

// ── PathPacing: the time → distance driver ────────────────────────────────────────────────────────────

// How a walker converts elapsed time into a distance along its curve. Three forms, authored via the
// platform's named-constructor idiom (the PlaybackMode / TimingProfile preset shape); identity is the kind,
// the first member:
//   Speed         — constant speed, in viewport pixels per SECOND (wall time; resolved to px/tick via the
//                   walker's TimingProfile, so the same value means the same on-screen speed on any
//                   cadence).
//   Eased         — one full traversal takes `duration`, shaped by `easing` (the Tween / Easing curve
//                   set): distance = length × ease(easing, t / duration).
//   DistanceTween — a game-owned Tween<float> IS the distance-vs-time function, so a multi-segment
//                   profile can accelerate, pause, and even move BACKWARD along the path (a non-monotone
//                   distance track, clamped to [0, length]).
struct PathPacing {
    enum class Kind : std::uint8_t { Speed, Eased, DistanceTween };
    Kind                     kind = Kind::Speed;             // identity, first member
    float                    pxPerSecond = 0.0f;             // Speed (0 = parked at the start)
    std::chrono::nanoseconds duration{};                     // Eased: wall-time of one traversal
    Easing                   easing = Easing::InOutQuad;     // Eased: the curve over the traversal
    const Tween<float>*      distance = nullptr;             // DistanceTween: game-owned; must outlive the
                                                             //   walker (span-style lifetime)
    [[nodiscard]] bool operator==(const PathPacing&) const noexcept = default;

    static PathPacing speed(float pxPerSecond);
    static PathPacing eased(std::chrono::nanoseconds duration, Easing e = Easing::InOutQuad);
    static PathPacing distanceTween(const Tween<float>& distance);
};

// ── The pure resolver ────────────────────────────────────────────────────────────────────────────────

// The output of the resolver: where the mover is, which way travel points there, the arc-length that was
// resolved, and whether playback has ended (per mode).
struct WalkSample {
    Vec2  position{};          // the point at the resolved arc-length
    Vec2  facing{};            // UNIT direction of travel there; ZERO where the curve has no direction
                               //   (degenerate segment / empty curve) — the stateless resolver cannot
                               //   hold a last heading; the cursor does (see PathWalker).
    float distance = 0.0f;     // the resolved arc-length (∈ [0, table.length()])
    bool  finished = false;    // playback ended (Single / LoopNTimes / PlayForDuration past their end)
    [[nodiscard]] bool operator==(const WalkSample&) const noexcept = default;
};

// THE pure resolver — the single point of movement truth (PathWalker is the stateful wrapper over it), the
// sampleWalk analog of sampleAnimation / sampleTween. Takes the baked table by const ref (pure functions own nothing);
// position = table.atDistance(s) and facing = table.tangentAtDistance(s) — the pair that always agrees.
//
// Per pacing kind, under the reused PlaybackMode (loop = wrap the DISTANCE — continuous on a closed curve,
// a sawtooth snap-back on an open one, the same wrap sampleTween has; a there-and-back is authored as a
// DistanceTween yoyo, never a mode):
//   Speed         — raw = pxPerTick × elapsedTicks; LoopIndefinitely wraps with fmod (never finished),
//                   Single holds `length` once raw ≥ length, LoopNTimes(n) holds `length` after n passes,
//                   PlayForDuration(d) holds the distance shown at the cutoff. Speed 0 stays parked at 0.
//   Eased         — durTicks = one pass; the mode wraps/holds TICKS exactly as sampleTween does, then
//                   s = length × ease(easing, posInPass / durTicks). The eased endpoints are pinned.
//   DistanceTween — pass-through: s = clamp(sampleTween(*distance, …).value, 0, length); the tween's own
//                   wrap / hold / zero-segment semantics ARE the contract. A null pointer is the parked
//                   default (s = 0; finished for the finite modes, not for a loop).
// LoopNTimes(0) rests at the START (s = 0), finished — a walker that never played sits where it started.
// Degenerate geometry (empty table or length 0): position at the start, zero facing, distance 0, finished
// for the finite modes and not for LoopIndefinitely (the mirror of sampleTween's total-0 case).
[[nodiscard]] WalkSample sampleWalk(const ArcLengthTable& table, const PathPacing& pacing,
                                std::uint64_t elapsedTicks, const TimingProfile& profile,
                                PlaybackMode mode) noexcept;

// ── The game-owned cursor ─────────────────────────────────────────────────────────────────────────────

// A game-owned playback cursor over a baked path. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the
// platform. The exact mirror of TweenPlayer / AnimationPlayer: it wraps elapsed-tick bookkeeping + play /
// pause / seek over the pure sampleWalk resolver. The renderer never sees it; the game constructs it, calls
// advance() each tick, and writes position() / facing() into whatever sink it likes. Quantization to an
// integer sink happens at the game's write — the walker's math is float end-to-end.
struct PathWalker {
    // The cadence a bare-constructed walker resolves pacing against. EngineConfig::setActive fans the
    // configured cadence into it at startup (alongside AnimationPlayer / TweenPlayer<T>::defaultTiming), or
    // assign it directly, or override a single walker via .profile. A single process-wide default —
    // legitimate here: the platform is single-threaded, and this is a config default, not retained state.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    ArcLengthTable table{};              // the baked geometry, OWNED BY VALUE — bake once at construction
                                         //   (.table = curve.arcTable()); re-path = assign a new table
                                         //   (+ restart()). Self-contained; no coupling to the source curve.
    PathPacing     pacing{};             // the time → distance driver
    TimingProfile  profile = defaultTiming;
    std::uint64_t  elapsedTicks = 0;
    bool           playing = true;
    WalkSample     sample{};             // cached by advance() so the getters need no arguments

    // Each game tick: accrue elapsedTicks (ONLY while playing) and re-resolve via sampleWalk under `mode`. Mode
    // defaults to loopIndefinitely so a bare advance() just loops, like the other two players. A fresh
    // sample whose facing is zero (a dead spot on the path) keeps the previous heading — a mover parked on
    // a directionless point keeps pointing the way it was going.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(),
                 std::uint64_t deltaTicks = 1) noexcept;

    [[nodiscard]] Vec2  position() const noexcept { return sample.position; }
    [[nodiscard]] Vec2  facing()   const noexcept { return sample.facing; }
    [[nodiscard]] float distance() const noexcept { return sample.distance; }
    [[nodiscard]] bool  finished() const noexcept { return sample.finished; }

    void play()    noexcept;  // resume
    void pause()   noexcept;  // freeze at the current position
    void stop()    noexcept;  // pause + rewind to the path start
    void restart() noexcept;  // rewind + play
    // Jump to a wall-time offset (resolved to ticks via the profile); keeps the playing / paused state. A
    // seek-by-distance is not offered — it would need the pacing inverse, ill-defined for a non-monotone
    // DistanceTween; this mirrors TweenPlayer::seek exactly.
    void seek(std::chrono::nanoseconds at) noexcept;
};

}  // namespace retropp
