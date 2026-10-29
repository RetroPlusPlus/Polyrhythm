#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "retropp/emission_atlas.h"  // detail::pow2AtLeast

// Where a sheet lands in the atlas store, and how large the store is.
//
// Every sheet a game uploads lives in one store texture, and the store lays them out on shelves across its
// width: a sheet takes the room a released sheet left when it fits, else goes on the first shelf tall
// enough with room left, else opens a new shelf below the last. A sheet that fits nowhere is answered by a
// repack of every live sheet from empty, tallest first, which reclaims whatever release left fragmented;
// one that still does not fit grows the store. Engine-internal, and pure: a wrong placement writes correct
// pixels to the wrong texels without failing, so the arithmetic is decided in one place and exercised on
// its own.

namespace retropp::detail {

// The most a side of the store texture is, on every backend the engine runs on.
inline constexpr int kAtlasStoreSide = 16384;

// A sheet's size, in texels.
struct AtlasSheetSize {
    int width  = 0;
    int height = 0;
};

// Where a sheet's top-left texel sits in the store.
struct AtlasPlacement {
    int x = 0;
    int y = 0;
    friend bool operator==(const AtlasPlacement&, const AtlasPlacement&) noexcept = default;
};

// A store's size, in texels.
struct AtlasStoreSize {
    int width  = 0;
    int height = 0;
    friend bool operator==(const AtlasStoreSize&, const AtlasStoreSize&) noexcept = default;
};

// The open shelves of a store, and the room released sheets left in them. Each shelf is a band of the
// store's full width, as tall as the first sheet that opened it, filled from the left. place() takes a
// released sheet's room first, then the shelves; packAtlasSheets lays every live sheet out afresh when
// neither has room.
class AtlasShelves {
public:
    AtlasShelves() = default;
    AtlasShelves(int width, int height) : width_(width), height_(height) {}

    // Where the sheet lands: the first released room that holds it (what is left of that room to its right
    // and below stays free), else the first shelf tall enough with room for it, else a new shelf below the
    // last that the store's height holds; nothing when none exists, or when the sheet has no area.
    [[nodiscard]] std::optional<AtlasPlacement> place(int width, int height) {
        if (width <= 0 || height <= 0 || width > width_) return std::nullopt;
        for (std::size_t i = 0; i < holes_.size(); ++i) {
            const Hole h = holes_[i];
            if (h.width < width || h.height < height) continue;
            holes_.erase(holes_.begin() + static_cast<std::ptrdiff_t>(i));
            if (h.width > width)
                holes_.push_back(Hole{.x = h.x + width, .y = h.y, .width = h.width - width, .height = height});
            if (h.height > height)
                holes_.push_back(Hole{.x = h.x, .y = h.y + height, .width = h.width, .height = h.height - height});
            return AtlasPlacement{.x = h.x, .y = h.y};
        }
        for (Shelf& s : shelves_) {
            if (s.height >= height && s.nextX + width <= width_) {
                const AtlasPlacement at{.x = s.nextX, .y = s.y};
                s.nextX += width;
                return at;
            }
        }
        const int top = shelves_.empty() ? 0 : shelves_.back().y + shelves_.back().height;
        if (top + height > height_) return std::nullopt;
        shelves_.push_back(Shelf{.y = top, .height = height, .nextX = width});
        return AtlasPlacement{.x = 0, .y = top};
    }

    // The room a sheet placed at `at` held, free for a later sheet that fits it.
    void release(AtlasPlacement at, int width, int height) {
        if (width <= 0 || height <= 0) return;
        holes_.push_back(Hole{.x = at.x, .y = at.y, .width = width, .height = height});
    }

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    struct Shelf {
        int y      = 0;
        int height = 0;
        int nextX  = 0;
    };
    // Room a released sheet held: free until a sheet that fits it is placed there.
    struct Hole {
        int x      = 0;
        int y      = 0;
        int width  = 0;
        int height = 0;
    };
    int                width_  = 0;
    int                height_ = 0;
    std::vector<Shelf> shelves_;
    std::vector<Hole>  holes_;
};

// Every sheet laid out from empty into a store of `size`, tallest first and input order on a tie, so the
// layout is the same for the same sheets. The placements are in input order; a sheet with no area is placed
// at the origin and takes no room. Nothing when a sheet does not fit, with no placement to show for it.
[[nodiscard]] inline std::optional<std::vector<AtlasPlacement>> packAtlasSheets(
    std::span<const AtlasSheetSize> sheets, AtlasStoreSize size) {
    std::vector<std::size_t> order;
    order.reserve(sheets.size());
    for (std::size_t i = 0; i < sheets.size(); ++i)
        if (sheets[i].width > 0 && sheets[i].height > 0) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return sheets[a].height > sheets[b].height;
    });
    AtlasShelves                shelves(size.width, size.height);
    std::vector<AtlasPlacement> placements(sheets.size());
    for (const std::size_t i : order) {
        const std::optional<AtlasPlacement> at = shelves.place(sheets[i].width, sheets[i].height);
        if (!at) return std::nullopt;
        placements[i] = *at;
    }
    return placements;
}

// The smallest power-of-two square that holds a first sheet, each side at most kAtlasStoreSide.
[[nodiscard]] inline AtlasStoreSize firstAtlasStore(int width, int height) noexcept {
    const int side = std::min(pow2AtLeast(std::max(width, height)), kAtlasStoreSide);
    return AtlasStoreSize{.width = side, .height = side};
}

// The size to try after `size` could not hold the sheets: the shorter side doubled, the width on a tie,
// neither past kAtlasStoreSide; nothing when both sides are there already.
[[nodiscard]] inline std::optional<AtlasStoreSize> grownAtlasStore(AtlasStoreSize size) noexcept {
    if (size.width >= kAtlasStoreSide && size.height >= kAtlasStoreSide) return std::nullopt;
    if (size.width <= size.height && size.width < kAtlasStoreSide)
        return AtlasStoreSize{.width = std::min(size.width * 2, kAtlasStoreSide), .height = size.height};
    return AtlasStoreSize{.width = size.width, .height = std::min(size.height * 2, kAtlasStoreSide)};
}

}  // namespace retropp::detail
