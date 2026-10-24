#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace retropp {

// ── Emission atlas planning — where each field's blurred image lives ─────────────────────────────────
//
// A field is a blurred image sized to its consumer's footprint rather than to the viewport, and many
// fields share one atlas texture. The planner decides where each one goes, such that a field's blur can
// never read a neighbor's content.
//
// That isolation is per-rect, not pairwise. A field's rect is its consumer's quad grown by a margin of
// m = ceil(reach) + 2 on every side, and a field is only ever READ inside that quad — the inner box — or
// one texel past it. A texel of the inner box sits at least m from its own rect's edge while a blur tap
// travels at most reach <= m - 2, so no tap leaves the rect it started in, in either pass, whatever is
// packed next door. The same holds one texel further out: a texel of the first ring sits at least m - 1
// from the edge, still more than a tap travels, so its blurred value is composed of this field's content
// alone. That ring is what the read needs: a filtered sample taken at the inner box's edge draws support from
// it, so a ring carrying this field's light and no neighbor's is what keeps the quad's edge free of a seam.
// Beyond the first ring nothing is guaranteed and nothing reads. Rects therefore pack adjacently, which is
// what makes many small fields at a wide reach affordable.
//
// Fields group into pages by reach. Every field on a page blurs with the same kernel, so one horizontal
// and one vertical pass serve the whole page however many rects sit on it — passes track the number of
// distinct reaches authored, never the field count.

// What one field needs: where its consumer sits in viewport pixels, and how far its light spreads.
struct EmissionDemand {
    int   x = 0, y = 0, w = 0, h = 0;  // the consumer's quad, viewport px, before reach inflation
    float reach = 0.0f;                // viewport px
};

// Where the planner put it. `page` indexes EmissionAtlasPlan::pages; -1 means it could not be placed.
struct EmissionPlacement {
    int rectX = 0, rectY = 0;  // atlas texel of the inflated rect's top-left
    int rectW = 0, rectH = 0;  // the inflated rect's size
    int page  = -1;
};

// One page: the fields sharing a reach, and the band of the atlas the blur pass scissors to.
struct EmissionPage {
    float reach = 0.0f;
    int   x = 0, y = 0, w = 0, h = 0;
};

struct EmissionAtlasPlan {
    int                            width = 0, height = 0;  // 0 x 0 when nothing is placed
    std::vector<EmissionPage>      pages;
    std::vector<EmissionPlacement> placements;  // parallel to the input demands, same order
    int                            dropped = 0;
};

namespace detail {

// The smallest power of two >= v, and the largest <= v. Atlas dimensions are powers of two, so the
// usable ceiling for a `maxSize` is the largest power of two that fits inside it.
[[nodiscard]] inline int pow2AtLeast(int v) noexcept {
    if (v <= 0) return 0;
    int p = 1;
    while (p < v && p < (1 << 29)) p <<= 1;
    return p;
}

[[nodiscard]] inline int pow2AtMost(int v) noexcept {
    if (v < 1) return 0;
    int p = 1;
    while ((p << 1) <= v && p < (1 << 29)) p <<= 1;
    return p;
}

}  // namespace detail

// The margin protecting a field's content from everything packed around it. One texel covers the blur's
// own rounding, the second keeps the first ring outside the inner box tap-clean so a bilinear read at the
// quad's edge draws clean support. A reach of zero or less still earns the full two-texel skirt.
[[nodiscard]] inline int emissionMargin(float reach) noexcept {
    const float spread = reach > 0.0f ? reach : 0.0f;
    return static_cast<int>(std::ceil(spread)) + 2;
}

// Plan an atlas for `demands`, with neither dimension exceeding `maxSize`.
//
// A demand is dropped — its placement left at page -1, counted in `dropped` — when it has no area, when
// its inflated rect exceeds the atlas ceiling, or when the atlas is full at its largest usable size.
// Dropping is always counted; the caller reports it.
[[nodiscard]] inline EmissionAtlasPlan planEmissionAtlas(std::span<const EmissionDemand> demands,
                                                         int                             maxSize) {
    EmissionAtlasPlan plan;
    plan.placements.assign(demands.size(), EmissionPlacement{});
    if (demands.empty()) return plan;

    const int ceiling = detail::pow2AtMost(maxSize);

    // A demand earns a rect when it has area and its inflated rect fits the ceiling.
    struct Candidate {
        std::size_t index;
        int         rectW, rectH;
        float       reach;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(demands.size());
    int unplaceable = 0;
    int widest      = 0;
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const EmissionDemand& d = demands[i];
        if (d.w <= 0 || d.h <= 0) {
            ++unplaceable;
            continue;
        }
        const int margin = emissionMargin(d.reach);
        const int rectW  = d.w + 2 * margin;
        const int rectH  = d.h + 2 * margin;
        if (rectW > ceiling || rectH > ceiling) {
            ++unplaceable;
            continue;
        }
        candidates.push_back(Candidate{.index = i, .rectW = rectW, .rectH = rectH, .reach = d.reach});
        widest = std::max(widest, rectW);
    }
    if (candidates.empty()) {
        plan.dropped = unplaceable;
        return plan;
    }

    // Pages in first-appearance order, reaches compared exactly: two demands authored identically
    // resolve through identical arithmetic and share a page, two authored differently keep their own.
    std::vector<float> reaches;
    std::vector<int>   pageOf(candidates.size(), 0);
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        int page = -1;
        for (std::size_t p = 0; p < reaches.size(); ++p) {
            if (reaches[p] == candidates[c].reach) {
                page = static_cast<int>(p);
                break;
            }
        }
        if (page < 0) {
            page = static_cast<int>(reaches.size());
            reaches.push_back(candidates[c].reach);
        }
        pageOf[c] = page;
    }

    // Shelf-pack every page into a band spanning `width`, bands stacking top to bottom. Returns whether
    // every candidate found a home; `out` carries what was placed either way.
    const auto attempt = [&](int width, int heightCap, EmissionAtlasPlan& out, int& overflow) {
        out.pages.clear();
        out.placements.assign(demands.size(), EmissionPlacement{});
        overflow      = 0;
        bool complete = true;
        int  y        = 0;
        for (int page = 0; page < static_cast<int>(reaches.size()); ++page) {
            // Tallest first, ties keeping input order, so a plan is identical for identical input.
            std::vector<std::size_t> members;
            for (std::size_t c = 0; c < candidates.size(); ++c)
                if (pageOf[c] == page) members.push_back(c);
            std::stable_sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
                return candidates[a].rectH > candidates[b].rectH;
            });

            // The index this band will occupy once it places anything.
            const int outPage = static_cast<int>(out.pages.size());
            const int bandTop = y;
            int       shelfX = 0, shelfY = y, shelfH = 0;
            bool      placedAny = false;
            for (std::size_t c : members) {
                const Candidate& cand = candidates[c];
                if (shelfX > 0 && shelfX + cand.rectW > width) {
                    shelfY += shelfH;
                    shelfX = 0;
                    shelfH = 0;
                }
                if (cand.rectW > width || shelfY + cand.rectH > heightCap) {
                    ++overflow;
                    complete = false;
                    continue;
                }
                out.placements[cand.index] = EmissionPlacement{.rectX = shelfX,
                                                               .rectY = shelfY,
                                                               .rectW = cand.rectW,
                                                               .rectH = cand.rectH,
                                                               .page  = outPage};
                shelfX += cand.rectW;
                shelfH    = std::max(shelfH, cand.rectH);
                placedAny = true;
            }
            const int bandBottom = shelfY + shelfH;
            if (placedAny)
                out.pages.push_back(EmissionPage{.reach = reaches[page],
                                                 .x     = 0,
                                                 .y     = bandTop,
                                                 .w     = width,
                                                 .h     = bandBottom - bandTop});
            y = bandBottom;
        }
        if (out.pages.empty()) {
            out.width  = 0;
            out.height = 0;
        } else {
            out.width  = width;
            out.height = std::min(detail::pow2AtLeast(y), heightCap);
        }
        return complete;
    };

    // Widen until everything fits. The floor holds the widest rect and is never below 256; a wider atlas
    // shortens the stack, so the first width that holds every rect is the one with the least area.
    int start = std::max(256, detail::pow2AtLeast(widest));
    if (start > ceiling) start = ceiling;

    EmissionAtlasPlan packed;
    int               overflow = 0;
    for (int width = start;; width <<= 1) {
        const bool complete = attempt(width, ceiling, packed, overflow);
        if (complete || width >= ceiling) break;
    }

    packed.dropped = unplaceable + overflow;
    return packed;
}

}  // namespace retropp
