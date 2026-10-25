#include "retropp/sprite_mask.h"

#include "retropp/renderer.h"  // Renderer::instance() — the sprite mask family resolves atlas coverage here

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace retropp {

// ── Cell + mask construction ─────────────────────────────────────────────────────────────────────

IntRect spriteCellRect(int sheetWidth, std::uint16_t tile, AssetDimensions size) noexcept {
    const int cellsPerRow = sheetWidth / kAtlasCellPx;
    if (cellsPerRow <= 0) return IntRect{};
    const int col = static_cast<int>(tile) % cellsPerRow;
    const int row = static_cast<int>(tile) / cellsPerRow;
    return IntRect{col * kAtlasCellPx, row * kAtlasCellPx, size.width, size.height};
}

// The sprite's current cell as an ArtMask, sampled from the engine renderer's uploaded pixels for `atlas`.
// The renderer is the one a program constructs; a sprite whose sheet was never uploaded masks as empty.
static ArtMask spriteCellMask(const Sprite& s) {
    const Renderer& r    = Renderer::instance();
    const IntRect   cell = spriteCellRect(r.atlasPixelSize(s.atlas).width, s.tile, s.size);
    ArtMask m{s.size.width, s.size.height,
              std::vector<std::uint8_t>(static_cast<std::size_t>(s.size.width) *
                                        static_cast<std::size_t>(s.size.height), 0)};
    for (int y = 0; y < s.size.height; ++y) {
        for (int x = 0; x < s.size.width; ++x) {
            if (r.atlasVisibleAt(s.atlas, cell.x + x, cell.y + y)) {
                m.visible[static_cast<std::size_t>(y) * static_cast<std::size_t>(s.size.width) +
                          static_cast<std::size_t>(x)] = 1;
            }
        }
    }
    return m;
}

ArtMask artMask(const LoadedImage& img, IntRect cell, TransparentIndices transparent) {
    ArtMask m{cell.width, cell.height,
              std::vector<std::uint8_t>(static_cast<std::size_t>(std::max(0, cell.width)) *
                                        static_cast<std::size_t>(std::max(0, cell.height)), 0)};
    if (cell.width <= 0 || cell.height <= 0) return m;
    for (int y = 0; y < cell.height; ++y) {
        for (int x = 0; x < cell.width; ++x) {
            const int sx = cell.x + x;
            const int sy = cell.y + y;
            if (sx < 0 || sy < 0 || sx >= img.width || sy >= img.height) continue;
            const std::size_t si = static_cast<std::size_t>(sy) * static_cast<std::size_t>(img.width) +
                                   static_cast<std::size_t>(sx);
            if (!transparent.contains(static_cast<int>(img.indices[si]))) {
                m.visible[static_cast<std::size_t>(y) * static_cast<std::size_t>(cell.width) +
                          static_cast<std::size_t>(x)] = 1;
            }
        }
    }
    return m;
}

// ── The coverage-query maps ──────────────────────────────────────────────────────────────────────

Transform spriteQuadToLayer(const Sprite& sprite) noexcept {
    const float px = sprite.pivot.x - sprite.origin.x;
    const float py = sprite.pivot.y - sprite.origin.y;
    // toLayer(p) = (x, y) + (pivot − origin) + transform·(p − pivot) — the three steps composed into one
    // homography: translate to the pivot, warp, translate to the placed origin.
    return Transform::translation(-sprite.pivot.x, -sprite.pivot.y)
        .then(sprite.transform)
        .then(Transform::translation(static_cast<float>(sprite.x) + px,
                                     static_cast<float>(sprite.y) + py));
}

namespace {

// Invert orientPoint: map a QUAD point back to raw art coordinates (undo the flips, then the rotation).
// The exact inverse of draw_state.h's orientPoint, so a round trip is the identity for all eight
// flip/rotation combinations.
Point inverseOrientPoint(Point q, int width, int height, Rotation rot, bool flipX, bool flipY) noexcept {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    if (flipX) q.x = w - q.x;
    if (flipY) q.y = h - q.y;
    switch (rot) {
        case Rotation::Rot90:  return Point{q.y, w - q.x};
        case Rotation::Rot180: return Point{w - q.x, h - q.y};
        case Rotation::Rot270: return Point{h - q.y, q.x};
        case Rotation::None:   break;
    }
    return q;
}

// A query point (in `space`) → the raw art coordinate whose coverage answers contains(). For a Layer query
// the point first maps back to quad space through the inverted placement; a Quad query is already there.
Point queryToArt(Point p, Space space, const Transform& layerToQuad, int artW, int artH, Rotation rot,
                 bool flipX, bool flipY) noexcept {
    Point q = p;
    if (space == Space::Layer) {
        q = Point{layerToQuad.applyX(p.x, p.y), layerToQuad.applyY(p.x, p.y)};
    }
    return inverseOrientPoint(q, artW, artH, rot, flipX, flipY);
}

// The tight box of visible art pixels (origin + extent, pixel units), or an empty rect when nothing is
// visible. `covered(x, y)` is the per-pixel visibility predicate (the sheet cell, or the frozen mask).
template <typename Covered>
IntRect visibleArtBounds(int artW, int artH, Covered&& covered) {
    int minX = artW, minY = artH, maxX = -1, maxY = -1;
    for (int y = 0; y < artH; ++y) {
        for (int x = 0; x < artW; ++x) {
            if (covered(x, y)) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0) return IntRect{};
    return IntRect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

// Map a tight art-pixel box to a placed axis-aligned bounds in `space`: the box's four corners go through
// the orientation (Quad) and, for a Layer query, the placement homography; the result is the AABB of the
// mapped corners. Quad bounds are exact (orientation preserves the integer grid); Layer bounds round
// outward to whole pixels (a conservative broad-phase box).
IntRect placedBounds(IntRect artBox, int artW, int artH, Rotation rot, bool flipX, bool flipY, Space space,
                     const Transform& quadToLayer) {
    if (artBox.width <= 0 || artBox.height <= 0) return IntRect{};
    const float x0 = static_cast<float>(artBox.x);
    const float y0 = static_cast<float>(artBox.y);
    const float x1 = static_cast<float>(artBox.x + artBox.width);
    const float y1 = static_cast<float>(artBox.y + artBox.height);
    const Point corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
    for (const Point c : corners) {
        const Point q = orientPoint(c, artW, artH, rot, flipX, flipY);
        const Point o = (space == Space::Layer)
                            ? Point{quadToLayer.applyX(q.x, q.y), quadToLayer.applyY(q.x, q.y)}
                            : q;
        minX = std::min(minX, o.x);
        minY = std::min(minY, o.y);
        maxX = std::max(maxX, o.x);
        maxY = std::max(maxY, o.y);
    }
    const int ix0 = static_cast<int>(std::floor(minX));
    const int iy0 = static_cast<int>(std::floor(minY));
    const int ix1 = static_cast<int>(std::ceil(maxX));
    const int iy1 = static_cast<int>(std::ceil(maxY));
    return IntRect{ix0, iy0, ix1 - ix0, iy1 - iy0};
}

// ── Silhouette trace ───────────────────────────────────────────────────────────────────────────

// Signed area (shoelace) — the sign is the winding, the magnitude twice the enclosed area.
double signedArea(const std::vector<Point>& poly) {
    double a = 0.0;
    for (std::size_t i = 0, n = poly.size(); i < n; ++i) {
        const Point& p = poly[i];
        const Point& q = poly[(i + 1) % n];
        a += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    return 0.5 * a;
}

// The outer boundary loops of a mask (interior holes excluded). Each visible pixel contributes the four
// directed edges of its unit square; an edge shared by two visible pixels cancels its neighbor, so what
// survives is exactly the boundary between visible and not. The surviving directed edges chain into loops;
// the outer loops share the winding of the largest loop (a hole winds the other way and is dropped).
std::vector<std::vector<Point>> outerContours(const ArtMask& mask) {
    const int W = mask.width, H = mask.height;
    const int cornerRows = H + 1;
    const auto cornerId = [cornerRows](int cx, int cy) { return cx * cornerRows + cy; };
    const long long stride = static_cast<long long>((W + 1)) * cornerRows;
    const auto edgeId = [stride](int from, int to) { return static_cast<long long>(from) * stride + to; };

    std::unordered_set<long long> edges;
    const auto addEdge = [&](int fromC, int toC) {
        const long long rev = edgeId(toC, fromC);
        const auto it = edges.find(rev);
        if (it != edges.end()) {
            edges.erase(it);  // cancels a shared interior edge
        } else {
            edges.insert(edgeId(fromC, toC));
        }
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!mask.at(x, y)) continue;
            const int a = cornerId(x, y);          // top-left
            const int b = cornerId(x + 1, y);      // top-right
            const int c = cornerId(x + 1, y + 1);  // bottom-right
            const int d = cornerId(x, y + 1);      // bottom-left
            addEdge(a, b);
            addEdge(b, c);
            addEdge(c, d);
            addEdge(d, a);
        }
    }
    if (edges.empty()) return {};

    std::unordered_map<int, int> next;  // from-corner → to-corner (each from is unique on a boundary)
    next.reserve(edges.size());
    for (const long long e : edges) {
        next.emplace(static_cast<int>(e / stride), static_cast<int>(e % stride));
    }

    const auto corner = [cornerRows](int id) {
        const int cx = id / cornerRows;
        const int cy = id % cornerRows;
        return Point{static_cast<float>(cx), static_cast<float>(cy)};
    };

    std::vector<std::vector<Point>> loops;
    while (!next.empty()) {
        const int start = next.begin()->first;
        std::vector<Point> loop;
        int cur = start;
        while (true) {
            const auto it = next.find(cur);
            if (it == next.end()) break;  // defensive: malformed chain
            loop.push_back(corner(cur));
            const int nxt = it->second;
            next.erase(it);
            cur = nxt;
            if (cur == start) break;
        }
        if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
    if (loops.empty()) return {};

    // Keep the loops that share the winding of the largest — a component's outer boundary; drop holes.
    std::size_t biggest = 0;
    double biggestMag = 0.0;
    std::vector<double> areas(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        areas[i] = signedArea(loops[i]);
        if (std::abs(areas[i]) > biggestMag) {
            biggestMag = std::abs(areas[i]);
            biggest = i;
        }
    }
    const bool outerPositive = areas[biggest] > 0.0;
    std::vector<std::vector<Point>> outer;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if ((areas[i] > 0.0) == outerPositive) outer.push_back(std::move(loops[i]));
    }
    return outer;
}

// Drop vertices collinear with their neighbors (the long orthogonal runs of a pixel contour collapse to
// their corners).
std::vector<Point> collinearMerge(const std::vector<Point>& loop) {
    const std::size_t n = loop.size();
    if (n < 3) return loop;
    std::vector<Point> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Point& prev = loop[(i + n - 1) % n];
        const Point& cur  = loop[i];
        const Point& nxt  = loop[(i + 1) % n];
        const float cross = (cur.x - prev.x) * (nxt.y - cur.y) - (cur.y - prev.y) * (nxt.x - cur.x);
        if (std::abs(cross) > 1e-6f) out.push_back(cur);
    }
    if (out.size() < 3) return loop;  // degenerate — keep the raw loop
    return out;
}

// Andrew's monotone-chain convex hull (CCW). Containing by construction: every input point — hence every
// visible pixel corner — lies within the hull.
std::vector<Point> convexHull(std::vector<Point> pts) {
    std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    const std::size_t n = pts.size();
    if (n < 3) return pts;
    const auto cross = [](const Point& o, const Point& a, const Point& b) {
        return static_cast<double>(a.x - o.x) * (b.y - o.y) - static_cast<double>(a.y - o.y) * (b.x - o.x);
    };
    std::vector<Point> hull(2 * n);
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = n - 1; i-- > 0;) {
        while (k >= lower && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
        hull[k++] = pts[i];
    }
    hull.resize(k - 1);
    return hull;
}

// Reorient a polygon to positive winding (so a convex vertex turns left, a reflex vertex right).
void makeCcw(std::vector<Point>& poly) {
    if (signedArea(poly) < 0.0) std::reverse(poly.begin(), poly.end());
}

// The axis-aligned bounding box of a polygon, as a 4-corner CCW loop. Containing by construction — the
// coarsest Conservative silhouette (no axis-aligned containing box has fewer than four corners).
std::vector<Point> boundingBoxPoly(const std::vector<Point>& poly) {
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;
    for (const Point& p : poly) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    return {{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}};
}

// Conservative simplification: from the exact contour, remove concavities (reflex vertices — each removal
// only fills area, never carves), then, if a convex polygon is still over budget, collapse edges outward to
// their neighbor tangents (again only adding area). The result contains the silhouette at every budget.
std::vector<Point> conservativeSimplify(std::vector<Point> poly, int maxPoints) {
    if (static_cast<int>(poly.size()) <= maxPoints) return poly;
    makeCcw(poly);

    // Phase 1 — remove the least-cost reflex vertex until at budget or convex.
    while (static_cast<int>(poly.size()) > maxPoints) {
        const std::size_t n = poly.size();
        std::size_t best = n;
        double bestArea = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < n; ++i) {
            const Point& u = poly[(i + n - 1) % n];
            const Point& v = poly[i];
            const Point& w = poly[(i + 1) % n];
            const double cr = static_cast<double>(v.x - u.x) * (w.y - v.y) -
                              static_cast<double>(v.y - u.y) * (w.x - v.x);
            if (cr < 0.0) {  // reflex — removing it fills the concavity (adds area, stays containing)
                const double area = std::abs(cr) * 0.5;
                if (area < bestArea) {
                    bestArea = area;
                    best = i;
                }
            }
        }
        if (best == n) break;  // convex — no reflex vertex left
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(best));
    }
    if (static_cast<int>(poly.size()) <= maxPoints) return poly;

    // Phase 2 — convex and still over budget: collapse the edge whose outward tangent-intersection adds the
    // least area, dropping one vertex per step toward the bounding box. The box (4 corners) is the floor —
    // no axis-aligned containing shape is coarser — so a budget below 4 still returns the box.
    const int floorPoints = std::max(maxPoints, 4);
    while (static_cast<int>(poly.size()) > floorPoints) {
        const std::size_t n = poly.size();
        std::size_t bestEdge = n;
        double bestArea = std::numeric_limits<double>::max();
        Point bestPt{};
        for (std::size_t i = 0; i < n; ++i) {
            // edge (v, w); extend its neighbor edges (u→v) and (w→z) to their intersection `pt`.
            const Point& u = poly[(i + n - 1) % n];
            const Point& v = poly[i];
            const Point& w = poly[(i + 1) % n];
            const Point& z = poly[(i + 2) % n];
            const double d1x = v.x - u.x, d1y = v.y - u.y;
            const double d2x = z.x - w.x, d2y = z.y - w.y;
            const double den = d1x * d2y - d1y * d2x;
            if (std::abs(den) < 1e-9) continue;  // parallel — no finite outward apex
            const double t = ((w.x - u.x) * d2y - (w.y - u.y) * d2x) / den;
            const Point pt{static_cast<float>(u.x + t * d1x), static_cast<float>(u.y + t * d1y)};
            // The cap area added = triangle(v, pt, w).
            const double area = std::abs(static_cast<double>(pt.x - v.x) * (w.y - v.y) -
                                         static_cast<double>(pt.y - v.y) * (w.x - v.x)) *
                                0.5;
            if (area < bestArea) {
                bestArea = area;
                bestEdge = i;
                bestPt = pt;
            }
        }
        if (bestEdge == n) break;  // every edge's neighbors are parallel — the box handles it below
        poly[bestEdge] = bestPt;                                                    // v → the apex
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>((bestEdge + 1) % n));  // drop w
    }
    // A stall above the floor (a rectangle has only parallel opposite edges) settles on the bounding box.
    if (static_cast<int>(poly.size()) > floorPoints) poly = boundingBoxPoly(poly);
    return poly;
}

// Balanced simplification: greedily drop the least-significant vertex (smallest perpendicular offset from
// the segment between its neighbors) until at budget. Errs both ways — the tightest hug the budget buys.
std::vector<Point> balancedSimplify(std::vector<Point> poly, int maxPoints) {
    while (static_cast<int>(poly.size()) > maxPoints && poly.size() > 3) {
        const std::size_t n = poly.size();
        std::size_t best = 0;
        double bestDev = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < n; ++i) {
            const Point& a = poly[(i + n - 1) % n];
            const Point& b = poly[i];
            const Point& c = poly[(i + 1) % n];
            const double abx = c.x - a.x, aby = c.y - a.y;
            const double len = std::sqrt(abx * abx + aby * aby);
            const double dev = len < 1e-9
                                   ? std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y))
                                   : std::abs((b.x - a.x) * aby - (b.y - a.y) * abx) / len;
            if (dev < bestDev) {
                bestDev = dev;
                best = i;
            }
        }
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(best));
    }
    return poly;
}

}  // namespace

std::vector<Point> traceSilhouette(const ArtMask& mask, int maxPoints, ShapeTrace trace) {
    if (maxPoints < 3) throw std::invalid_argument("traceSilhouette: maxPoints must be >= 3");
    if (mask.empty()) return {};

    std::vector<std::vector<Point>> loops = outerContours(mask);
    if (loops.empty()) return {};  // fully transparent

    std::vector<Point> contour;
    if (loops.size() == 1) {
        contour = collinearMerge(loops[0]);
    } else {
        // Disconnected blobs — one polygon spanning them all via their common convex hull.
        std::vector<Point> all;
        for (const auto& loop : loops) all.insert(all.end(), loop.begin(), loop.end());
        contour = convexHull(std::move(all));
    }
    if (contour.size() < 3) return contour;

    return trace == ShapeTrace::Balanced ? balancedSimplify(std::move(contour), maxPoints)
                                         : conservativeSimplify(std::move(contour), maxPoints);
}

// ── The exact mask forms ─────────────────────────────────────────────────────────────────────────

bool SpriteMask::contains(Point p) const {
    if (sprite == nullptr) return false;
    const Renderer& r    = Renderer::instance();
    const int       artW = sprite->size.width;
    const int       artH = sprite->size.height;
    const Transform layerToQuad =
        space == Space::Layer ? spriteQuadToLayer(*sprite).inverse() : Transform{};
    const Point a = queryToArt(p, space, layerToQuad, artW, artH, sprite->rotation, sprite->flipX,
                               sprite->flipY);
    const int ax = static_cast<int>(std::floor(a.x));
    const int ay = static_cast<int>(std::floor(a.y));
    if (ax < 0 || ay < 0 || ax >= artW || ay >= artH) return false;
    const IntRect cell = spriteCellRect(r.atlasPixelSize(sprite->atlas).width, sprite->tile, sprite->size);
    return r.atlasVisibleAt(sprite->atlas, cell.x + ax, cell.y + ay);
}

IntRect SpriteMask::bounds() const {
    if (sprite == nullptr) return IntRect{};
    const Renderer& r    = Renderer::instance();
    const int       artW = sprite->size.width;
    const int       artH = sprite->size.height;
    const IntRect   cell = spriteCellRect(r.atlasPixelSize(sprite->atlas).width, sprite->tile, sprite->size);
    const IntRect   box  = visibleArtBounds(artW, artH, [&](int x, int y) {
        return r.atlasVisibleAt(sprite->atlas, cell.x + x, cell.y + y);
    });
    return placedBounds(box, artW, artH, sprite->rotation, sprite->flipX, sprite->flipY, space,
                        spriteQuadToLayer(*sprite));
}

bool FrozenSpriteMask::contains(Point p) const {
    if (mask.empty()) return false;
    const Transform layerToQuad = space == Space::Layer ? quadToLayer.inverse() : Transform{};
    const Point a = queryToArt(p, space, layerToQuad, mask.width, mask.height, rotation, flipX, flipY);
    return mask.at(static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)));
}

IntRect FrozenSpriteMask::bounds() const {
    if (mask.empty()) return IntRect{};
    const IntRect box =
        visibleArtBounds(mask.width, mask.height, [&](int x, int y) { return mask.at(x, y); });
    return placedBounds(box, mask.width, mask.height, rotation, flipX, flipY, space, quadToLayer);
}

// ── The Sprite verbs ─────────────────────────────────────────────────────────────────────────────
//
// Each verb reads the sprite's own sheet coverage from `atlas` against the engine renderer's uploaded
// pixels. A borrow defers to the sprite live; freezeMask / maskShape snapshot the cell now.

SpriteMask Sprite::mask(Space space) const {
    return SpriteMask{.sprite = this, .space = space};
}

FrozenSpriteMask Sprite::freezeMask(Space space) const {
    return FrozenSpriteMask{.mask       = spriteCellMask(*this),
                             .rotation   = rotation,
                             .flipX      = flipX,
                             .flipY      = flipY,
                             .space      = space,
                             .quadToLayer = spriteQuadToLayer(*this)};
}

ShapePoints Sprite::maskShape(int maxPoints, Space space, ShapeTrace trace) const {
    const ArtMask mask = spriteCellMask(*this);
    const std::vector<Point> raw = traceSilhouette(mask, maxPoints, trace);  // throws when maxPoints < 3
    const Transform toLayerM = spriteQuadToLayer(*this);
    std::vector<Point> out;
    out.reserve(raw.size());
    for (const Point a : raw) {
        const Point q = orientPoint(a, size.width, size.height, rotation, flipX, flipY);
        out.push_back(space == Space::Layer
                          ? Point{toLayerM.applyX(q.x, q.y), toLayerM.applyY(q.x, q.y)}
                          : q);
    }
    return ShapePoints{.points = std::move(out)};
}

}  // namespace retropp
