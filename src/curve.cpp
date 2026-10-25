#include "retropp/curve.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace retropp {

namespace {

using detail::v2add;
using detail::v2dot;
using detail::v2mul;
using detail::v2sub;

// ── Local scalar/vector helpers (the sqrt/transcendental side that stays out of the header) ─────────

[[nodiscard]] float v2len(Vec2 a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y); }
[[nodiscard]] float v2dist(Vec2 a, Vec2 b) noexcept { return v2len(v2sub(a, b)); }
[[nodiscard]] float dot2(Vec2 v) noexcept { return v.x * v.x + v.y * v.y; }
[[nodiscard]] Vec2  v2mid(Vec2 a, Vec2 b) noexcept { return v2mul(v2add(a, b), 0.5f); }

[[nodiscard]] float clamp01(float t) noexcept { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

// Distance from `p` to the segment a→b (clamped projection). The Linear-degree exact distance, and the
// leaf of the cubic subdivision.
[[nodiscard]] float pointSegmentDistance(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const Vec2  ab = v2sub(b, a);
    const Vec2  ap = v2sub(p, a);
    const float ee = v2dot(ab, ab);
    const float t  = ee > 0.0f ? clamp01(v2dot(ap, ab) / ee) : 0.0f;
    return v2dist(p, v2add(a, v2mul(ab, t)));
}

// Exact unsigned distance from `pos` to the quadratic Bézier A, B(control), C — the analytic closed
// form (a depressed-cubic root solve; the standard sdBezier). When the control makes the curve
// degenerate (A − 2B + C ≈ 0, i.e. a straight parameterization), it falls back to the segment A→C.
[[nodiscard]] float quadraticUnsignedDistance(Vec2 pos, Vec2 A, Vec2 B, Vec2 C) noexcept {
    const Vec2  a  = v2sub(B, A);
    const Vec2  b  = v2add(v2sub(A, v2mul(B, 2.0f)), C);  // A − 2B + C
    const float bb = v2dot(b, b);
    if (bb < 1e-9f) {  // control collinear / curve is straight → quadratic degenerates to a line
        return pointSegmentDistance(pos, A, C);
    }
    const Vec2  c  = v2mul(a, 2.0f);
    const Vec2  d  = v2sub(A, pos);
    const float kk = 1.0f / bb;
    const float kx = kk * v2dot(a, b);
    const float ky = kk * (2.0f * v2dot(a, a) + v2dot(d, b)) / 3.0f;
    const float kz = kk * v2dot(d, a);

    const float p  = ky - kx * kx;
    const float p3 = p * p * p;
    const float q  = kx * (2.0f * kx * kx - 3.0f * ky) + kz;
    const float h  = q * q + 4.0f * p3;

    float res;
    if (h >= 0.0f) {  // one real root
        const float hs = std::sqrt(h);
        const float x0 = (hs - q) * 0.5f;
        const float x1 = (-hs - q) * 0.5f;
        const float t  = clamp01(std::cbrt(x0) + std::cbrt(x1) - kx);  // cbrt preserves sign
        res = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t)), t)));
    } else {  // three real roots — take the nearest
        const float z = std::sqrt(-p);
        const float v = std::acos(q / (p * z * 2.0f)) / 3.0f;
        const float m = std::cos(v);
        const float n = std::sin(v) * 1.7320508075688772f;  // √3
        const float t0 = clamp01((m + m) * z - kx);
        const float t1 = clamp01((-n - m) * z - kx);
        const float t2 = clamp01((n - m) * z - kx);
        const float r0 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t0)), t0)));
        const float r1 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t1)), t1)));
        const float r2 = dot2(v2add(d, v2mul(v2add(c, v2mul(b, t2)), t2)));
        res = std::min(r0, std::min(r1, r2));
    }
    return std::sqrt(std::max(res, 0.0f));
}

// Unsigned distance from `p` to the cubic p0,p1,p2,p3 by recursive de Casteljau subdivision: split at
// 0.5 until the control hull is flat to tolerance, then take the leaf chord's segment distance. The
// tolerance bounds the geometric error; the recursion depth is bounded so a pathological curve can't
// spin forever.
constexpr float kCubicFlatTol = 0.02f;  // viewport px — half a hundredth of a tile is far below visible
constexpr int   kCubicMaxDepth = 24;

[[nodiscard]] float cubicDistanceRec(Vec2 p, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, int depth) noexcept {
    // Flatness: how far the interior controls bow off the chord p0→p3.
    const float bow = std::max(pointSegmentDistance(p1, p0, p3), pointSegmentDistance(p2, p0, p3));
    if (depth >= kCubicMaxDepth || bow <= kCubicFlatTol) {
        return pointSegmentDistance(p, p0, p3);
    }
    const Vec2 p01  = v2mid(p0, p1), p12 = v2mid(p1, p2), p23 = v2mid(p2, p3);
    const Vec2 p012 = v2mid(p01, p12), p123 = v2mid(p12, p23);
    const Vec2 mid  = v2mid(p012, p123);
    return std::min(cubicDistanceRec(p, p0, p01, p012, mid, depth + 1),
                    cubicDistanceRec(p, mid, p123, p23, p3, depth + 1));
}

// Unsigned distance from `p` to one segment, by degree.
[[nodiscard]] float segmentUnsignedDistance(const CurveSegment& s, Vec2 p) noexcept {
    switch (s.degree) {
        case CurveDegree::Linear:    return pointSegmentDistance(p, s.p0, s.p1);
        case CurveDegree::Quadratic: return quadraticUnsignedDistance(p, s.p0, s.p1, s.p2);
        case CurveDegree::Cubic:
        default:                     return cubicDistanceRec(p, s.p0, s.p1, s.p2, s.p3, 0);
    }
}

// Arc length of one segment by uniform polyline sampling.
constexpr int kArcSamplesPerSegment = 16;

[[nodiscard]] float segmentArcLength(const CurveSegment& s) noexcept {
    Vec2  prev = evalSegment(s, 0.0f);
    float total = 0.0f;
    for (int i = 1; i <= kArcSamplesPerSegment; ++i) {
        const Vec2 cur = evalSegment(s, static_cast<float>(i) / kArcSamplesPerSegment);
        total += v2dist(prev, cur);
        prev = cur;
    }
    return total;
}

}  // namespace

// ── Authoring front doors ────────────────────────────────────────────────────────────────────────

Curve Curve::line(Vec2 a, Vec2 b) {
    return Curve{{CurveSegment{a, b, Vec2{}, Vec2{}, CurveDegree::Linear}}, false};
}

Curve Curve::quadratic(Vec2 p0, Vec2 ctrl, Vec2 p1) {
    return Curve{{CurveSegment{p0, ctrl, p1, Vec2{}, CurveDegree::Quadratic}}, false};
}

Curve Curve::cubic(Vec2 p0, Vec2 c0, Vec2 c1, Vec2 p1) {
    return Curve{{CurveSegment{p0, c0, c1, p1, CurveDegree::Cubic}}, false};
}

Curve Curve::hermite(Vec2 p0, Vec2 tangent0, Vec2 p1, Vec2 tangent1) {
    // Exact Hermite↔Bézier identity: c0 = p0 + t0/3, c1 = p1 − t1/3.
    const Vec2 c0 = v2add(p0, v2mul(tangent0, 1.0f / 3.0f));
    const Vec2 c1 = v2sub(p1, v2mul(tangent1, 1.0f / 3.0f));
    return cubic(p0, c0, c1, p1);
}

Curve Curve::throughPoints(std::span<const Vec2> pts, bool closed) {
    Curve out;
    out.closed = closed;
    const std::size_t n = pts.size();
    if (n < 2) return out;  // need at least two points to form a span

    // Neighbor access with endpoint handling: wrap for closed, reflect a phantom point for open.
    const auto P = [&](std::ptrdiff_t k) -> Vec2 {
        const std::ptrdiff_t cnt = static_cast<std::ptrdiff_t>(n);
        if (closed) {
            return pts[static_cast<std::size_t>(((k % cnt) + cnt) % cnt)];
        }
        if (k < 0)    return v2sub(v2mul(pts[0], 2.0f), pts[1]);              // reflect P0 across P1
        if (k >= cnt) return v2sub(v2mul(pts[n - 1], 2.0f), pts[n - 2]);     // reflect P(n-1) across P(n-2)
        return pts[static_cast<std::size_t>(k)];
    };

    // Uniform Catmull-Rom → cubic Bézier: for span Pi→Pi+1, control1 = Pi + (Pi+1 − Pi−1)/6,
    // control2 = Pi+1 − (Pi+2 − Pi)/6 — the curve passes THROUGH every input point.
    const std::ptrdiff_t spans = closed ? static_cast<std::ptrdiff_t>(n)
                                        : static_cast<std::ptrdiff_t>(n) - 1;
    out.segments.reserve(static_cast<std::size_t>(spans));
    for (std::ptrdiff_t i = 0; i < spans; ++i) {
        const Vec2 pPrev = P(i - 1), p0 = P(i), p1 = P(i + 1), pNext = P(i + 2);
        const Vec2 c0 = v2add(p0, v2mul(v2sub(p1, pPrev), 1.0f / 6.0f));
        const Vec2 c1 = v2sub(p1, v2mul(v2sub(pNext, p0), 1.0f / 6.0f));
        out.segments.push_back(CurveSegment{p0, c0, c1, p1, CurveDegree::Cubic});
    }
    return out;
}

// ── Chainable appenders ──────────────────────────────────────────────────────────────────────────

Curve& Curve::lineTo(Vec2 to) {
    const Vec2 from = segments.empty() ? Vec2{} : segmentEnd(segments.back());
    segments.push_back(CurveSegment{from, to, Vec2{}, Vec2{}, CurveDegree::Linear});
    return *this;
}

Curve& Curve::quadraticTo(Vec2 ctrl, Vec2 to) {
    const Vec2 from = segments.empty() ? Vec2{} : segmentEnd(segments.back());
    segments.push_back(CurveSegment{from, ctrl, to, Vec2{}, CurveDegree::Quadratic});
    return *this;
}

Curve& Curve::cubicTo(Vec2 c0, Vec2 c1, Vec2 to) {
    const Vec2 from = segments.empty() ? Vec2{} : segmentEnd(segments.back());
    segments.push_back(CurveSegment{from, c0, c1, to, CurveDegree::Cubic});
    return *this;
}

// ── Pure queries ─────────────────────────────────────────────────────────────────────────────────

namespace {

// Map global t ∈ [0,1] to (segment index, local u), UNIFORMLY per segment. Precondition: non-empty.
struct SegmentParam {
    std::size_t index;
    float       localU;
};
[[nodiscard]] SegmentParam locateUniform(std::size_t count, float t) noexcept {
    const float       ct     = clamp01(t);
    const float       scaled = ct * static_cast<float>(count);
    std::size_t       idx    = static_cast<std::size_t>(scaled);
    if (idx >= count) idx = count - 1;  // t == 1 lands one past the last segment; pin it to the end
    return SegmentParam{idx, scaled - static_cast<float>(idx)};
}

// The whole-curve arc table is a vector of ArcSample (the public type in curve.h): each entry's
// cumulative distance AND the curve parameter (segment + localU) it sits at — so a distance query
// resolves to a parameter, not only a position. atDistance and tangentAtDistance share this: the point IS
// evalSegment at the parameter, the facing IS the unit derivative at the SAME parameter, so they never
// disagree about where the mover is.
struct Located {
    std::size_t seg;
    float       u;
};

// Sample every segment at u = 0 … 1 into a cumulative-distance table. Each segment contributes one entry
// per sample including both endpoints; consecutive segments meet at a coincident point (a zero-length
// step), so the boundary distance is shared by the two entries. Precondition: non-empty.
[[nodiscard]] std::vector<ArcSample> buildArcTable(const std::vector<CurveSegment>& segs) {
    std::vector<ArcSample> table;
    table.reserve(segs.size() * (kArcSamplesPerSegment + 1));
    float acc   = 0.0f;
    Vec2  prev{};
    bool  first = true;
    for (std::size_t si = 0; si < segs.size(); ++si) {
        for (int i = 0; i <= kArcSamplesPerSegment; ++i) {
            const float u   = static_cast<float>(i) / kArcSamplesPerSegment;
            const Vec2  cur = evalSegment(segs[si], u);
            if (!first) acc += v2dist(prev, cur);
            first = false;
            table.push_back(ArcSample{acc, static_cast<std::uint32_t>(si), u});
            prev = cur;
        }
    }
    return table;
}

// Resolve arc-length `target` to a (segment, localU): find the bracketing pair and lerp localU within it.
// A bracket straddling a segment boundary (a zero-length distance span) snaps to the end of the lower
// segment. Precondition: table non-empty, target ∈ [0, table.back().distance].
[[nodiscard]] Located locateByDistance(const std::vector<ArcSample>& table, float target) noexcept {
    std::size_t hi = 1;
    while (hi < table.size() && table[hi].distance < target) ++hi;
    if (hi >= table.size()) return Located{table.back().segment, table.back().localU};
    const ArcSample& lo = table[hi - 1];
    const ArcSample& up = table[hi];
    if (lo.segment != up.segment) return Located{lo.segment, lo.localU};  // boundary: end of lower segment
    const float span = up.distance - lo.distance;
    const float f    = span > 0.0f ? (target - lo.distance) / span : 0.0f;
    return Located{lo.segment, lo.localU + (up.localU - lo.localU) * f};
}

}  // namespace

Vec2 Curve::at(float t) const {
    if (segments.empty()) return Vec2{};
    const SegmentParam sp = locateUniform(segments.size(), t);
    return evalSegment(segments[sp.index], sp.localU);
}

Vec2 Curve::tangent(float t) const {
    if (segments.empty()) return Vec2{};
    const SegmentParam sp = locateUniform(segments.size(), t);
    const Vec2  d   = evalSegmentDerivative(segments[sp.index], sp.localU);
    const float len = v2len(d);
    if (len <= 1e-6f) return Vec2{};  // zero-length derivative (degenerate segment / cusp) → zero
    return Vec2{d.x / len, d.y / len};
}

float Curve::length() const {
    float total = 0.0f;
    for (const CurveSegment& s : segments) total += segmentArcLength(s);
    return total;
}

ArcLengthTable Curve::arcTable() const {
    return ArcLengthTable{segments, buildArcTable(segments)};
}

// The Curve's own constant-speed queries bake a table per call and query it — the convenient one-shot
// form. For repeated queries on one curve, hold an ArcLengthTable (Curve::arcTable) and skip the rebuild.
Vec2 Curve::atDistance(float s) const { return arcTable().atDistance(s); }
Vec2 Curve::tangentAtDistance(float s) const { return arcTable().tangentAtDistance(s); }

// ── ArcLengthTable: the baked reuse path (same results as the Curve's on-call queries, no resample) ──

float ArcLengthTable::length() const noexcept {
    return samples.empty() ? 0.0f : samples.back().distance;
}

Vec2 ArcLengthTable::atDistance(float s) const {
    if (samples.empty() || segments.empty()) return Vec2{};
    const float total = samples.back().distance;
    if (total <= 0.0f) return evalSegment(segments[0], 0.0f);  // zero-length curve
    const float   target = s < 0.0f ? 0.0f : (s > total ? total : s);
    const Located loc    = locateByDistance(samples, target);
    return evalSegment(segments[loc.seg], loc.u);
}

Vec2 ArcLengthTable::tangentAtDistance(float s) const {
    if (samples.empty() || segments.empty()) return Vec2{};
    const float total = samples.back().distance;
    if (total <= 0.0f) return Vec2{};  // zero-length curve → no direction
    const float   target = s < 0.0f ? 0.0f : (s > total ? total : s);
    const Located loc    = locateByDistance(samples, target);
    const Vec2    d      = evalSegmentDerivative(segments[loc.seg], loc.u);
    const float   len    = v2len(d);
    if (len <= 1e-6f) return Vec2{};  // degenerate location → zero
    return Vec2{d.x / len, d.y / len};
}

float Curve::signedDistance(Vec2 p) const {
    if (segments.empty()) return std::numeric_limits<float>::infinity();
    float d = std::numeric_limits<float>::infinity();
    for (const CurveSegment& s : segments) d = std::min(d, segmentUnsignedDistance(s, p));
    if (!closed) return d;  // open curve: sign is meaningless

    // Sign by winding number: accumulate the turning angle of (boundary − p) around the closed loop.
    // |total| ≈ 2π when p is enclosed, ≈ 0 when outside. Dense uniform sampling over the loop.
    const int   samples = std::max<int>(64, static_cast<int>(segments.size()) * 32);
    float       total   = 0.0f;
    Vec2        prevDir = v2sub(at(0.0f), p);
    for (int i = 1; i <= samples; ++i) {
        const Vec2  dir   = v2sub(at(static_cast<float>(i) / static_cast<float>(samples)), p);
        const float cross = prevDir.x * dir.y - prevDir.y * dir.x;
        const float dotp  = prevDir.x * dir.x + prevDir.y * dir.y;
        total += std::atan2(cross, dotp);
        prevDir = dir;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const bool inside = std::abs(total) > kPi;  // ~2π inside, ~0 outside
    return inside ? -d : d;
}

bool Curve::contains(Vec2 p) const {
    if (!closed) return false;  // containment is defined for region-boundary loops only
    return signedDistance(p) <= 0.0f;
}

}  // namespace retropp
