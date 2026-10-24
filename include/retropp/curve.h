#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "retropp/geometry.h"  // Vec2 — the curve's coordinate vocabulary (viewport pixels)

namespace retropp {

// ── Curve: the platform's one curve primitive ───────────────────────────────────────────────────────
//
// A pure-data / pure-resolver value type — the same pure-value shape as Transform and the pure-data +
// pure-resolver shape of Tween / Animation: PURE DATA + PURE FUNCTIONS, zero platform-owned state. There
// is no curve field the platform ticks into draw state; a consumer holds a Curve and calls a query. One
// type serves every consumer because each reaches for a different query: a curved effect-region
// boundary uses signedDistance(); a sprite path uses
// at() / tangent() / atDistance(); value-plotting uses at().
//
// A Curve is the *shape* (parameter → point). A timing driver (Tween / Easing) is the *speed along*
// it — orthogonal and composable: position(time) = curve.atDistance(driver(time)). The straight-line
// case (Curve::line) equals a Tween<Vec2> lerp exactly.
//
// Internally a Curve is a sequence of mixed-degree Bézier segments. Mixed-degree (not uniform-cubic)
// so a quadratic stays exactly a quadratic — its distance has a closed form — and a straight line is
// genuinely Linear (the degenerate curve). The three authoring front doors — Bézier handles,
// Catmull-Rom through-points, Hermite point+tangent — all author into this one internal form.

enum class CurveDegree : std::uint8_t { Linear = 1, Quadratic = 2, Cubic = 3 };

// One Bézier segment in viewport-pixel (Vec2) space. `degree` names how many control points are live:
//   Linear    = p0 → p1                       (p2, p3 ignored)
//   Quadratic = p0, p1 (control), p2          (p3 ignored)
//   Cubic     = p0, p1, p2 (controls), p3
// The platform's ONE internal curve form — Catmull-Rom and Hermite author into Cubic; a straight line
// is Linear. PURE DATA; identity is the named points + degree.
struct CurveSegment {
    Vec2        p0{}, p1{}, p2{}, p3{};
    CurveDegree degree = CurveDegree::Cubic;

    [[nodiscard]] bool operator==(const CurveSegment&) const = default;
};

// ── Constexpr per-segment evaluators (the curve math the .cpp sampling/SDF bodies build on) ─────────
//
// Plain de Casteljau / Bézier-derivative arithmetic — no transcendentals — so they fold at compile
// time and are static_assert-testable, the same constexpr split the Transform affine subset and
// Tween's lerp keep (the sqrt/atan2 paths live in curve.cpp). `u` is the LOCAL parameter ∈ [0,1]
// within the one segment; the whole-curve mapping is Curve::at / Curve::tangent.

namespace detail {
[[nodiscard]] constexpr Vec2  v2add(Vec2 a, Vec2 b) noexcept { return Vec2{a.x + b.x, a.y + b.y}; }
[[nodiscard]] constexpr Vec2  v2sub(Vec2 a, Vec2 b) noexcept { return Vec2{a.x - b.x, a.y - b.y}; }
[[nodiscard]] constexpr Vec2  v2mul(Vec2 a, float s) noexcept { return Vec2{a.x * s, a.y * s}; }
[[nodiscard]] constexpr float v2dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
}  // namespace detail

// The point at local parameter `u` ∈ [0,1] (clamped on entry), by degree (de Casteljau).
[[nodiscard]] constexpr Vec2 evalSegment(const CurveSegment& s, float u) noexcept {
    using namespace detail;
    const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    const float mu = 1.0f - cu;
    switch (s.degree) {
        case CurveDegree::Linear:
            return v2add(v2mul(s.p0, mu), v2mul(s.p1, cu));
        case CurveDegree::Quadratic:
            return v2add(v2add(v2mul(s.p0, mu * mu), v2mul(s.p1, 2.0f * mu * cu)),
                         v2mul(s.p2, cu * cu));
        case CurveDegree::Cubic:
        default:
            return v2add(v2add(v2mul(s.p0, mu * mu * mu), v2mul(s.p1, 3.0f * mu * mu * cu)),
                         v2add(v2mul(s.p2, 3.0f * mu * cu * cu), v2mul(s.p3, cu * cu * cu)));
    }
}

// The RAW (non-unit) derivative dB/du at local `u` — the direction of travel, magnitude = local
// speed. Curve::tangent unit-normalizes this (the sqrt lives in curve.cpp).
[[nodiscard]] constexpr Vec2 evalSegmentDerivative(const CurveSegment& s, float u) noexcept {
    using namespace detail;
    const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    const float mu = 1.0f - cu;
    switch (s.degree) {
        case CurveDegree::Linear:
            return v2sub(s.p1, s.p0);
        case CurveDegree::Quadratic:
            return v2add(v2mul(v2sub(s.p1, s.p0), 2.0f * mu), v2mul(v2sub(s.p2, s.p1), 2.0f * cu));
        case CurveDegree::Cubic:
        default:
            return v2add(v2add(v2mul(v2sub(s.p1, s.p0), 3.0f * mu * mu),
                               v2mul(v2sub(s.p2, s.p1), 6.0f * mu * cu)),
                         v2mul(v2sub(s.p3, s.p2), 3.0f * cu * cu));
    }
}

// A segment's start / end points by degree (the live first and last control point). Pure helpers the
// authoring appenders and the whole-curve mapping share.
[[nodiscard]] constexpr Vec2 segmentStart(const CurveSegment& s) noexcept { return s.p0; }
[[nodiscard]] constexpr Vec2 segmentEnd(const CurveSegment& s) noexcept {
    switch (s.degree) {
        case CurveDegree::Linear:    return s.p1;
        case CurveDegree::Quadratic: return s.p2;
        case CurveDegree::Cubic:
        default:                     return s.p3;
    }
}

// One entry of a baked arc-length table (ArcLengthTable): a cumulative distance along the curve plus the
// curve parameter (segment index + local u) sitting at that distance. PURE DATA.
struct ArcSample {
    float         distance = 0.0f;
    std::uint32_t segment  = 0;
    float         localU   = 0.0f;
    [[nodiscard]] bool operator==(const ArcSample&) const noexcept = default;
};

struct ArcLengthTable;  // a baked arc-length table over a Curve; defined just below Curve

// ── The curve ───────────────────────────────────────────────────────────────────────────────────
//
// A list of segments + an open/closed flag. Open = a path (a sprite walks it). Closed = a region
// boundary loop (the last segment's end joins the first segment's start; signedDistance gets a sign).
// Authoring goes through the named ctors / chainable appenders; aggregate init stays available.
struct Curve {
    std::vector<CurveSegment> segments;
    bool                      closed = false;

    [[nodiscard]] bool operator==(const Curve&) const = default;

    // ── Authoring front doors (a straight line is the degenerate curve) ──────────────────────────
    [[nodiscard]] static Curve line(Vec2 a, Vec2 b);
    [[nodiscard]] static Curve quadratic(Vec2 p0, Vec2 ctrl, Vec2 p1);
    [[nodiscard]] static Curve cubic(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1);
    // Catmull-Rom through the points: the curve passes THROUGH every input point. `closed` wraps the
    // neighbor tangents (a loop); open reflects phantom endpoints. ≥2 points required (fewer → empty).
    [[nodiscard]] static Curve throughPoints(std::span<const Vec2> pts, bool closed = false);
    // Hermite: a cubic leaving p0 along tangent0 and arriving at p1 along tangent1 (a `directional
    // vector` IS a tangent — the sprite-path {origin, direction} form). Exact Hermite↔Bézier identity.
    [[nodiscard]] static Curve hermite(Vec2 p0, Vec2 tangent0, Vec2 p1, Vec2 tangent1);

    // Chainable appenders (the Transform::then / Tween::then idiom) — each appends a segment starting
    // at the current end (the last segment's terminal point, or the origin if the curve is empty).
    Curve& lineTo(Vec2 to);
    Curve& quadraticTo(Vec2 ctrl, Vec2 to);
    Curve& cubicTo(Vec2 c0, Vec2 c1, Vec2 to);

    // ── Pure queries ─────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t count() const noexcept { return segments.size(); }
    [[nodiscard]] bool        empty() const noexcept { return segments.empty(); }

    // at(t): global t ∈ [0,1] (clamped) mapped to (segment, localU) UNIFORMLY per segment — the plain
    // parameter meaning (each segment owns an equal 1/N slice of t, regardless of its arc length).
    [[nodiscard]] Vec2 at(float t) const;
    // tangent(t): the UNIT direction of travel at global t (zero-length derivative → a zero vector).
    [[nodiscard]] Vec2 tangent(float t) const;

    // length(): total arc length, by per-segment sampling into a cumulative length table.
    [[nodiscard]] float length() const;
    // atDistance(s): the CONSTANT-SPEED query — the point at arc-length s ∈ [0, length] (clamped). The
    // distinct meaning from at(): atDistance(length/2) is the geometric midpoint by arc length, which
    // differs from at(0.5) for a non-uniform curve. Use this (not at()) to move along a path at an
    // even pace.
    [[nodiscard]] Vec2 atDistance(float s) const;
    // tangentAtDistance(s): the UNIT direction of travel at arc-length s — the facing that MATCHES
    // atDistance(s). Use this to orient a mover, NOT tangent(s / length()): on a non-uniform curve
    // s/length() is a parameter, not an arc-length, so tangent(s/length()) reads the heading at a
    // different point on the curve. Zero-length derivative → zero vector.
    [[nodiscard]] Vec2 tangentAtDistance(float s) const;

    // length() / atDistance() / tangentAtDistance() resample the curve on each call (cost linear in the
    // segment count). When you query ONE curve repeatedly — a mover walking a path every frame, or many
    // movers on it — bake the table once with arcTable() (below) and query the returned ArcLengthTable
    // instead; it samples the curve a single time and reuses it.

    // signedDistance(p): the CPU SDF — min distance from p to the curve (Linear exact, Quadratic exact
    // closed-form, Cubic by recursive subdivision), with a SIGN for `closed` curves (negative inside,
    // positive outside, by winding number). Open curves return the unsigned distance (sign meaningless).
    // The exact distance field, computed on the CPU.
    [[nodiscard]] float signedDistance(Vec2 p) const;
    // contains(p): closed curves only — signedDistance(p) <= 0. Always false for an open curve.
    [[nodiscard]] bool contains(Vec2 p) const;

    // arcTable(): bake a reusable arc-length table for repeated constant-speed queries — see
    // ArcLengthTable below. Samples the curve once; the returned value answers length / atDistance /
    // tangentAtDistance with no resample.
    [[nodiscard]] ArcLengthTable arcTable() const;
};

// A baked arc-length table over a Curve — the reuse path for the constant-speed queries. Build it once
// with Curve::arcTable(), then call length / atDistance / tangentAtDistance as often as you like with no
// per-call resampling. PURE DATA, self-contained (it copies the curve's segments, so it has no lifetime
// coupling to the source curve), game-owned — the platform never holds one, exactly like a Curve. Its
// results equal the curve's own on-call queries; it just skips the re-sampling. Build it after the curve
// is final; rebuild it if the curve's points change. This is the unit a path-walking cursor holds.
struct ArcLengthTable {
    std::vector<CurveSegment> segments;  // a copy of the source curve's segments
    std::vector<ArcSample>    samples;   // the cumulative arc-length table, one entry per sample

    [[nodiscard]] float length() const noexcept;          // total arc length
    [[nodiscard]] Vec2  atDistance(float s) const;        // constant-speed point at arc-length s
    [[nodiscard]] Vec2  tangentAtDistance(float s) const; // unit facing at arc-length s
};

}  // namespace retropp
