#pragma once

#include <cstdint>
#include <vector>

#include "retropp/draw_state.h"  // Sprite, Point, ShapePoints, Space, Rotation
#include "retropp/geometry.h"    // IntRect, AssetDimensions
#include "retropp/image.h"       // LoadedImage, TransparentIndices, ShapeTrace
#include "retropp/transform.h"   // Transform (the quad<->layer homography)

// The sprite MASK family — a sprite's image as a MASK, decoupled from the texture that defined it. A
// Sprite always carries a texture and draws it; the mask is the coverage alone, usable anywhere a shape
// is usable — test points against it (pixel-exact collision), or take it as geometry to drive a
// textureless Region in the shape of the sprite. Three forms, in whichever coordinate Space you ask for:
//   sprite.mask(space)          → SpriteMask       — a BORROW: exact, live, non-owning (frame life)
//   sprite.freezeMask(space)    → FrozenSpriteMask — OWNED: an exact snapshot, detached, storable
//   sprite.maskShape(n, space)  → ShapePoints      — OWNED: the mask as GEOMETRY, a coarse ≤ n-point
//                                                    polygon (a real shape — Region / stencil() / physics)
// Every form reads the sprite's CURRENT tile — re-query after a frame change. The coverage is the
// sprite's own sheet, resolved from `atlas` against the uploaded pixels. The point/orientation/placement
// math is CPU-only, tick-state (the same discipline the anchors follow).
//
// The cost model — the intuition is inverted: exactness is expensive to DRAW, not to test.
//   - The exact forms answer contains(point) with ONE coverage read, O(1) — no polygon. A ShapePoints
//     test is O(vertices). The coarse polygon is not the fast mask; it is the differently-USABLE one.
//   - A DRAWN polygon's vertex count is a per-pixel cost while it is on screen (the region gate
//     evaluates the polygon SDF per fragment; it carries at most 64 vertices — longer polygons truncate
//     at pack time with a logged warning).
//   - freezeMask() copies the coverage mask on every call — per-frame-per-object use is allocation
//     churn plus retained memory. maskShape() traces the coverage on every call — cache the result.
// To re-draw the sprite's ART, the mask is the wrong tool: a second Sprite with the same atlas / tile /
// transform is one instanced quad (a whole-silhouette ColorFill flat-colors it). That covers re-drawing
// the art and nothing else — it cannot give a textureless region, a detached shape, or an evenly
// inflated outline (ShapePoints::radius can).

namespace retropp {

// ── ArtMask — the binary visibility of one sprite cell, in ART space ─────────────────────────────
// One bit per art pixel of a sprite's cell: 1 = visible (the palette index is not a structural hole). It
// is the CPU coverage the trace + freezeMask read — traced to a polygon (traceSilhouette) or snapshotted
// (freezeMask). Build one from a decoded LoadedImage + a cell rect (the headless route, no renderer needed).
struct ArtMask {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> visible;  // 1 = visible, row-major (width * height)

    [[nodiscard]] bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        return visible[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(x)] != 0;
    }
    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
};

// The pixel rect of a sprite's cell within its sheet — the inverse of the atlas slicer's tile math: `tile`
// is the top-left 8px cell index, so its column/row on the sheet's cell grid give the pixel origin, and
// `size` gives the extent. A degenerate sheet width yields an empty rect.
[[nodiscard]] IntRect spriteCellRect(int sheetWidth, std::uint16_t tile, AssetDimensions size) noexcept;

// Build the ArtMask for a cell of a decoded image (the headless route): a pixel is visible when its index
// is not a structural hole. A cell that falls outside the image masks the in-bounds part and treats the
// rest as not visible.
[[nodiscard]] ArtMask artMask(const LoadedImage& img, IntRect cell, TransparentIndices transparent);

// Trace a mask's OUTER silhouette to a polygon of at most `maxPoints` vertices, in ART pixels. The boundary
// is the marching-squares outer contour (interior holes are bridged — outer boundary only; disconnected
// blobs merge through their common convex hull). Simplification honors `trace`: Conservative keeps the
// silhouette CONTAINED at every budget (only ever adding area, degenerating toward the hull then the box);
// Balanced hugs it minimax-tight. Returns {} for an empty / fully-transparent mask. Throws
// std::invalid_argument when maxPoints < 3.
[[nodiscard]] std::vector<Point> traceSilhouette(const ArtMask& mask, int maxPoints,
                                                 ShapeTrace trace = ShapeTrace::Conservative);

// ── The exact mask forms — coverage-queryable, GPU-free ──────────────────────────────────────────
//
// An exact mask test maps a Quad-space or Layer-space point back to an art pixel and reads coverage.
// The map is captured as one homography: the whole (x, y) + (pivot − origin) + transform · (p − pivot)
// chain a sprite's toLayer() applies composes into a single quad→layer Transform, so Layer queries invert
// it and Quad queries skip it. A projection behind the camera plane gets no special guard — the same
// exposure Sprite::anchor(k, Space::Layer) documents.

// The forward quad→layer homography for a sprite (M with sprite.toLayer(p) == M · p). freezeMask()
// captures it so the snapshot is self-contained; the borrow computes it live.
[[nodiscard]] Transform spriteQuadToLayer(const Sprite& sprite) noexcept;

// A sprite's mask as a BORROW — a non-owning view over the sprite, valid only while it outlives the
// view (immediate-mode / tick lifetime, like a span). It reads the sprite's live coverage on demand (from
// the sprite's uploaded sheet), so it tracks the sprite's flip / rotation / transform / placement for free.
// To keep a mask past the frame, freezeMask() it. The Space is baked in at construction (mask(space));
// every answer is in that space.
struct SpriteMask {
    const Sprite* sprite = nullptr;   // borrowed — must outlive this view
    Space         space  = Space::Quad;

    // Is `p` (in this mask's Space) on a visible pixel? One exact coverage read: the point maps back to
    // an art pixel and the pixel's visibility is the answer. Outside the art (or a fully-transparent pixel)
    // is false. O(1) — no polygon.
    [[nodiscard]] bool contains(Point p) const;

    // A conservative axis-aligned bounds of the mask, in this mask's Space (Quad: exact art-pixel
    // extent under orientation; Layer: the placed extent, outward-rounded to whole pixels). Empty rect for
    // a fully-transparent tile. A cheap broad-phase before contains().
    [[nodiscard]] IntRect bounds() const;
};

// A sprite's mask as an OWNED snapshot — it copies the coverage mask and the sprite's placement at
// freezeMask() time, so it answers contains() / bounds() long after the sprite (and its sheet) are gone.
// The owning peer of SpriteMask; mint it when a mask must be stored (a trail, a history buffer, a
// collider cached across frames). Detached: it never reads the sprite again.
struct FrozenSpriteMask {
    ArtMask   mask;                        // owned coverage (cell-local art space)
    Rotation  rotation = Rotation::None;   // the orientation ops the mask is read under
    bool      flipX = false;
    bool      flipY = false;
    Space     space  = Space::Quad;
    Transform quadToLayer{};               // the captured placement (identity when frozen in Quad space)

    [[nodiscard]] bool    contains(Point p) const;
    [[nodiscard]] IntRect bounds() const;
};

}  // namespace retropp
