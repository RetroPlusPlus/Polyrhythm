#include "retropp/sprite_path.h"

#include <cmath>     // std::atan2, std::ceil, std::lround
#include <cstddef>   // std::size_t
#include <limits>    // std::numeric_limits
#include <span>      // std::span — throughPoints input
#include <utility>   // std::move

#include "retropp/draw_state.h"  // Sprite — applyTo writes into one
#include "retropp/transform.h"   // Transform — the composed scale/rotation write

namespace retropp {

// ── SpritePathMove named constructors ───────────────────────────────────────────────────────────────

SpritePathMove SpritePathMove::to(Vec2 destination) {
    return SpritePathMove{.kind = Kind::Line, .destination = destination};
}

SpritePathMove SpritePathMove::to(Vec2 origin, Vec2 destination) {
    return SpritePathMove{.kind = Kind::Line, .origin = origin, .destination = destination};
}

SpritePathMove SpritePathMove::through(std::vector<Vec2> points) {
    return SpritePathMove{.kind = Kind::ThroughPoints, .points = std::move(points)};
}

SpritePathMove SpritePathMove::through(Vec2 origin, std::vector<Vec2> points) {
    return SpritePathMove{.kind = Kind::ThroughPoints, .origin = origin, .points = std::move(points)};
}

SpritePathMove SpritePathMove::hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent) {
    return SpritePathMove{.kind               = Kind::Hermite,
                          .destination        = destination,
                          .originTangent      = originTangent,
                          .destinationTangent = destinationTangent};
}

SpritePathMove SpritePathMove::hermite(Vec2 origin, Vec2 destination, Vec2 originTangent,
                                       Vec2 destinationTangent) {
    return SpritePathMove{.kind               = Kind::Hermite,
                          .origin             = origin,
                          .destination        = destination,
                          .originTangent      = originTangent,
                          .destinationTangent = destinationTangent};
}

SpritePathMove SpritePathMove::onCurve(retropp::Curve c) {
    return SpritePathMove{.kind = Kind::Curve, .curve = std::move(c)};
}

// ── Compose + bake helpers ──────────────────────────────────────────────────────────────────────────

namespace {

// A node whose one-pass duration is this value never finishes: the sequence rests in it (Speed 0 on nonzero
// geometry, or a null distance-tween). The rollover walk stops at such a node — nothing after it is reached.
constexpr std::uint64_t kNeverFinishes = std::numeric_limits<std::uint64_t>::max();

// The resolver's output: the composed sample, the active-content node it landed in, whether the SEQUENCE
// finished under its mode, and — when finished — the tick at which it finished (so an auto-popped interrupt
// can flow its leftover ticks into the resumed content).
struct ActiveResolve {
    SpritePathSample sample{};
    std::size_t      nodeIndex  = 0;
    bool             finished   = false;
    std::uint64_t    finishTick = 0;
};

// The tangent facing as a clockwise degree angle in the engine's top-left-origin pixel space — exactly what
// Transform::rotation takes. A zero facing yields 0°.
float facingDegrees(Vec2 facing) noexcept {
    return std::atan2(facing.y, facing.x) * 57.29577951308232f;  // 180 / π
}

// Resolve a move spec to a curve. origin defaults to the inherited chain origin for the point/line/Hermite
// forms; the raw Curve form is exempt (it starts where its own geometry starts). ThroughPoints prepends the
// origin so the path travels FROM the origin THROUGH the listed points.
Curve buildCurve(const SpritePathMove& m, Vec2 origin) {
    const Vec2 o = m.origin.value_or(origin);
    switch (m.kind) {
        case SpritePathMove::Kind::Line:
            return Curve::line(o, m.destination);
        case SpritePathMove::Kind::ThroughPoints: {
            std::vector<Vec2> pts;
            pts.reserve(m.points.size() + 1);
            pts.push_back(o);
            pts.insert(pts.end(), m.points.begin(), m.points.end());
            return Curve::throughPoints(std::span<const Vec2>(pts), false);
        }
        case SpritePathMove::Kind::Hermite:
            return Curve::hermite(o, m.originTangent, m.destination, m.destinationTangent);
        case SpritePathMove::Kind::Curve:
            return m.curve;
    }
    return Curve{};  // unreachable — every kind handled
}

// One node's one-pass duration in WHOLE ticks, computed to match sampleWalk's single()-finish tick exactly, per
// pacing kind:
//   Speed         — the first tick t at which pxPerTick × t ≥ length (ceil), matching sampleWalk's `raw ≥ length`;
//                   Speed 0 (or ≤ 0) on nonzero geometry never finishes (the sentinel). Degenerate geometry → 0.
//   Eased         — ticksForDuration(duration), INDEPENDENT of geometry: a zero-length Eased node is the WAIT
//                   node (stands still for the duration).
//   DistanceTween — totalTicks(*distance); a null distance-tween never finishes (the parked sentinel).
std::uint64_t nodeDuration(const SpritePathNode& node, const ArcLengthTable& arc,
                           const TimingProfile& profile) noexcept {
    const float length = arc.length();
    switch (node.pacing.kind) {
        case PathPacing::Kind::Speed: {
            if (length <= 0.0f) {
                return 0;  // sampleWalk short-circuits a degenerate curve to finished — the pass carries no ticks
            }
            const float secondsPerTick = static_cast<float>(profile.tickPeriod().count()) * 1e-9f;
            const float pxPerTick       = node.pacing.pxPerSecond * secondsPerTick;
            if (pxPerTick <= 0.0f) {
                return kNeverFinishes;  // parked on nonzero geometry — rests here forever (the sentinel node)
            }
            std::uint64_t t = static_cast<std::uint64_t>(std::ceil(length / pxPerTick));
            // Float-correct so the boundary matches sampleWalk's `pxPerTick × t ≥ length` predicate exactly.
            while (pxPerTick * static_cast<float>(t) < length) ++t;
            while (t > 0 && pxPerTick * static_cast<float>(t - 1) >= length) --t;
            return t;
        }
        case PathPacing::Kind::Eased:
            return profile.ticksForDuration(node.pacing.duration);  // may be 0 (all-instantaneous)
        case PathPacing::Kind::DistanceTween:
            if (node.pacing.distance == nullptr) {
                return kNeverFinishes;  // the parked default distance-track — rests here (the sentinel node)
            }
            return totalTicks(*node.pacing.distance, profile);  // may be 0
    }
    return 0;  // unreachable — every kind handled
}

// Bake the active content's node list into a per-node pass: each node's geometry, its one-pass duration, and
// the position + facing its movement ends at (the next node's inherited origin). Node 0 departs from
// `chainStart`; node i from node i−1's end. Durations and end positions are CLOCK-INDEPENDENT — every wrap of
// the sequence replays the identical geometry, which is what makes advance() and seek() agree.
std::vector<BakedPathNode> bakePass(const std::vector<SpritePathNode>& nodes, Vec2 chainStart,
                                    const TimingProfile& profile) {
    std::vector<BakedPathNode> out;
    out.reserve(nodes.size());
    Vec2 origin = chainStart;
    for (const SpritePathNode& node : nodes) {
        BakedPathNode b{};
        b.arc      = buildCurve(node.move, origin).arcTable();
        b.duration = nodeDuration(node, b.arc, profile);
        // The movement's end position + facing: resolve one pass at its own duration (a sentinel never ends,
        // so use tick 0 — a parked node's position is its origin either way; nothing chains past it anyway).
        const std::uint64_t endTick = (b.duration == kNeverFinishes) ? 0 : b.duration;
        const WalkSample    end     = sampleWalk(b.arc, node.pacing, endTick, profile, PlaybackMode::single());
        b.endPosition = end.position;
        b.endFacing   = end.facing;
        out.push_back(std::move(b));
        origin = b.endPosition;  // the next node departs from here
    }
    return out;
}

// The hold-last heading a node inherits: the last non-zero end-facing among the nodes before the landing node
// in this pass (and, if we have wrapped, the previous identical pass's last non-zero end-facing), else the
// prior sample's facing. Deterministic from the pass alone — so a dead spot (a wait / sentinel node) shows the
// heading the sprite was traveling, and batched / seeked resolves match tick-by-tick.
Vec2 carriedFacing(const std::vector<BakedPathNode>& pass, std::size_t landingIndex, bool wrapped,
                   Vec2 priorFacing) noexcept {
    Vec2 held = priorFacing;
    if (wrapped) {
        for (const BakedPathNode& b : pass) {
            if (b.endFacing != Vec2{}) held = b.endFacing;
        }
    }
    for (std::size_t i = 0; i < landingIndex && i < pass.size(); ++i) {
        if (pass[i].endFacing != Vec2{}) held = pass[i].endFacing;
    }
    return held;
}

// Compose one node at its NODE-LOCAL clock `localTicks`: movement via sampleWalk under single() (one pass per
// entry), then the rotation / scale / facing / animation tracks off the same local clock. `heldFacing`
// supplies the heading across a directionless spot; `prior` supplies the hold-last flipX; `seqFinished` is the
// SEQUENCE-level finished flag (the node's own walk-finished is discarded — completion is a sequence property).
SpritePathSample composeNode(const SpritePathNode& node, const BakedPathNode& baked,
                             std::uint64_t localTicks, const TimingProfile& profile, Vec2 heldFacing,
                             const SpritePathSample& prior, bool seqFinished) noexcept {
    const WalkSample walk    = sampleWalk(baked.arc, node.pacing, localTicks, profile, PlaybackMode::single());
    Vec2             heading = walk.facing;
    if (heading == Vec2{}) {
        heading = heldFacing;  // hold the last real heading across a directionless spot
    }

    SpritePathSample next{};
    next.position = walk.position;
    next.facing   = heading;
    next.distance = walk.distance;
    next.finished = seqFinished;

    // Rotation track + RotateToFacing, summed.
    float rotation = 0.0f;
    if (node.rotationDegrees.has_value()) {
        rotation += sampleTween(*node.rotationDegrees, localTicks, profile, node.rotationMode).value;
    }
    if (node.facing == FacingPolicy::RotateToFacing) {
        rotation += facingDegrees(heading);
    }
    next.rotationDegrees = rotation;

    // Scale track (identity when absent).
    if (node.scale.has_value()) {
        next.scale = sampleTween(*node.scale, localTicks, profile, node.scaleMode).value;
    }

    // flipX under FacingPolicy::FlipX: mirror while traveling toward -x, hold the previous value while the
    // horizontal component is 0 (vertical travel does not flip-flop). Otherwise carry the cached value.
    bool flip = prior.flipX;
    if (node.facing == FacingPolicy::FlipX) {
        if (heading.x < 0.0f) {
            flip = true;
        } else if (heading.x > 0.0f) {
            flip = false;
        }
    }
    next.flipX = flip;

    // Frame track (nullptr when absent or empty).
    if (node.animation != nullptr && node.animation->count() > 0) {
        next.frame = &sampleAnimationFrame(*node.animation, localTicks, profile, node.animationMode);
    }

    return next;
}

// Resolve the composed sample for one node landed at (index, localTicks) under the given finished state.
ActiveResolve landNode(const std::vector<SpritePathNode>& nodes, const std::vector<BakedPathNode>& pass,
                       std::size_t index, std::uint64_t localTicks, bool wrapped,
                       const TimingProfile& profile, const SpritePathSample& prior, bool finished,
                       std::uint64_t finishTick) noexcept {
    const Vec2 held = carriedFacing(pass, index, wrapped, prior.facing);
    ActiveResolve r{};
    r.sample     = composeNode(nodes[index], pass[index], localTicks, profile, held, prior, finished);
    r.nodeIndex  = index;
    r.finished   = finished;
    r.finishTick = finishTick;
    return r;
}

// A parked sample at the chain start (empty content, loopNTimes(0), or a zero-progress sequence) — no node
// drives anything, so tracks are identity and the position is the chain anchor.
ActiveResolve parkedAtChainStart(Vec2 chainStart, const SpritePathSample& prior, bool finished) noexcept {
    ActiveResolve r{};
    r.sample.position = chainStart;
    r.sample.facing   = prior.facing;   // hold the last heading
    r.sample.flipX    = prior.flipX;    // hold the last mirror
    r.sample.finished = finished;
    r.nodeIndex       = 0;
    r.finished        = finished;
    return r;
}

// THE resolver — the single point of sequence truth, a PURE function of (content, elapsed, mode). advance()
// and seek() both funnel through it, so batched advances, tick-by-tick advances, and seeks all resolve the
// same landing from the same elapsed-tick count. Walks the node durations to find the landing node + its
// node-local clock, honoring the sequence-level PlaybackMode over the base node list (loop / rest / N laps /
// play-for-a-duration), then composes there.
ActiveResolve resolveActive(const std::vector<SpritePathNode>& nodes,
                            const std::vector<BakedPathNode>& pass, PlaybackMode mode,
                            const TimingProfile& profile, std::uint64_t elapsed,
                            const SpritePathSample& prior, Vec2 chainStart) noexcept {
    if (pass.empty()) {  // no content — park at the chain start
        return parkedAtChainStart(chainStart, prior, mode.kind != PlaybackMode::Kind::LoopIndefinitely);
    }

    // Pass structure: the finite prefix's total, and whether a sentinel node caps forward progress.
    bool          hasSentinel = false;
    std::uint64_t passTotal   = 0;
    for (const BakedPathNode& b : pass) {
        if (b.duration == kNeverFinishes) {
            hasSentinel = true;
            break;
        }
        passTotal += b.duration;
    }

    // Sequence-mode clamp: the effective clock to resolve, how many passes the mode permits, and any
    // time-based finish (PlayForDuration cuts by wall time regardless of movement).
    std::uint64_t effElapsed  = elapsed;
    std::uint64_t maxPasses   = std::numeric_limits<std::uint64_t>::max();  // "unbounded"
    bool          preFinished = false;
    std::uint64_t finishTick  = 0;
    switch (mode.kind) {
        case PlaybackMode::Kind::Single:
            maxPasses = 1;
            break;
        case PlaybackMode::Kind::LoopIndefinitely:
            break;  // unbounded
        case PlaybackMode::Kind::LoopNTimes:
            if (mode.loopCount == 0) {  // never plays → rests at the chain start, finished
                return landNode(nodes, pass, 0, 0, false, profile, prior, true, 0);
            }
            maxPasses = mode.loopCount;
            break;
        case PlaybackMode::Kind::PlayForDuration: {
            const std::uint64_t cut = profile.ticksForDuration(mode.duration);
            if (cut == 0 || elapsed >= cut) {
                preFinished = true;
                finishTick  = cut;
                effElapsed  = (cut == 0) ? 0 : (cut - 1);  // hold the sample shown at the cutoff tick
            }
            break;  // unbounded until the cutoff
        }
    }

    // No forward progress possible (every node zero-duration, none a sentinel): park at the chain start.
    if (passTotal == 0 && !hasSentinel) {
        const bool finished = preFinished || (mode.kind != PlaybackMode::Kind::LoopIndefinitely);
        return parkedAtChainStart(chainStart, prior, finished);
    }

    // Walk the passes: subtract each node's duration from the effective clock; a sentinel node (or a node the
    // clock lands inside) is the landing; a fully-consumed pass rolls over (wrap or rest, per the mode).
    std::uint64_t t         = effElapsed;
    std::uint64_t passesDone = 0;
    for (;;) {
        std::uint64_t rem      = t;
        std::size_t   landIdx  = pass.size();
        std::uint64_t local    = 0;
        for (std::size_t i = 0; i < pass.size(); ++i) {
            if (pass[i].duration == kNeverFinishes) {  // rest here forever (localTicks accrue in the sentinel)
                landIdx = i;
                local   = rem;
                break;
            }
            if (rem < pass[i].duration) {  // the clock lands in node i
                landIdx = i;
                local   = rem;
                break;
            }
            rem -= pass[i].duration;
        }

        if (landIdx < pass.size()) {  // landed (mid-node or resting in a sentinel)
            const bool wrapped = (passesDone > 0);
            return landNode(nodes, pass, landIdx, local, wrapped, profile, prior, preFinished,
                            preFinished ? finishTick : 0);
        }

        // A whole pass completed with `rem` ticks left over.
        ++passesDone;
        if (passesDone >= maxPasses) {  // a finite mode (Single / LoopNTimes) has played all its passes
            // Rest at the last node's end: the MOVEMENT holds the endpoint (sampleWalk single() past its
            // duration), but the NODE-LOCAL CLOCK keeps growing so the last node's tracks keep resolving —
            // a courier resting at the route's end whose walk cycle is still playing. localTicks is the
            // ongoing time in that last node = its own duration + the overshoot past the final pass.
            const std::size_t   last     = pass.size() - 1;
            const std::uint64_t restLocal = pass[last].duration + rem;
            return landNode(nodes, pass, last, restLocal, true, profile, prior, true, passTotal * maxPasses);
        }
        t = rem;  // wrap into the next pass (node 0 re-chains from the same start — identical geometry)
    }
}

// ── SpritePath runtime helpers ──────────────────────────────────────────────────────────────────────

// Whether ANY node the path currently holds (base + every stacked interrupt) declares a transform driver /
// uses FlipX — the write-envelope union, so a transition off a rotating / scaling / mirroring node clears the
// stale state instead of freezing it (§ the applyTo envelope).
bool nodeDeclaresTransform(const SpritePathNode& n) noexcept {
    return n.rotationDegrees.has_value() || n.scale.has_value()
        || n.facing == FacingPolicy::RotateToFacing;
}

bool anyDeclaresTransform(const SpritePath& p) noexcept {
    for (const SpritePathNode& n : p.nodes) {
        if (nodeDeclaresTransform(n)) return true;
    }
    for (const SpritePathInterrupt& f : p.interrupts) {
        for (const SpritePathNode& n : f.nodes) {
            if (nodeDeclaresTransform(n)) return true;
        }
    }
    return false;
}

bool anyUsesFlipX(const SpritePath& p) noexcept {
    for (const SpritePathNode& n : p.nodes) {
        if (n.facing == FacingPolicy::FlipX) return true;
    }
    for (const SpritePathInterrupt& f : p.interrupts) {
        for (const SpritePathNode& n : f.nodes) {
            if (n.facing == FacingPolicy::FlipX) return true;
        }
    }
    return false;
}

// Build the base pass once (lazily) — the chain start is only known then, and designated init cannot bake.
void ensureBaked(SpritePath& p) {
    if (!p.baked) {
        p.chainStart = p.start;
        p.pass       = bakePass(p.nodes, p.start, p.profile);
        p.baked      = true;
    }
}

// Shift a baked pass's geometry by `delta` — its segment control points and each node's end position. The
// arc-length samples index into the segments by (segment, localU), so moving the control points moves every
// atDistance()/tangentAtDistance() result with them; facing (a direction) is unchanged.
void translatePass(std::vector<BakedPathNode>& pass, Vec2 delta) {
    for (BakedPathNode& b : pass) {
        for (CurveSegment& s : b.arc.segments) {
            s.p0 = Vec2{s.p0.x + delta.x, s.p0.y + delta.y};
            s.p1 = Vec2{s.p1.x + delta.x, s.p1.y + delta.y};
            s.p2 = Vec2{s.p2.x + delta.x, s.p2.y + delta.y};
            s.p3 = Vec2{s.p3.x + delta.x, s.p3.y + delta.y};
        }
        b.endPosition = Vec2{b.endPosition.x + delta.x, b.endPosition.y + delta.y};
    }
}

// Restore the runtime the top interrupt suspended — the suspended pass is baked, so nothing is recomputed.
// Under ResumePolicy::Continue the resumed content carries on from where the sprite now stands: its geometry
// shifts by the net displacement the interrupt introduced, so the route drifts on instead of snapping back.
// Under Return the suspended state is restored verbatim. The active content becomes whatever sat beneath.
void popFrameRestore(SpritePath& p) {
    SpritePathInterrupt& f       = p.interrupts.back();
    const Vec2           livePos = p.sample.position;  // where the interrupt left the sprite

    p.elapsedTicks = f.suspendedElapsedTicks;
    p.pass         = std::move(f.suspendedPass);
    p.chainStart   = f.suspendedChainStart;
    p.nodeIndex    = f.suspendedNodeIndex;
    p.sample       = f.suspendedSample;

    if (f.resume == ResumePolicy::Continue) {
        const Vec2 delta{livePos.x - p.sample.position.x, livePos.y - p.sample.position.y};
        translatePass(p.pass, delta);
        p.chainStart      = Vec2{p.chainStart.x + delta.x, p.chainStart.y + delta.y};
        p.sample.position = livePos;  // the getters read the current position immediately (the next resolve agrees)
    }

    p.interrupts.pop_back();
}

// Resolve the active content and auto-pop any interrupt that finished — its leftover ticks (this batch's time
// beyond the interrupt's end) flowing into the resumed content, cascading through nested pops.
void resolveAndAutoPop(SpritePath& p, PlaybackMode baseMode) {
    for (;;) {
        const bool                         onBase     = p.interrupts.empty();
        const PlaybackMode                 activeMode = onBase ? baseMode : p.interrupts.back().mode;
        const std::vector<SpritePathNode>& activeNodes = onBase ? p.nodes : p.interrupts.back().nodes;

        const ActiveResolve r = resolveActive(activeNodes, p.pass, activeMode, p.profile, p.elapsedTicks,
                                              p.sample, p.chainStart);
        if (r.finished && !onBase) {  // an interrupt finished → pop and flow the leftover into the parent
            const std::uint64_t leftover = p.elapsedTicks - r.finishTick;
            popFrameRestore(p);
            p.elapsedTicks += leftover;
            continue;
        }
        p.sample    = r.sample;
        p.nodeIndex = r.nodeIndex;
        return;
    }
}

}  // namespace

// ── SpritePath ───────────────────────────────────────────────────────────────────────────────────────

void SpritePath::advance(PlaybackMode mode, std::uint64_t deltaTicks) {
    ensureBaked(*this);
    if (playing) {
        elapsedTicks += deltaTicks;
    }
    resolveAndAutoPop(*this, mode);
}

void SpritePath::applyTo(Sprite& s) const {
    // Position → the sprite's top-left, quantized — the one quantize point.
    s.x = static_cast<int>(std::lround(sample.position.x));
    s.y = static_cast<int>(std::lround(sample.position.y));

    const SpritePathNode* cur = currentNode();

    // Frame fields only when the CURRENT node has an animation track (sample.frame is set iff so). A node
    // without one leaves the last art showing — holding art is meaningful; writing "no art" is not. Within a
    // track, an art frame writes the art; a palette-only frame (hasArt() == false) recolors the carried-over
    // art — so palette always writes, the art fields only when the frame carries art.
    if (cur != nullptr && sample.frame != nullptr) {
        if (sample.frame->hasArt()) {
            s.atlas = sample.frame->atlas();
            s.tile  = sample.frame->tile();
            s.size  = sample.frame->size();
        }
        s.palette = sample.frame->palette;
    }

    // A geometric transform when ANY node the path holds declares one — the sample carries identity while the
    // current node drives neither axis, so a tumble STOPS instead of freezing at the last rotation. Pivot
    // defaults to the sprite's center using the size AFTER the frame write; scale composes first, then rotation.
    if (anyDeclaresTransform(*this)) {
        const float px = (cur != nullptr && cur->pivot) ? cur->pivot->x : static_cast<float>(s.size.width) * 0.5f;
        const float py = (cur != nullptr && cur->pivot) ? cur->pivot->y : static_cast<float>(s.size.height) * 0.5f;
        s.transform = Transform::scale(sample.scale.x, sample.scale.y, px, py)
                          .then(Transform::rotation(sample.rotationDegrees, px, py));
    }

    // flipX when ANY node uses the FlipX policy — the held value carries across nodes that don't drive it.
    if (anyUsesFlipX(*this)) {
        s.flipX = sample.flipX;
    }
}

const SpritePathNode* SpritePath::currentNode() const noexcept {
    const std::vector<SpritePathNode>& active = interrupts.empty() ? nodes : interrupts.back().nodes;
    if (nodeIndex >= active.size()) {
        return nullptr;
    }
    return &active[nodeIndex];
}

void SpritePath::interrupt(std::vector<SpritePathNode> newNodes, PlaybackMode mode, ResumePolicy resume) {
    ensureBaked(*this);

    // Snapshot the whole current runtime into a frame (suspended verbatim, pass included).
    SpritePathInterrupt frame{};
    frame.nodes                 = std::move(newNodes);
    frame.mode                  = mode;
    frame.resume                = resume;
    frame.chainStart            = sample.position;
    frame.suspendedElapsedTicks = elapsedTicks;
    frame.suspendedPass         = std::move(pass);
    frame.suspendedChainStart   = chainStart;
    frame.suspendedNodeIndex    = nodeIndex;
    frame.suspendedSample       = sample;

    // Switch the active content to the interrupt: it departs from the sprite's current position at tick 0.
    chainStart   = sample.position;
    elapsedTicks = 0;
    nodeIndex    = 0;
    pass         = bakePass(frame.nodes, chainStart, profile);
    interrupts.push_back(std::move(frame));

    // Resolve immediately so the getters are fresh (an instantly-finished interrupt auto-pops here).
    resolveAndAutoPop(*this, PlaybackMode::loopIndefinitely());
}

void SpritePath::popInterrupt() {
    if (interrupts.empty()) {
        return;
    }
    // Restore the suspended runtime — the suspended sample already reflects the resumed content exactly where
    // it was suspended, so the next advance(mode) continues from there under the real base mode.
    popFrameRestore(*this);
}

void SpritePath::play() { playing = true; }

void SpritePath::pause() { playing = false; }

void SpritePath::stop() {
    interrupts.clear();
    chainStart   = start;
    pass         = bakePass(nodes, start, profile);
    baked        = true;
    playing      = false;
    elapsedTicks = 0;
    nodeIndex    = 0;
    const ActiveResolve r =
        resolveActive(nodes, pass, PlaybackMode::loopIndefinitely(), profile, 0, sample, start);
    sample    = r.sample;
    nodeIndex = r.nodeIndex;
}

void SpritePath::restart() {
    interrupts.clear();
    chainStart   = start;
    pass         = bakePass(nodes, start, profile);
    baked        = true;
    playing      = true;
    elapsedTicks = 0;
    nodeIndex    = 0;
    const ActiveResolve r =
        resolveActive(nodes, pass, PlaybackMode::loopIndefinitely(), profile, 0, sample, start);
    sample    = r.sample;
    nodeIndex = r.nodeIndex;
}

void SpritePath::seek(std::chrono::nanoseconds at) {
    ensureBaked(*this);
    elapsedTicks = profile.ticksForDuration(at);

    const bool                         onBase     = interrupts.empty();
    const PlaybackMode                 activeMode = onBase ? PlaybackMode::loopIndefinitely()
                                                          : interrupts.back().mode;
    const std::vector<SpritePathNode>& activeNodes = onBase ? nodes : interrupts.back().nodes;

    const ActiveResolve r =
        resolveActive(activeNodes, pass, activeMode, profile, elapsedTicks, sample, chainStart);
    sample    = r.sample;
    nodeIndex = r.nodeIndex;
    // The stack is left untouched — seek drives the active content's clock only (no auto-pop).
}

}  // namespace retropp
