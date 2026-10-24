#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "retropp/animation.h"    // Animation, AnimationFrame, PlaybackMode, sampleAnimationFrame
#include "retropp/curve.h"        // Curve, ArcLengthTable
#include "retropp/geometry.h"     // Vec2
#include "retropp/path_walker.h"  // PathPacing — the movement driver, resolved via sampleWalk
#include "retropp/timing.h"       // TimingProfile
#include "retropp/tween.h"        // Tween<float>, Tween<Vec2> — the rotation / scale tracks

namespace retropp {

struct Sprite;  // draw_state.h — applyTo() writes into one; only a reference is named here

// ── Sprite paths: SpritePath, the movement orchestrator ─────────────────────────────────────────────
//
// One game-owned cursor that drives a sprite along a route while it rotates, scales, and animates — the
// player one level up from PathWalker. Where PathWalker resolves elapsed ticks → a position + facing and
// nothing else, SpritePath composes that movement with concurrent rotation and scale tracks, a frame
// animation, and a facing policy, all off ONE elapsed-tick clock, and writes the composed result into a
// Sprite. It is the movement analog of the whole player family: pure-data content (SpritePathNode) plus a
// pure resolver internally, wrapped in a game-owned cursor whose state lives in the game's object, not the
// platform. The renderer never sees it.
//
// A path plays a SEQUENCE of nodes back-to-back: each node names where it travels, how fast, which way it
// faces, and the tracks that run alongside the move. Node i's movement, when it authors no origin, departs
// from node i−1's end — a patrol route is a chain of legs. A sequence-level PlaybackMode (passed to
// advance()) decides what happens after the last node: loop, rest, N laps, or play-for-a-duration. On top of
// the base sequence sits the interrupt stack — suspend the whole current playback, run a detour departing
// from where the sprite stands, and resume EXACTLY where it left off when the detour finishes.

// ── SpritePathMove: where a node travels ────────────────────────────────────────────────────────────

// A declarative movement spec. It is resolved to a Curve when its node is entered — origin is only known
// then, so it defaults to wherever the chain hands off (the path's start, or the previous node's end).
// Named constructors are the authoring surface; aggregate initialization stays available and
// interchangeable. Identity is the kind, the first member.
struct SpritePathMove {
    enum class Kind : std::uint8_t { Line, ThroughPoints, Hermite, Curve };
    Kind                kind = Kind::Line;              // identity, first member
    std::optional<Vec2> origin{};             // absent → the inherited origin (start, or a previous node's end)
    Vec2                destination{};         // Line / Hermite: the point traveled to
    std::vector<Vec2>   points;               // ThroughPoints: traveled through in order, ending at back()
    Vec2                originTangent{};        // Hermite: the departure directional vector
    Vec2                destinationTangent{};   // Hermite: the arrival directional vector
    retropp::Curve      curve{};              // Curve: travel this exact curve — origin defaulting does NOT
                                              //   apply; it starts where the curve starts (an authored jump)

    [[nodiscard]] bool operator==(const SpritePathMove&) const = default;

    static SpritePathMove to(Vec2 destination);                          // Line from the inherited origin
    static SpritePathMove to(Vec2 origin, Vec2 destination);             // Line from an explicit origin
    static SpritePathMove through(std::vector<Vec2> points);             // ThroughPoints from the origin
    static SpritePathMove through(Vec2 origin, std::vector<Vec2> points);
    static SpritePathMove hermite(Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove hermite(Vec2 origin, Vec2 destination, Vec2 originTangent, Vec2 destinationTangent);
    static SpritePathMove onCurve(retropp::Curve c);                     // travel a pre-authored curve as-is
};

// How the movement facing drives the sprite's orientation. Applied during compose; the raw facing() stays
// exposed for a game to override afterward.
enum class FacingPolicy : std::uint8_t {
    None,            // facing does not touch the sprite (default)
    FlipX,           // flipX true while traveling toward -x (from the horizontal tangent sign; holds its
                     //   previous value while the horizontal component is 0, so vertical travel does not
                     //   flip-flop a mirrored sprite)
    RotateToFacing,  // rotation from the tangent angle (atan2), summed with the rotation track; art is
                     //   assumed authored facing +x
};

// One node: a movement spec + pacing + the concurrent tracks. PURE DATA — the SpritePath cursor owns all
// runtime state and resolves each track via the pure resolvers (sampleTween / sampleAnimation) against a NODE-LOCAL
// clock (ticks since the node was entered), so a node is a self-contained scene that replays whole on every
// entry. A tween track is held BY VALUE (a node is self-contained, copyable, with no lifetime coupling); the
// Animation is BY POINTER (a shared asset the game owns, valid for the cursor's lifetime), as is the
// DistanceTween a pacing may carry.
//
// Two idioms fall out of the pacing kinds, both kept deliberately:
//   • The WAIT node  — a zero-length move (Move::to the same point) with Eased pacing: the node stays entered
//                      for the eased duration while the sprite stands still and its tracks play.
//   • The SENTINEL node — Speed 0 (the parked default) on nonzero geometry never finishes: the sequence rests
//                      there indefinitely, tracks playing, until an interrupt or a re-path moves it on.
struct SpritePathNode {
    std::string_view            label{};      // optional symbolic id (empty = unnamed); identity, first member
    SpritePathMove              move{};       // where it travels
    PathPacing                  pacing{};     // how fast (constant speed / eased / a distance tween; default parked)
    FacingPolicy                facing = FacingPolicy::None;
    std::optional<Tween<float>> rotationDegrees{};              // rotation track (absent = no track)
    PlaybackMode                rotationMode = PlaybackMode::single();
    std::optional<Tween<Vec2>>  scale{};                        // scale track (absent = no track)
    PlaybackMode                scaleMode = PlaybackMode::single();
    const Animation*            animation = nullptr;            // frames track (game-owned; nullptr = none)
    PlaybackMode                animationMode = PlaybackMode::loopIndefinitely();
    std::optional<Vec2>         pivot{};      // sprite-local pixels for rotation/scale; absent = the sprite's
                                              //   center at the write
};

// ── The composed sample ────────────────────────────────────────────────────────────────────────────

// The composed per-tick result, as RAW VALUES — no pre-composed Transform, because the rotation/scale pivot
// defaults to the sprite's center and the sprite's size is only known at the write (the frame track may
// change it). applyTo() composes for the common case; a consumer reading these composes its own.
struct SpritePathSample {
    Vec2                  position{};              // float — quantized at the write
    Vec2                  facing{};                // the movement facing (holds the last real heading across dead spots)
    float                 rotationDegrees = 0.0f;  // the rotation track + RotateToFacing, summed
    Vec2                  scale{1.0f, 1.0f};       // the scale track (identity when absent)
    bool                  flipX = false;           // meaningful under FacingPolicy::FlipX
    const AnimationFrame* frame = nullptr;         // the current frame (nullptr = no animation track)
    float                 distance = 0.0f;         // the movement arc-length resolved
    bool                  finished = false;        // MOVEMENT finished — of the active content's SEQUENCE under
                                                   //   its mode (the tracks never gate completion)

    [[nodiscard]] bool operator==(const SpritePathSample&) const noexcept = default;
};

// ── The baked pass (private-by-convention runtime) ──────────────────────────────────────────────────

// One node of the active content, baked: its movement geometry, its one-pass duration in whole ticks, and
// the position + facing its movement ends at (which the next node in the chain departs from). The pass is
// baked once when the active content is (re)established — the durations and end positions are CLOCK-
// INDEPENDENT, so every wrap of the sequence replays the identical geometry, which is exactly what makes
// advance() and seek() resolve the same landing from the same elapsed-tick count. duration == UINT64_MAX
// marks a SENTINEL node (never finishes: Speed 0 on nonzero geometry, or a null distance-tween).
struct BakedPathNode {
    ArcLengthTable arc{};                                 // baked movement geometry for this node
    std::uint64_t  duration = 0;                          // one-pass ticks; UINT64_MAX = never finishes
    Vec2           endPosition{};                         // where the movement ends (next node's inherited origin)
    Vec2           endFacing{};                           // heading at the end (carries hold-last across the chain)
};

// How a popped interrupt hands control back to the content beneath it. Movement is relative — a node's shape
// is authored against its origin — so the natural hand-back carries on from where the sprite now stands.
enum class ResumePolicy : std::uint8_t {
    Continue,  // carry on from the sprite's current position: the resumed content's geometry shifts by the
               //   interrupt's net displacement, so the route drifts on from where the detour ended (default)
    Return,    // restore the exact position held when the interrupt was pushed (snap back to it)
};

// ── The interrupt frame (private-by-convention runtime) ─────────────────────────────────────────────

// One pushed interrupt: the content it plays + its mode + resume policy + where it departs from, PLUS the
// whole runtime it suspended (the exact state of whatever was playing when this was pushed). The suspended
// pass is stored baked, so resume recomputes nothing; Continue then shifts it by the net displacement.
struct SpritePathInterrupt {
    std::vector<SpritePathNode> nodes;                            // the interrupting content
    PlaybackMode                mode = PlaybackMode::single();     // captured at interrupt()
    ResumePolicy                resume = ResumePolicy::Continue;   // how it hands control back on pop
    Vec2                        chainStart{};                     // this interrupt's departure point (sprite pos at push)

    // the suspended runtime (of whatever was playing when this was pushed) — restored verbatim on pop:
    std::uint64_t              suspendedElapsedTicks = 0;
    std::vector<BakedPathNode> suspendedPass;                      // baked — resume re-bakes nothing
    Vec2                       suspendedChainStart{};
    std::size_t                suspendedNodeIndex = 0;
    SpritePathSample           suspendedSample{};
};

// ── The game-owned cursor ────────────────────────────────────────────────────────────────────────────

// The "just play it" cursor over a node SEQUENCE. STATE LIVES HERE, IN THE GAME'S OBJECT — not in the
// platform. The same shape as TweenPlayer / AnimationPlayer / PathWalker: elapsed-tick bookkeeping + play /
// pause / seek over the composed resolve. The game constructs it, calls advance() each tick, and either
// reads the getters or calls applyTo(sprite). The base sequence bakes to a per-node pass lazily on the first
// advance() (and re-bakes on stop() / restart()), because designated initialization cannot run a bake and
// the chain start is only known then. Re-path a mover by assigning a new `nodes` and calling restart().
struct SpritePath {
    // The cadence a bare-constructed path resolves pacing and track durations against. EngineConfig::setActive
    // fans the configured cadence into it at startup (alongside the other players' defaultTiming), or assign
    // it directly, or override one path via .profile. A single process-wide default — legitimate here: the
    // platform is single-threaded, and this is a config default, not retained state.
    static inline TimingProfile defaultTiming = TimingProfile::GameBoyColor;

    std::vector<SpritePathNode> nodes;             // the base sequence this path plays
    Vec2                        start{};           // where the chain begins when nodes[0]'s move names no origin
    TimingProfile               profile = defaultTiming;
    std::uint64_t               elapsedTicks = 0;  // the ACTIVE content's clock (base, or the top interrupt)
    bool                        playing = true;
    SpritePathSample            sample{};          // cached by advance() so the getters need no arguments

    // Private-by-convention runtime. `pass`, `chainStart`, `nodeIndex`, and `elapsedTicks` always describe
    // the ACTIVE content (the top of the interrupt stack, or the base when the stack is empty).
    std::vector<SpritePathInterrupt> interrupts;   // the interrupt stack (deepest first, top = back())
    std::vector<BakedPathNode>       pass;         // the active content's baked pass
    Vec2                             chainStart{}; // the active content's chain start
    std::size_t                      nodeIndex = 0;// the active content's current node (cached by the resolve)
    bool                             baked = false;// whether the base pass has been built at least once

    // Each game tick: bake the base if needed, accrue elapsedTicks (ONLY while playing), re-resolve the
    // composed sample, and auto-pop any interrupt that finished (its leftover ticks flowing into the resumed
    // content). `mode` is the BASE SEQUENCE's PlaybackMode over `nodes` (an active interrupt plays under its
    // own captured mode and does not consult this); it defaults to loopIndefinitely so a bare advance() loops
    // the route, like the other players.
    void advance(PlaybackMode mode = PlaybackMode::loopIndefinitely(), std::uint64_t deltaTicks = 1);

    // Write the composed sample into `s`: position → x/y (quantized); the frame's art fields when the CURRENT
    // node has an animation track (a node without one leaves the last art showing); a scale-then-rotation
    // transform when ANY node the path currently holds declares rotation / scale / RotateToFacing (identity
    // while the current node drives neither, so a tumble STOPS instead of freezing); flipX when ANY node uses
    // FacingPolicy::FlipX. key, alpha, flipY, and the 90° texture rotation are left untouched — the game's.
    void applyTo(Sprite& s) const;

    [[nodiscard]] Vec2                  position()        const noexcept { return sample.position; }
    [[nodiscard]] Vec2                  facing()          const noexcept { return sample.facing; }
    [[nodiscard]] float                 rotationDegrees() const noexcept { return sample.rotationDegrees; }
    [[nodiscard]] Vec2                  scaleValue()      const noexcept { return sample.scale; }
    [[nodiscard]] bool                  flipX()           const noexcept { return sample.flipX; }
    [[nodiscard]] const AnimationFrame* frame()           const noexcept { return sample.frame; }
    [[nodiscard]] float                 distance()        const noexcept { return sample.distance; }
    [[nodiscard]] bool                  finished()        const noexcept { return sample.finished; }

    // The active content's current node — game logic keyed to a route leg reads its label here.
    // currentNode() is nullptr when the active content is empty.
    [[nodiscard]] std::size_t           currentNodeIndex() const noexcept { return nodeIndex; }
    [[nodiscard]] const SpritePathNode* currentNode()      const noexcept;

    // ── The interrupt stack ──────────────────────────────────────────────────────────────────────────
    // interrupt() suspends the whole current playback and starts `newNodes` at tick 0, its first node
    // departing from the sprite's current position; the detour plays under its own `mode` (default single()).
    // It auto-pops when its sequence finishes; popInterrupt() ends the current interrupt now (and is the only
    // exit from a LoopIndefinitely interrupt). Depth > 1 nests: pops cascade. `resume` decides the hand-back:
    // Continue (default) carries the route on from where the detour ended (it drifts); Return snaps the sprite
    // back to where it stood when the interrupt was pushed.
    void interrupt(std::vector<SpritePathNode> newNodes, PlaybackMode mode = PlaybackMode::single(),
                   ResumePolicy resume = ResumePolicy::Continue);
    void popInterrupt();                                      // end the current interrupt now; resume
    [[nodiscard]] bool        interrupted()    const noexcept { return !interrupts.empty(); }
    [[nodiscard]] std::size_t interruptDepth() const noexcept { return interrupts.size(); }

    void play();                             // resume
    void pause();                            // freeze at the current sample
    void stop();                             // pause + rewind to base node 0 (clears the stack, re-bakes)
    void restart();                          // rewind + play (clears the stack, re-bakes — the re-path entry point)
    void seek(std::chrono::nanoseconds at);  // jump the ACTIVE content's clock (stack untouched); keeps playing/paused
};

}  // namespace retropp
