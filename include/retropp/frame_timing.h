#pragma once

#include <cstdint>

namespace retropp {

// The render-timing the run loop hands the renderer each iteration: the blend factor and whether a
// simulation tick committed this iteration. The renderer interpolates each object between its previous and
// current mirror state by `alpha`, and rotates that per-object history on `tickAdvanced`.
struct FrameTiming {
    // Where to render along the interval the mirror holds, in [0, 1). One iteration can commit several
    // ticks, which leaves the mirror spanning that many fixed steps; alpha is the sub-tick fraction
    // mapped across the whole span, so motion reads at a constant rate however the ticks bunched. An
    // iteration that commits one tick — the steady state — publishes the bare fraction.
    float alpha        = 0.0f;
    bool  tickAdvanced = false;  // at least one simulation tick committed this loop iteration

    // The two quantities `alpha` is composed from, published beside it. A reader that eases across a
    // span of its own — a layer whose world advances every few ticks — composes its own factor from
    // these; recovering them by dividing `alpha` back out would not be exact.
    float         subTick    = 0.0f;  // fraction of the current tick period elapsed, in [0, 1)
    std::uint32_t commitSpan = 1;     // ticks the most recent commit ran; retained across 0-tick iterations
};

// The channel the run loop uses to reach the renderer without sharing a reference. The loop computes the
// blend factor and the tick signal but holds no renderer pointer (the game's render callback is the only
// link, and it stays renderFrame(frame) — no interpolation argument). The loop publishes here right before
// invoking the render callback; the renderer reads here at the top of renderFrame. SDL-free, so the run
// loop stays SDL-free.
//
// Scoped to the calling thread (the single-threaded loop publishes and renders on the same thread). A
// thread whose loop has not published yet reads the default (alpha 0, tickAdvanced false) — which the
// renderer composites as the submission verbatim, the correct first-frame behavior.
void                      publishFrameTiming(FrameTiming timing) noexcept;
[[nodiscard]] FrameTiming frameTiming() noexcept;

}  // namespace retropp
