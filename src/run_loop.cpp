#include "retropp/run_loop.h"

#include "retropp/frame_timing.h"

namespace retropp {

void RunLoop::advance() {
    if (exitResolved_) return;  // a guard already resolved the exit to Proceed — pull no further ticks

    const auto t = clock_.now();

    if (!started_) {            // lazy baseline on the first call — nothing to advance yet
        started_ = true;
        last_ = t;
        publishFrameTiming(FrameTiming{.alpha = 0.0f, .tickAdvanced = false});  // no tick yet → renderer composites verbatim
        if (render_) render_();
        resolveExitAtBoundary();  // an exit requested before the first frame resolves here, not a frame late
        return;
    }

    auto frame = t - last_;
    last_ = t;
    if (frame < std::chrono::nanoseconds::zero()) {
        frame = std::chrono::nanoseconds::zero();  // monotonic guard (backward clock)
    }
    if (frame > kMaxFrameTime) {
        frame = kMaxFrameTime;                     // spiral-of-death clamp
    }

    accumulator_ += frame;
    int ticksThisFrame = 0;                        // how many sim ticks committed this iteration
    while (accumulator_ >= tickPeriod_) {          // fixed-step catch-up (profile's period)
        // Sample once per tick: the latest level as held, the union of levels seen since the last
        // tick as the press source (so a sub-tick tap isn't dropped), and the accumulated analog —
        // per player slot. Then reset the per-tick accumulators to the current level / cleared
        // relatives so the next window starts fresh and a held action doesn't re-fire its press edge.
        InputSample tickSample = latest_;
        for (int i = 0; i < kMaxPlayers; ++i) {
            tickSample.players[static_cast<std::size_t>(i)].analog =
                pendingAnalog_[static_cast<std::size_t>(i)];
        }
        input_.sampleTick(tickSample, heldUnion_);
        if (tick_) tick_(input_);
        ++tickCount_;
        accumulator_ -= tickPeriod_;
        for (int i = 0; i < kMaxPlayers; ++i) {
            heldUnion_[static_cast<std::size_t>(i)] =
                latest_.players[static_cast<std::size_t>(i)].held;
            pendingAnalog_[static_cast<std::size_t>(i)].clearRelatives();
        }
        ++ticksThisFrame;
    }
    if (ticksThisFrame > 0) commitSpan_ = ticksThisFrame;

    // The sub-tick fraction, then that fraction mapped across the interval the renderer's mirror
    // holds. A commit of N ticks leaves the mirror spanning N fixed steps (prev at tick T - N, cur at
    // T) while the fraction alone describes one step, so the factor is where sim time T + raw - 1
    // falls along that interval: (N - 1 + raw) / N. At N == 1 it reduces to exactly raw, the steady
    // state. The result stays in [0, 1) for any span, and a long catch-up lands near cur, which is
    // where a large jump belongs.
    const float raw = static_cast<float>(accumulator_.count())
                    / static_cast<float>(tickPeriod_.count());  // [0, 1)
    const float alpha = (static_cast<float>(commitSpan_ - 1) + raw)
                      / static_cast<float>(commitSpan_);
    // Publish the blend factor + the tick signal for the renderer's per-id interpolation before handing
    // off to the render callback (which reaches the renderer one call away, sharing no reference). The
    // fraction and the span go with them: a layer that declares how often its world advances eases across
    // that many ticks instead of across the interval alpha describes, and composes its own factor.
    publishFrameTiming(FrameTiming{.alpha        = alpha,
                                   .tickAdvanced = ticksThisFrame > 0,
                                   .subTick      = raw,
                                   .commitSpan   = static_cast<std::uint32_t>(commitSpan_)});
    if (render_) render_();

    resolveExitAtBoundary();
}

void RunLoop::resolveExitAtBoundary() {
    // Resolve a pending exit at the frame boundary — once per advance() while pending, never mid-tick.
    // The completed tick batch means sim state is settled when the guard reads it (a resume snapshot /
    // save). No guard registered → Proceed immediately.
    if (!exitPending_) return;
    const ExitVerdict verdict = exitGuard_ ? exitGuard_() : ExitVerdict::Proceed;
    switch (verdict) {
        case ExitVerdict::Proceed:
            exitPending_  = false;
            exitResolved_ = true;   // terminal — advance() early-returns hereafter
            running_      = false;  // ends RunLoop::run()
            break;
        case ExitVerdict::NotYet:
            break;                  // keep pending; the sim keeps advancing, ask again next boundary
        case ExitVerdict::Veto:
            exitPending_  = false;  // abandon the exit; the host clears any OS quit latch
            break;
    }
}

void RunLoop::run() {
    running_ = true;
    while (running_) {
        advance();
    }
}

}  // namespace retropp
