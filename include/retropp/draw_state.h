#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "retropp/curve.h"      // CurveSegment — an optional curved region boundary
#include "retropp/geometry.h"   // PixelSize
#include "retropp/guest_frame.h" // GuestFrameContent — a hosted machine's picture as layer content
#include "retropp/image.h"      // AtlasId (relocated here beside the atlas-ingestion surface)
#include "retropp/object_key.h" // ObjectKey — the required developer-supplied reconciliation identity
#include "retropp/output.h"     // EvaluationGrid (leaf header — the crisp-evaluation grid selector)
#include "retropp/palette.h"    // PaletteId
#include "retropp/transform.h"  // Transform

namespace retropp {

// The draw-state submission envelope: the C++ shape a game hands the renderer each frame.
//
// The frame's draw state is computed WHOLE every frame from the game's logical inputs —
// there is no mid-frame state-change API, no reconstructed scanline ISR, no register poke.
// A game recomputes and resubmits a fresh FrameDrawState per frame; layer existence, z,
// scroll, size, alpha, and effect/modifier parameters are all fresh each frame (the one
// amortized exception is the atlas texels, uploaded when they change). Effects that a GB
// expressed through hardware tricks are expressed here as layers, per-tile/per-region
// colour attributes, frame-level modifiers, and screen-space-effect declarations — never
// as a hardware-register idiom.
//
// Identity is a typed, first-class field throughout (the reconciliation Key, AtlasId, the *Kind enums) —
// never an array position, never demoted to a comment.

// ── Reconciliation key ────────────────────────────────────────────────────────────────

// ObjectKey — the required, developer-supplied identity the renderer matches an object to its previous
// tick state by — lives in object_key.h (included above), a leaf header, so surfaces that need a named
// identity without the rendering vocabulary reach it directly.

// AtlasId (a handle to uploaded atlas pixel data) lives in image.h beside the atlas-ingestion
// surface — included above. TileContent / SpriteContent below carry the fully-qualified
// retropp::AtlasId.

// ── Tile content ────────────────────────────────────────────────────────────────────

// A 90° TEXTURE rotation of a cell — which source atlas pixel each output pixel reads, the same kind
// of operation as flipX/flipY (a fragment read, not a geometry change). It composes with the flips to
// reach all eight orientations of square art: one corner tile serves all four corners, one edge tile
// all four sides. Clockwise. Distinct from the geometric Sprite::transform / DrawLayer::transform,
// which rotate the destination QUAD at an arbitrary angle — this is the discrete, grid-aligned,
// cell-packed sibling of the flips.
enum class Rotation : std::uint8_t { None = 0, Rot90 = 1, Rot180 = 2, Rot270 = 3 };

// The coordinate space a space-dual query answers in. Quad = the sprite's own rectangle before
// placement — orientation applied, but no transform and no placement; the space `origin` and `pivot`
// live in, so a transform INPUT is read here. Layer = the layer's coordinate space, the point mapped
// through the sprite's transform + placement (matches toLayer()) — what a sibling, a path, or an effect
// downstream consumes. One enum shared by the quad-vs-layer queries — anchor(k, Space) and
// center(Space) — so the whole surface reads as one grammar. Always passed explicitly (no default): the
// query names the space it answers in, and the space is a value the caller can compute, store, or loop over.
enum class Space : std::uint8_t { Quad, Layer };

// A source texel within a cell after orientation. The tile and sprite fragment shaders reproduce
// sourceCellTexel exactly.
struct CellTexel {
    int x = 0;
    int y = 0;
    [[nodiscard]] constexpr bool operator==(const CellTexel&) const noexcept = default;
};

// Map a destination within-cell pixel (dx, dy) in [0,w) x [0,h) to the SOURCE atlas pixel to read,
// applying the flips and then the rotation. Order is "rotate the art, then mirror": the flip is applied
// to the destination coordinate first, then the inverse rotation selects the source. For a square cell
// (w == h — every tile, and a square sprite) all eight orientations stay in bounds. For a non-square
// cell (a sprite whose width != height) Rot90/Rot270 transpose the read — the source extents swap — a
// permitted result (use the geometric transform for true quad rotation of non-square art). Pure +
// constexpr: it is the single authority the two fragment shaders mirror.
[[nodiscard]] constexpr CellTexel sourceCellTexel(int dx, int dy, int w, int h,
                                                  Rotation rot, bool flipX, bool flipY) noexcept {
    if (flipX) dx = w - 1 - dx;
    if (flipY) dy = h - 1 - dy;
    switch (rot) {
        case Rotation::Rot90:  return CellTexel{dy,         w - 1 - dx};
        case Rotation::Rot180: return CellTexel{w - 1 - dx, h - 1 - dy};
        case Rotation::Rot270: return CellTexel{h - 1 - dy, dx};
        case Rotation::None:   break;
    }
    return CellTexel{dx, dy};
}

// One cell of a tilemap: which atlas tile, which sheet, which palette, flip. Named fields per the
// no-positional-opacity discipline — identity is a field, never a packed byte behind a comment. Each
// cell names its OWN sheet (`atlas`) and palette directly: `tile` is the cell index within that
// sheet's 8px grid, `palette` is the palette it colours through. One map layer therefore mixes tiles
// from any number of sheets and palettes — there is no per-layer set or select.
struct TileCell {
    AtlasId       atlas{};      // which uploaded sheet this cell draws from
    std::uint16_t tile    = 0;  // cell index within its own sheet (`atlas`), on the 8px grid
    PaletteId     palette{};    // which uploaded palette colours it
    bool          flipX   = false;
    bool          flipY   = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};

// How a tile layer's tilemap is sampled outside its [0, mapPx) bounds. One mode governs both axes;
// the field lives on TileContent (it governs *tilemap* sampling; a sprite has no tilemap).
//   Repeat — toroidal: the map tiles infinitely on both axes (floorMod wrap).
//   Clamp  — clamp the world coord to the map's edge row/column (smear the border tile outward).
//   Blank  — FINITE map: a world coord outside [0, mapPx) on EITHER axis is a hole (transparent;
//            the layers below show through), so the map renders exactly once and can never show a
//            wrap seam — the mode a finite overworld map wants.
// Same blank-edge vocabulary as the transform footprint's DisplacementEdge::Blank — Blank discards
// to reveal the layers below.
enum class TileWrap : std::uint8_t { Repeat, Clamp, Blank };

// A tile layer's content: a row-major tilemap (widthInTiles × heightInTiles) of cells, each naming
// its own indexed sheet + palette. The map is sampled per-pixel in the tile shader against the
// layer's scroll, so arbitrary layer sizes and wrapping are handled on the GPU; `wrap` chooses how
// the tilemap is sampled beyond its bounds (Repeat/Clamp/Blank). `cells` is game-owned; valid for the
// duration of the renderFrame() call that consumes it.
struct TileContent {
    int                       widthInTiles  = 0;
    int                       heightInTiles = 0;
    std::span<const TileCell> cells;           // row-major, widthInTiles * heightInTiles
    TileWrap                  wrap = TileWrap::Repeat;  // out-of-bounds sampling; Repeat = toroidal
    // Optional content-change declaration for the upload skip. Unset (the default): the platform hashes the
    // packed cells each frame and skips the GPU re-upload when they are unchanged — automatic, no
    // bookkeeping. Set it to answer the change question yourself and skip even that per-frame hash, the
    // path for a huge map: `true` = these cells changed, re-upload them; `false` = unchanged, skip. It is
    // a declaration, not a hint — setting `false` while the cells have in fact changed renders the stale
    // map. That is the contract, not a defect.
    std::optional<bool>       contentChanged;
};

// The two-word (R32G32_UINT) tilemap cell the tile fragment shader unpacks:
//   word0: tile (bits 0..15) | flipX (bit 16) | flipY (bit 17) | rotation (bits 18..19)
//   word1: atlas (bits 0..15, AtlasId) | palette (bits 16..31, PaletteId)
// The atlas and palette handles are carried directly — a PaletteId is already the flat palette-store
// offset; an AtlasId indexes the global atlas-region table. This constexpr pair is the unit-tested
// mirror of the GPU packing — the shader unpacks the identical layout, so
// unpackTileCell(packTileCell(c)) == c for every valid cell.
struct PackedTileCell {
    std::uint32_t w0 = 0;
    std::uint32_t w1 = 0;
    [[nodiscard]] constexpr bool operator==(const PackedTileCell&) const noexcept = default;
};

[[nodiscard]] constexpr PackedTileCell packTileCell(const TileCell& c) noexcept {
    return PackedTileCell{
        static_cast<std::uint32_t>(c.tile)
            | (static_cast<std::uint32_t>(c.flipX ? 1u : 0u) << 16)
            | (static_cast<std::uint32_t>(c.flipY ? 1u : 0u) << 17)
            | (static_cast<std::uint32_t>(c.rotation) << 18),
        static_cast<std::uint32_t>(c.atlas)
            | (static_cast<std::uint32_t>(c.palette) << 16),
    };
}

[[nodiscard]] constexpr TileCell unpackTileCell(PackedTileCell p) noexcept {
    TileCell c;
    c.tile    = static_cast<std::uint16_t>(p.w0 & 0xFFFFu);
    c.flipX   = ((p.w0 >> 16) & 1u) != 0u;
    c.flipY   = ((p.w0 >> 17) & 1u) != 0u;
    c.rotation = static_cast<Rotation>((p.w0 >> 18) & 3u);
    c.atlas   = static_cast<AtlasId>(p.w1 & 0xFFFFu);
    c.palette = static_cast<PaletteId>((p.w1 >> 16) & 0xFFFFu);
    return c;
}

// ── Point ─────────────────────────────────────────────────────────────────────────────

// A point in PIXELS (top-left origin) — the shared unit of every placed thing: Sprite::x/y and the
// pivot/anchor space (sprite-local px), effect-region vertices and `radius` (viewport px), the
// Transform pivots. Deliberately pixels, not normalized UV, so points and distances share one unit.
// Points are the platform's lingua franca: a resolver that answers "where" answers in a Point, and any
// consumer that takes a position takes one. Identity is the named fields.
struct Point {
    float x = 0.0f;
    float y = 0.0f;
    [[nodiscard]] constexpr bool operator==(const Point&) const noexcept = default;
};
static_assert(sizeof(Point) == 8 && alignof(Point) == 4,
              "Point must match the shader's float2 — it is memcpy'd into the points storage buffer");

// ── Sprite content ────────────────────────────────────────────────────────────────────

// A sprite's pixel dimensions are an AssetDimensions (geometry.h) — the same type the atlas slicer
// carves an image into, with the console-named presets (AssetDimensions::GameBoy8x8, …).

// A named point on a sprite's ART — a socket, hinge, muzzle, emitter, tow-hook: anywhere something
// attaches or something spawns. `x`/`y` are ART-space pixels (the art as it sits on the sheet, before
// flips/rotation/transform); `label` is the durable address (an index is an address too, but reordering
// a table silently repoints an index — a label survives reordering and fails loudly). Anchors publish
// points; pivots consume them: attach a sprite to another by writing the anchor's resolved coordinates
// into the child's x/y ("pivot on this anchor") — two resolvers composing, nothing held.
struct Anchor {
    std::string_view label;   // durable identity — addressable by name as well as by index
    float            x = 0.0f;  // art-space px
    float            y = 0.0f;
};

// The first duplicated label in an anchor table, or nullopt when every label is unique. constexpr —
// the static_assert seam for a compile-time-known table (the layerKeysAreUnique idiom):
//   static_assert(!findDuplicateAnchorLabel(kClawAnchors), "duplicate anchor label");
// At query time a duplicate is not an error — anchor(k, Space) returns the FIRST match.
[[nodiscard]] constexpr std::optional<std::string_view>
findDuplicateAnchorLabel(std::span<const Anchor> anchors) noexcept {
    for (std::size_t i = 0; i < anchors.size(); ++i)
        for (std::size_t j = i + 1; j < anchors.size(); ++j)
            if (anchors[i].label == anchors[j].label) return anchors[i].label;
    return std::nullopt;
}

// Where an ART-space point lands on the destination QUAD under the sprite's texture orientation ops —
// the continuous forward map of `rotation` then the flips (the inverse of sourceCellTexel's dest→source
// read; at pixel centres the two agree exactly, which the unit tests pin). Anchors ride this map (they
// are points on the drawn thing — a flipped leg's socket mirrors with the leg); the PIVOT deliberately
// does not (it is the quad's placement handle, and texture ops never move geometry). For a non-square
// sprite Rot90/Rot270 transpose the art extents — the same transpose the texture read makes.
[[nodiscard]] constexpr Point orientPoint(Point p, int width, int height,
                                          Rotation rot, bool flipX, bool flipY) noexcept {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    Point q = p;
    switch (rot) {
        case Rotation::Rot90:  q = Point{w - p.y, p.x};     break;
        case Rotation::Rot180: q = Point{w - p.x, h - p.y}; break;
        case Rotation::Rot270: q = Point{p.y, h - p.x};     break;
        case Rotation::None:   break;
    }
    if (flipX) q.x = w - q.x;
    if (flipY) q.y = h - q.y;
    return q;
}

// ── Container blend mode ──────────────────────────────────────────────────────────────────────
//
// How a compositing CONTAINER's pixels combine with what they composite over. A container — a Sprite, a
// Region, a DrawLayer, or the whole FrameDrawState — carries a BlendMode beside its `alpha`: `alpha` is
// HOW MUCH the container contributes, `blend` is HOW it combines. Normal is the alpha-over of a Photoshop-
// style layer stack (the default, and the exact output when every container is Normal); the
// others are the standard separable blend operators a retro look reaches for — Add (glows / fire /
// light), Subtract, Multiply (shadows / tints), Screen (bloom), and Half (a halved average,
// (dst+src)/2, for translucency). Blend is a property of the CONTAINER that owns the pixels, never of a
// screen-space effect: an effect is a colour SOURCE, and the region / layer / frame that holds it
// decides how that source merges. The math is the separable operator B(dst, src) per mode applied
// source-alpha-weighted; retropp::applyBlendMode (postprocess.h) is the single authority the
// compositor shaders mirror.
enum class BlendMode : std::uint8_t {
    Normal,    // alpha-over: (1-srcA)·dst + srcA·src — the default; at alpha 1 the source replaces
    Add,       // additive: dst + src           (glows, fire, light)
    Subtract,  // subtractive: dst − src
    Multiply,  // multiplicative: dst · src      (shadows, tints)
    Screen,    // 1 − (1−dst)(1−src) — inverse-multiply (bloom)
    Half,      // (dst + src) / 2 — a halved average (translucency)
};

// ── Effect region — the shape an effect is confined to ───────────────────────────────────

// Effect-region geometry is authored in Points (declared above) in VIEWPORT PIXELS — the screen
// space an effect composites in. (A region carried by a Sprite instead reads its shape in the
// sprite's QUAD space — see Sprite::regions.)

// The shape an effect is confined to. A polygon of ordered VIEWPORT-PIXEL vertices, inflated by
// `radius` (a signed-distance rounding), warped by `transform`. The points ARE the position — there
// is no separate origin. Containment is an SDF, not a rasterized polygon, so one type covers every
// shape (see regionContains / sdPolygon in postprocess.h):
//   empty                → NO region: the effect covers the whole viewport (the default).
//   1 point + radius     → a CIRCLE (distance-to-point ≤ radius).
//   2 points + radius    → a CAPSULE / stadium (distance-to-segment ≤ radius).
//   ≥ 3 points, radius 0 → a sharp polygon (triangle / quad / N-gon; arbitrary CONCAVE outlines OK).
//   ≥ 3 points, radius>0 → a rounded polygon.
// `transform` (identity default) is a Transform composed on top — scale / stretch (non-uniform
// scale) / skew / rotate / perspective / translate the placed shape, evaluated by the same inverse-
// homography the tile path uses. Move a shape by rewriting points OR via transform translation.
//
// `points` is a std::vector with no cap in the API; the renderer packs the vertices into the
// region-select cbuffer, which carries up to 64 — a longer polygon is truncated there with a logged
// warning. The presets are the named-constructor idiom (a placed shape is parametric); a raw
// ShapePoints{ .points = {...}, .radius = r } stays allowed for the unnamed.
//
// `curve` (the default-empty member) makes the boundary a CLOSED CURVE rather than a straight-edged
// polygon — exact between control points, no facets, no vertex cap. When `curve` is non-empty it IS
// the boundary and `points` is ignored; `radius` (SDF inflation) and `transform` (the inverse-
// homography warp) compose on top of the curve distance exactly as they do for a polygon. Linear and
// quadratic segments evaluate analytically (exact). A cubic / Catmull-Rom segment has no closed-form GPU
// distance: attach a baked mask (`curveMask`, from Renderer::bakeCurveMask / bakeCurveRegion) to evaluate
// it exactly, or leave it unset and the boundary samples to a faceted polygon (the points path) — see
// fromCurve and the region gate. Empty `curve` ⇒ the polygon path, identical to a curve-free region.

// A handle to a baked curve signed-distance mask (Renderer::bakeCurveMask). A cubic / Catmull-Rom /
// arbitrary closed boundary has no closed-form GPU distance, so its Curve::signedDistance is baked once
// into a texture the region samples per fragment. 0 (the default) = none — the region carries no baked mask.
enum class CurveMaskId : std::uint32_t {};

struct ShapePoints {
    std::vector<Point>        points;        // ordered viewport-pixel vertices; empty = no polygon
    float                     radius = 0.0f; // SDF inflation: 0 = sharp polygon edges
    Transform                 transform{};   // additional warp, identity default
    std::vector<CurveSegment> curve;         // a CLOSED curve boundary (viewport px); empty = none
    // Treat the OUTSIDE of the shape as the region instead of the inside. A region-confined effect then
    // applies OUTSIDE the shape; the inside/outside test simply flips. Default false (the inside is the
    // region). An empty region ignores it.
    bool                      invert = false;
    // Confine to a BAND along the shape's boundary instead of its filled interior — the shape's outline.
    // 0 (default) = the filled region (the whole inside). > 0 = a stroke of that width (viewport px),
    // centered on the radius-inflated boundary, so the region's effects trace a hoop / border / curved
    // PATH rather than fill the interior. Composes with `radius` (the band rides the inflated edge),
    // `transform`, `curve`, and `invert` (stroke then invert = everything except the band). Applies to
    // EVERY region consumer — the effect-confinement gate AND the Transparency see-through path — because
    // it is one transform of the signed distance both already compute. A stroke is symmetric about the
    // boundary, hence sign-independent, so an OPEN fromCurve(...) strokes into an open band (an effect
    // follows an arbitrary curved path); a polygon (via `points`) is always a closed-loop band.
    float                     strokeWidth = 0.0f;
    // A baked signed-distance mask for a CUBIC / arbitrary curved boundary — the exact GPU evaluation of a
    // boundary the analytic linear+quadratic path cannot solve in closed form. 0 (default) = none. Bake one
    // with Renderer::bakeCurveMask (or take a ready shape from Renderer::bakeCurveRegion); the region samples
    // the mask per fragment instead of sampling the curve to a faceted polygon. `radius`, `strokeWidth`,
    // `transform`, and `invert` compose on the sampled distance unchanged. Consulted only when `curve` carries
    // a cubic segment (a linear/quadratic boundary stays on the exact analytic path; a polygon ignores it).
    CurveMaskId               curveMask{};

    [[nodiscard]] bool operator==(const ShapePoints&) const = default;
    [[nodiscard]] bool hasRegion() const noexcept { return !points.empty() || !curve.empty(); }

    // Is `p` inside this shape? The same containment the region gate resolves on screen — the test routes
    // through the gate's CPU mirrors (curveRegionContains / regionContains), so a drawn Region and this
    // test agree by construction: what you see is what you hit. `radius`, `strokeWidth`, `transform`,
    // `invert` and a curve boundary all apply, exactly as they draw. An EMPTY shape (hasRegion() false)
    // contains nothing — note this deliberately differs from the free-function mirrors, where an effect
    // with no region applies everywhere. O(vertices) per test (the polygon SDF walks the vertex list).
    // `p` is in this shape's OWN coordinates: ShapePoints carries no notion of its space, so matching
    // spaces is the caller's job — a shape authored in viewport pixels tests viewport points, and a mask
    // from Sprite::maskShape(n, space) tests points in that `space`. Callers on the crisp path who need
    // Viewport-grid snapping use the free mirrors' `snap` parameter directly.
    // (Defined in draw_state.cpp — the mirrors live in postprocess.h, which includes this header.)
    [[nodiscard]] bool contains(Point p) const noexcept;

    // A copy of this shape with its inside and outside swapped (toggles `invert`). Use it to confine a
    // standalone Region's effects to the OUTSIDE of a shape you authored:
    //   Region{ .shape = ShapePoints::circle(c, r).inverted(), .effects = {…} }.
    // The stencil() helper uses it to put a side effect on the OUTSIDE of a Transparency's shape.
    [[nodiscard]] ShapePoints inverted() const { ShapePoints s = *this; s.invert = !s.invert; return s; }

    // Named-constructor presets (the Transform::rotation() idiom). All coordinates are viewport pixels.
    [[nodiscard]] static ShapePoints circle(Point c, float r);
    [[nodiscard]] static ShapePoints capsule(Point a, Point b, float r);
    [[nodiscard]] static ShapePoints triangle(Point a, Point b, Point c);
    [[nodiscard]] static ShapePoints rectangle(Point topLeft, float w, float h);
    [[nodiscard]] static ShapePoints roundedRectangle(Point topLeft, float w, float h, float r);
    [[nodiscard]] static ShapePoints regularPolygon(Point c, float r, int sides);

    // A region whose boundary is the curve `c` (treated as closed — the last segment's end joins the
    // first's start). `radius` inflates it; `transform` warps it. The primary path for genuinely curved
    // boundaries authored with Curve::quadratic / quadraticTo / line / lineTo (and cubic / Catmull-Rom,
    // which the gate samples to a faceted polygon).
    [[nodiscard]] static ShapePoints fromCurve(const Curve& c, float radius = 0.0f, Transform t = {});
};

inline ShapePoints ShapePoints::circle(Point c, float r)        { return ShapePoints{.points = {c}, .radius = r}; }
inline ShapePoints ShapePoints::capsule(Point a, Point b, float r) { return ShapePoints{.points = {a, b}, .radius = r}; }
inline ShapePoints ShapePoints::triangle(Point a, Point b, Point c) { return ShapePoints{.points = {a, b, c}}; }

inline ShapePoints ShapePoints::rectangle(Point tl, float w, float h) {
    return ShapePoints{.points = {{tl.x, tl.y}, {tl.x + w, tl.y}, {tl.x + w, tl.y + h}, {tl.x, tl.y + h}}};
}

// A rounded rectangle that FITS within [topLeft, topLeft+{w,h}]: the polygon is inset by r on every side
// and the SDF inflates it back out by r, so corners round with radius r and the outer extent is exactly
// w×h (for r ≤ min(w,h)/2). r = 0 reduces to the sharp rectangle's footprint.
inline ShapePoints ShapePoints::roundedRectangle(Point tl, float w, float h, float r) {
    const float x0 = tl.x + r, y0 = tl.y + r, x1 = tl.x + w - r, y1 = tl.y + h - r;
    return ShapePoints{.points = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, .radius = r};
}

// A regular n-gon (n floored at 3, no upper cap) centred at c with circumradius r, first vertex at the
// top. The unbounded points make a high `sides` a smooth-looking circle approximation if you want one.
inline ShapePoints ShapePoints::regularPolygon(Point c, float r, int sides) {
    const int n = sides < 3 ? 3 : sides;
    ShapePoints s;
    s.points.reserve(static_cast<std::size_t>(n));
    constexpr float kTwoPi  = 6.283185307179586f;
    constexpr float kHalfPi = 1.5707963267948966f;
    for (int i = 0; i < n; ++i) {
        const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(n) - kHalfPi;  // start at top
        s.points.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return s;
}

// The boundary IS the curve `c` (its segments, treated as a closed loop). `points` stays empty so the
// gate takes the curve path; radius/transform ride along unchanged.
inline ShapePoints ShapePoints::fromCurve(const Curve& c, float radius, Transform t) {
    return ShapePoints{.radius = radius, .transform = t, .curve = c.segments};
}

// ── Screen-space effects ──────────────────────────────────────────────────────────────────

enum class Axis : std::uint8_t { Horizontal, Vertical };

enum class ScreenSpaceEffectKind : std::uint8_t {
    None,            // pass-through — no effect (the default)
    RowDisplacement, // wavy water / heat haze / per-line SCX = f(row, time) in a shader
    Ripple,          // radial concentric ripple — a water droplet; built-in
    Custom,          // a game-registered shader — see PostProcessStageId + .customShader
    Transparency,    // make the effect's region SEE-THROUGH (reveal what's behind); built-in
    ColorFill,       // paint a colour onto the effect's region — out.rgb = fill (a solid fill); built-in
    Gleam,           // luminance-keyed diagonal sheen sweep — the marquee "shine"; built-in
    ColorSaturation, // cross-channel desaturation — pull each pixel toward its own luminance; built-in
    Bloom,           // threshold-blur-add glow — bright content radiates a soft halo; built-in
    Glow,            // authored-colour aura — a threshold-blurred emission mask times a chosen tint; built-in
    Swirl,           // angular twist about a centre — the whirlpool / vortex; built-in
};

// Which side of a Transparency effect's region goes see-through (kind == Transparency). The region is the
// shape of the Region that owns the effect; this picks which side of it the layer turns transparent along.
//   TransparentInside  — the pixels INSIDE the shape go see-through → a hole; the layers below show through. (default)
//   TransparentOutside — the pixels OUTSIDE the shape go see-through → a porthole; only the inside stays solid.
// Transparency is the subtractive sibling of region-as-fill: where a region confines an effect to ADD inside
// a shape, Transparency makes the layer SEE-THROUGH to reveal what is behind it. (The `stencil()` free helper
// builds the equivalent Region(s) for the common "make a shape see-through" case — see below.)
enum class StencilMode : std::uint8_t { TransparentInside, TransparentOutside };

// The platform's BUILT-IN effect library is the set of ScreenSpaceEffectKinds the platform
// owns a shader for — RowDisplacement (the axis-aligned wave), Ripple (the radial droplet), and ColorFill
// (a colour painted onto a region). A game sets `.kind` on a ScreenSpaceEffect and fills the fields that
// kind consults (plain designated-init — every field is settable inline); the platform supplies the shader.
// No registration, no shader authoring — that is the Custom path. New built-ins land behind this enum.
// Which fields each built-in consults (the rest stay at their defaults, ignored):
//   RowDisplacement → amplitude, frequency, phase, axis, edge
//   Ripple          → amplitude, frequency, phase, center, decay
//   Custom          → none of the above — the game's own shader + uniform define the behaviour
//   Transparency    → stencil, feather — makes its REGION see-through (no colour effect); the region is the
//                     shape of the Region that owns it (like every other effect; confinement comes from a Region)
//   ColorFill       → fill, fillIntensity — paints a colour (out.rgb = fill · fillIntensity) onto its region;
//                     via the owning region's blend mode it is the day/night (Multiply), tint (Add), and
//                     fade/flash (Normal) workhorse; fillIntensity > 1 lets Multiply brighten (float16 path)
//   Gleam           → sweep, width, gain, slant — a diagonal luminance-keyed sheen band over its region
//   ColorSaturation → saturation — pulls each pixel toward its own luminance (0 grey … 255 identity)
//   Bloom           → radius, threshold, intensity — brighter-than-threshold content blurs outward and adds
//                     back as a glow halo (0-intensity or 0-radius = identity)
//   Glow            → radius, threshold, intensity, fill, fillIntensity — a threshold-blurred emission mask
//                     times an AUTHORED tint (fill · fillIntensity); Bloom radiates the source's own colour,
//                     Glow radiates the colour you pick (threshold 0 = the whole silhouette emits)
//   Swirl           → center, radius, amplitude, phase — an angular twist about `center` within `radius`,
//                     amplitude + phase DEGREES of turn at the centre easing to none at the rim
//   (any kind)      → paramTable — a generic per-row data table (one Vec4 per scanline / per region id);
//                     in v1 only a Custom shader reads it (via the preamble's paramRow / paramRowAtUv)
// (scope applies to EVERY kind: it is a compositing decision the platform makes, not the shader's. NO effect
//  carries its own geometry — every kind is region-agnostic; confinement comes from a Region.)

// A handle to a game-registered custom shader stage the renderer owns.
// Identity is the typed handle, mirroring AtlasId/PaletteId; the renderer maps it to the pipeline
// pair it built from the game's fragment in registerPostProcessStage(). A custom shader is a
// first-class effect KIND: an effect with kind == Custom carries one of these in .customShader and
// runs through the SAME per-layer (Layer/Below) and frame-level (postEffects) machinery the built-in
// effects use — wherever a built-in effect works, a custom one does too.
enum class PostProcessStageId : std::uint32_t {};

// What a displacement does at the frame edge, where a row/column pulled inward exposes a strip with
// no source pixel behind it. Developer-selectable per effect:
//   Blank   — the exposed strip is the backdrop colour (nothing there). The faithful default: a
//             whole-frame displacement has no off-screen content to reveal, so it shows blank.
//   Stretch — the edge pixel is duplicated outward (CLAMP_TO_EDGE), smearing the border colour.
enum class DisplacementEdge : std::uint8_t { Blank, Stretch };

// Which pixels an effect transforms — the composable Photoshop-layer model. (Meaningful for
// DrawLayer::effects and Sprite::effects; FrameDrawState::postEffects is inherently whole-frame and
// ignores it.)
//   Layer — ISOLATED: displace ONLY this content's own pixels, before it composites. A wavy water
//           layer distorts while the layers/sprites composited above it stay still; on a sprite,
//           the effect transforms the sprite's own art. The default.
//   Below — ADJUSTMENT LAYER: displace the WHOLE accumulated image at this layer's z — this layer's
//           own content AND everything beneath it, coherently — then layers above this z composite
//           on top, undisplaced. A content-less Below layer just under a HUD wobbles the world while
//           the HUD rides steady; a content-bearing Below layer wobbles itself together with the
//           scene beneath. Multiple Below effects compose by z. On a SPRITE, a Below effect makes the
//           sprite a refraction lens: it distorts the composited scene beneath the sprite's layer,
//           confined to the sprite's silhouette (the art's alpha coverage), and the sprite draws no art
//           of its own — the art is purely the coverage mask, so an opaque mask gives a full-strength
//           lens with no self-occlusion.
enum class ScreenSpaceEffectScope : std::uint8_t { Layer, Below };

// A screen-space effect declaration: the parameters carried as data, interpreted by the effect's
// shader stage. In screen space the fragment's row coordinate IS the scanline, so a continuous
// effect is a function f(row, time, frame-state) the GPU evaluates per-pixel — no reconstructed LY
// counter, no HBlank ISR. The game advances `phase` per frame to animate (runtime-dynamic).
struct ScreenSpaceEffect {
    ScreenSpaceEffectKind kind = ScreenSpaceEffectKind::None;  // identity, first member

    // kind == Custom only — WHICH registered custom shader runs (the handle from
    // Renderer::registerPostProcessStage(path)). A Custom effect carries its handle here, then sets the
    // shader's OWN declared parameters as inline fields below (the generated union) — exactly the way a
    // built-in sets amplitude/center/etc. Placed right after `kind` so the call reads
    //   ScreenSpaceEffect{ .kind = Custom, .customShader = h, .<param> = …, … }.
    PostProcessStageId customShader{};

    // ── Built-in effect parameters (amplitude/frequency/phase/axis ignored for kind == Custom — a custom
    //    shader uses its OWN params, below; `edge` and `scope` apply to ALL kinds incl. Custom) ──
    float amplitude = 0.0f;   // displacement magnitude, viewport px (RowDisplacement / Ripple); DEGREES of
                              // turn for Swirl — a per-kind unit reading, see the Swirl block below
    float frequency = 0.0f;   // cycles across the axis (RowDisplacement) / rings across the field (Ripple)
    float phase     = 0.0f;   // animation phase — the game advances it off frame time to animate. Swirl ADDS
                              // it to `amplitude` (degrees) so advancing it spins the vortex
    Axis  axis      = Axis::Horizontal;                            // RowDisplacement
    // Edge policy at the frame border — governs RowDisplacement's exposed strip AND a Custom shader's
    // sampleSource() (the platform forwards it to the shader): Blank (default) = transparent (reveal the
    // backdrop / layers below); Stretch = clamp/smear. A layer that doesn't want clamping never gets it.
    DisplacementEdge edge = DisplacementEdge::Blank;
    ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer;  // per-layer reach; Layer (isolated) default

    // Ripple: a RADIAL concentric displacement (a water droplet) — the sample is pushed along
    // the radius from `center` by sin(2π·(frequency·dist − phase)), faded by exp(−decay·dist). `center` is
    // VIEWPORT PIXELS (like Point / Sprite::x,y — the platform normalizes to UV and aspect-corrects so the
    // rings stay circular); `decay` is the radial falloff.
    Point center{};
    float decay = 0.0f;

    // ── Transparency parameters (kind == Transparency) ──
    // A Transparency makes its REGION see-through (the shape comes from the owning Region, like every other
    // effect — it carries no geometry of its own). `stencil` picks which side of that region goes see-through
    // (TransparentInside = the inside, TransparentOutside = the outside); `feather` softens the boundary (shape-local px,
    // the same units as shape.radius — 0 = a hard edge, > 0 = a coverage ramp centered on the boundary).
    // `scope` and `edge` apply as for every kind; the other built-in params are ignored. The `stencil()` free
    // helper (below) builds the Region(s) for the common "make a shape see-through, optionally run effects on
    // each side" case.
    StencilMode stencil = StencilMode::TransparentInside;
    float       feather = 0.0f;

    // ── ColorFill parameters (kind == ColorFill) ──
    // Paint a colour onto the pixels the effect covers (its region): a stroked Region draws a colored
    // line/path, a filled Region a solid shape. out.rgb = fill; the layer alpha sets the opacity. The
    // owning Region's `blend` grades how the fill combines over the scene — Multiply for a shadow / tint /
    // day-night, Add for a glow, Screen for bloom — with Normal replacing the covered pixels (a fade / flash).
    // The CPU mirror of the fill colour is retropp::applyColorFill; the grade is retropp::applyBlendMode.
    Rgba8 fill{};

    // A scalar applied to `fill` so the painted colour can exceed 1 (the fill is fill_rgb/255 · fillIntensity).
    // Default 1 = the plain fill. Above 1 only shows through a container blend that carries the headroom —
    // Multiply (scene · fill, a multiplicative exposure that BRIGHTENS while preserving contrast), Screen, Add
    // — and only because the offscreen intermediates are float16 (an 8-bit intermediate would clamp it to 1).
    // Below 1 dims the fill; 0 paints black. Unbounded — a sane range is the game's responsibility.
    float fillIntensity = 1.0f;

    // ── Gleam parameters (kind == Gleam) ──
    // A luminance-keyed diagonal sheen band (the marquee "shine") over the pixels the effect covers. The
    // band leans along the axis d = uv.x + uv.y·slant; `sweep` is its centre on that axis and `width` its
    // half-width (both UV units); `gain` is the multiplicative boost at the crest (0 = no effect). The boost
    // multiplies each pixel by its own brightness (plus a white lift scaled by that brightness), so bright
    // content catches the light and dark stays dark. The CPU mirror is retropp::applyGleam.
    float sweep = 0.0f;   // band centre along the slant axis, UV
    float width = 0.1f;   // band half-width, UV (> 0 — the falloff radius)
    float gain  = 0.0f;   // sheen boost at the crest — 0 = identity (the default: no effect)
    float slant = 0.35f;  // diagonal lean: axis = uv.x + uv.y·slant (0 = a vertical band)

    // ── ColorSaturation parameters (kind == ColorSaturation) ──
    // Pull each pixel toward its own luminance — a cross-channel grade ColorFill cannot express (ColorFill is
    // per-channel affine + a mix toward a solid colour; saturation needs each channel drawn toward the pixel's
    // OWN brightness). 255 = full saturation = the exact identity (the default — an unset effect is a no-op);
    // 0 = greyscale; between = partial desaturation. Handed to the developer as a uint8, the same 0..255-maps-
    // to-0..1 surface as an Rgba8 channel — the platform normalizes ÷255 for the shader (saturationParams). The
    // CPU mirror is retropp::applySaturation; the luma weights match Gleam's (one luminance authority).
    std::uint8_t saturation = 255;

    // ── Bloom parameters (kind == Bloom) ──
    // A threshold-blur-add glow: pixels brighter than `threshold` blur outward by `radius` (a separable
    // Gaussian, σ = radius/2, whole-pixel taps clamped at 32 per side) and add back over the source scaled
    // by `intensity` — bright content radiates a soft halo, and the halo lifts coverage so it composites
    // over whatever sits below the effect's container. `radius` is in the site's own pixels: viewport px
    // at the frame / layer / region sites and on a Below-scope sprite lens (scene space), the sprite's OWN
    // art px on a sprite chain step (the amplitude convention) — where it also grows the sprite's render
    // footprint so the halo is never clipped at the static quad. `threshold` is a uint8 luminance floor
    // (0 = every pixel blooms, the same 0..255-maps-to-0..1 surface as an Rgba8 channel); `intensity` is
    // the glow strength (0 = the exact identity — the default renders no glow; 255 = full). The glow adds
    // light without a clamp, so on the float16 chain a hot halo can exceed 1 until the final blit — the
    // fillIntensity headroom behaviour. The CPU mirror is retropp::applyBrightpass + gaussianKernelWeight.
    float        radius    = 0.0f;
    std::uint8_t threshold = 0;
    std::uint8_t intensity = 0;

    // ── Glow parameters (kind == Glow) ──
    // Glow is Bloom's authored-colour sibling: where Bloom radiates the source's OWN light, Glow radiates a
    // colour you PICK. It consults NO fields of its own — it reuses `radius` (the aura reach), `threshold`
    // (the emission floor; 0 = the whole silhouette emits, dark art included), `intensity` (the strength; 0 =
    // the exact identity default), and `fill` × `fillIntensity` (the authored tint — fill_rgb/255 · fillIntensity
    // per channel; > 1 is an HDR-hot aura on the float16 chain, the ColorFill headroom). The blurred emission
    // MASK is scalar (coverage × brightpass survival of the straight luminance), so no source hue enters the
    // added halo — that scalar-times-tint is the line against Bloom. The CPU mirror is retropp::glowMask +
    // gaussianKernelWeight + retropp::applyGlowAdd.

    // ── Swirl parameters (kind == Swirl) ──
    // An angular twist about a centre — a whirlpool. Ripple's angular sibling: where Ripple pushes the
    // sample ALONG the radius (concentric rings), Swirl carries it AROUND the centre. It consults NO fields
    // of its own — it reuses `center` (the swirl centre), `radius` (the disc extent), `amplitude` (the
    // turn) and `phase` (added to `amplitude`, so advancing it spins the vortex). Content within `radius`
    // of `center` rotates by amplitude + phase at the exact centre, easing smoothly to nothing at the rim;
    // content outside the disc is untouched.
    //
    // `amplitude` and `phase` are DEGREES — the platform's angle unit (Transform::rotation takes degrees too),
    // converted to radians below the API boundary. Positive turns the content CLOCKWISE, matching
    // Transform::rotation in the same top-left-origin pixel space. A full turn is 360; a half turn 180.
    // `center` and `radius` are in the site's own pixels: viewport px at the frame / layer / region sites
    // and on a Below-scope sprite lens (scene space), the sprite's OWN art px on a sprite chain step (the
    // Ripple convention) — where `radius` also grows the render footprint so a twisted crest is never
    // clipped at the static quad. amplitude + phase == 0 or radius <= 0 is the exact identity (the default
    // — an unset Swirl is a no-op). `edge` is not consulted: a read that leaves the frame clamps to the
    // border texel, as Ripple's does. The CPU mirror is retropp::swirlParams + retropp::swirlReadPx.

    // ── Per-row data table (a generic effect input) ──
    // An optional array the game fills each frame, one Vec4 per row — a per-scanline value (indexed by the
    // fragment's scanline) or a per-region value (indexed by id); the consumer's shader decides what the
    // index means. It is the table counterpart to the scalar params above: where amplitude / phase are one
    // value, this is an array the effect reads by row. Delivered to the shader as a small data texture it
    // Loads by row (the preamble's paramRow / paramRowAtUv). In v1 only a Custom shader reads it (built-in
    // kinds ignore it); empty (the default) means no table. Game-owned, valid for the renderFrame call —
    // the immediate-mode span contract, like a layer's cells / sprites / palettes.
    std::span<const Vec4> paramTable{};

    // ── Custom-shader parameters (kind == Custom) ──
    // The UNION of every game-authored custom shader's OWN cbuffer params, surfaced here BY NAME (a shader
    // declares `cbuffer Params { float2 pivot; float blend; }`, the build reflects it, and `.pivot`/`.blend`
    // become fields here). A Custom effect sets the ones its shader declares — inline, exactly like a
    // built-in's named params, no per-game uniform struct / byte span / size. The renderer fills the
    // shader's cbuffer from these via that shader's generated packer (it never reads the fields directly,
    // so this generated set never changes the platform's view of the struct). Generated empty when no custom
    // shader is referenced in the build (gen_effect_fields.cmake).
#include "retropp/generated/custom_effect_fields.inc"
};

// ── Region — a shape that owns the effects applied inside it ──────────────────────────────────
//
// Ownership runs shape → effects: a Region binds a SHAPE to the effects applied INSIDE that shape, in
// list order. An effect itself carries no shape; the confinement comes from the Region that owns it (an
// effect with no region covers its whole scope via DrawLayer::effects / FrameDrawState::postEffects). A
// layer (DrawLayer::regions) and the frame (FrameDrawState::regions) own a list of regions; one region
// can carry several effects (a Transparency that makes a shape see-through AND a Ripple that fills the same
// shape), and one effect drops into many regions with no duplication. Confine effects to the OUTSIDE of a
// shape with ShapePoints::inverted(). The same SDF gate the platform already had does the confinement, per
// effect. Regions are OPTIONAL and ADDITIVE — the whole-reach effects / postEffects paths are unchanged, so
// a frame that uses neither composites through the whole-reach paths alone. An empty `effects` list
// is a no-op region.
struct Region {
    ObjectKey                      key;      // required reconciliation identity — first member. Regions are not
                                             // interpolated, but they carry a key like every other drawable so
                                             // the identity model is uniform across layers, sprites, and regions.
    ShapePoints                    shape;    // the confinement (viewport pixels); shape.inverted() = outside
    std::vector<ScreenSpaceEffect> effects;  // applied inside `shape`, in list order
    float                          alpha = 1.0f;  // opacity of this region's effects over the scene, [0,1]; 1 = full
    BlendMode                      blend = BlendMode::Normal;  // how its effects combine over the scene; Normal = alpha-over
};

// ── stencil() — the "make a shape see-through" helper ─────────────────────────────────────────
//
// Build the Region(s) that make `shape` see-through and (optionally) run effects on each side. A free
// helper, no platform state: it expands to the equivalent Region model and the renderer treats the result
// like any other regions. Push it onto a layer's or the frame's `regions` (replace or append):
//   layer.regions  = stencil(ShapePoints::circle({80, 72}, 30));               // a plain hole in this layer
//   frame.regions  = stencil(shape, StencilMode::TransparentOutside, /*feather=*/8); // a soft frame-wide porthole
//
// `mode` picks which side goes see-through (TransparentInside = the inside → a hole the layers below show through;
// TransparentOutside = the outside → a porthole keeping only the inside). `feather` softens the boundary (0 = hard).
// `scope` is the Transparency's reach: Layer (default) turns only THIS layer see-through (reveal the layers
// at lower z); Below turns this layer AND everything beneath it see-through (reveal the backdrop). `insideRegion`
// / `outsideRegion` are effects confined to the shape's inside / its outside — they run at Below scope on the
// composited scene, so they distort what shows THROUGH the see-through area, not just the (transparent) layer.
[[nodiscard]] inline std::vector<Region>
stencil(ShapePoints shape,
        StencilMode mode = StencilMode::TransparentInside,
        float feather = 0.0f,
        ScreenSpaceEffectScope scope = ScreenSpaceEffectScope::Layer,
        std::vector<ScreenSpaceEffect> insideRegion  = {},
        std::vector<ScreenSpaceEffect> outsideRegion = {}) {
    std::vector<Region> regions;
    // The see-through: a Transparency confined to `shape`, at the caller's scope. The keys are fixed
    // literals — regions are not interpolated, so their keys need only be present, not unique.
    regions.push_back(Region{.key     = "stencil",
                             .shape   = shape,
                             .effects = {ScreenSpaceEffect{.kind    = ScreenSpaceEffectKind::Transparency,
                                                           .scope   = scope,
                                                           .stencil = mode,
                                                           .feather = feather}}});
    // Each side effect: confined to the shape (inside) or its inverse (outside), at Below scope so it
    // resolves on the composited scene the see-through reveals.
    for (ScreenSpaceEffect e : insideRegion) {
        e.scope = ScreenSpaceEffectScope::Below;
        regions.push_back(Region{.key = "stencil.inside", .shape = shape, .effects = {e}});
    }
    for (ScreenSpaceEffect e : outsideRegion) {
        e.scope = ScreenSpaceEffectScope::Below;
        regions.push_back(Region{.key = "stencil.outside", .shape = shape.inverted(), .effects = {e}});
    }
    return regions;
}

// The sprite mask family's return types (defined in sprite_mask.h) — forward-declared so Sprite can
// name them as return types without pulling the mask header into this one (sprite_mask.h includes
// draw_state.h, not the reverse).
struct SpriteMask;
struct FrozenSpriteMask;

// One placed sprite. `x`/`y` place the sprite's ORIGIN in the LAYER's coordinate space (before scroll —
// the vertex shader subtracts the layer scroll, so a sprite on a world-scroll layer tracks the
// background, and a HUD layer at scroll {0,0} stays fixed). `origin` and `pivot` both default to {0,0} —
// the quad's top-left — so a sprite that sets neither places by its top-left corner. `tile` is the top-left atlas cell
// (8px grid); the sprite reads a size.width × size.height pixel rectangle from the atlas at that
// cell's pixel origin (so a 16×16 sprite spans a 2×2 cell block laid out contiguously). `atlas` names
// the sprite's own sheet and `palette` the palette it colours through — both directly, per-sprite.
// Identity is the named fields — no packed attribute byte.
//
// `origin` and `pivot` are two QUAD-space points doing two jobs: `x/y` place the `origin` (the
// placement handle), and `transform` spins about the `pivot` (the transform centre). A local quad point
// p lands at (x, y) + (pivot − origin) + transform·(p − pivot). At identity this cancels to
// (x, y) + (p − origin) — the pivot drops out, so a pivot change never moves an untransformed sprite;
// under any origin-fixing transform (the plain rotation(θ) / scale / skew forms) the pivot's own image
// is (x, y) + (pivot − origin), invariant of the angle (a transform carrying its own translation or a
// baked-pivot term adds that displacement, as authored). Set the two to the SAME point to attach: with
// origin = pivot = a mount anchor, that point sits at (x, y) AND the sprite spins about it — the
// placement handle and the hinge coincide, which is what a joint is. Both are QUAD-space: texture ops
// never move them (see orientPoint); set either from art via anchor(k, Space::Quad) or center(Space::Quad).
// A transform INPUT is always read in Quad space — every Space::Layer value is computed THROUGH pivot and
// origin (toLayer), so feeding a Layer value back into this sprite's pivot/origin is circular. Layer-space
// queries are for consumers (a sibling pinning to this sprite's anchor, a path, an effect focus).
//
// `transform` is the sprite's own geometric transform, applied in SPRITE-LOCAL pixel space — the
// [0, size.width] × [0, size.height] rectangle of the sprite's own art — about `pivot` (a pivot baked
// into the matrix by a named constructor still composes within the transform itself; the `pivot` field
// is the spin centre `x/y` do NOT move — placement is `origin`). It composes with the layer's own DrawLayer::transform: a sprite
// quad goes sprite.transform first, then the layer transform, exactly as a tile layer's content does.
// The identity default is a no-op (a plain axis-aligned quad). `flipX`/`flipY` and `rotation` are
// TEXTURE ops (which source pixel is read), independent of the geometry: `rotation` rotates the art in
// 90° steps and composes with the flips for all eight orientations of square art. The geometric
// `transform` is the separate path for arbitrary-angle quad rotation. A sprite can carry both — its art
// reoriented by rotation+flips and its quad warped by transform.
//
// `z` stacks sprites WITHIN their layer, back-to-front ascending — the within-layer sibling of
// DrawLayer::z, with one deliberate asymmetry: sprite z is NOT unique. Any values are legal; equal-z
// sprites keep submission order (the sort is stable). A top-down Y-sort writes the feet Y straight in.
// Discrete, like the flips — it snaps to the current submission, never eases.
//
// `anchors` is the sprite's published points — a game-owned span (the immediate-mode contract, like a
// layer's cells/sprites; a static constexpr table just works), valid for the queries made against it.
// anchor(k, Space::Quad) answers in QUAD space (orientation applied — where the art feature sits on the
// placed quad); anchor(k, Space::Layer) answers in the LAYER's space (through transform + placement — the
// space x/y live in, what a same-layer sibling consumes). Both spaces address by label or index and THROW
// std::out_of_range on a miss (a label fails loudly). Cross-layer consumers map between layer spaces
// themselves — the sprite value knows nothing of its layer's scroll or transform, by design.
struct Sprite {
    ObjectKey       key;                  // required reconciliation identity — first member; the stable
                                          // developer-supplied name the interpolator matches this sprite to
                                          // its previous tick state by, unique within a frame across ALL sprite
                                          // layers (the interpolator holds one sprite map for the whole frame).
    int             x       = 0;
    int             y       = 0;
    std::int32_t    z       = 0;       // within-layer stacking key — ascending draws back-to-front;
                                       // NON-unique (ties keep submission order); snaps, never eases
    AssetDimensions size    = AssetDimensions::GameBoy8x8;
    AtlasId         atlas{};           // which uploaded sheet this sprite draws from
    std::uint16_t   tile    = 0;       // top-left atlas cell within `atlas`
    PaletteId       palette{};         // which uploaded palette colours it
    float           alpha   = 1.0f;    // per-sprite opacity [0,1], default opaque. Composes MULTIPLICATIVELY under
                                       // the layer: effective = palette α × this α × layer α (the layer is the outer
                                       // envelope — a sprite can make itself more transparent than its layer, never
                                       // more opaque). Eased by the interpolator like DrawLayer::alpha. It is opacity,
                                       // not a hole: 0 renders nothing visible but discards nothing (only a material-0
                                       // palette entry is a structural hole).
    BlendMode     blend     = BlendMode::Normal;  // how this sprite composites over its container's image — the
                                       // container pair beside `alpha` (alpha = HOW MUCH the sprite contributes,
                                       // blend = HOW), completing the container grammar every other surface already
                                       // speaks. Normal is the byte-identical alpha-over default; a non-Normal sprite
                                       // grades against its COMPOSITING CONTAINER's accumulated image at draw time
                                       // (applyBlendMode, the single authority): a Multiply shadow decal darkens the
                                       // scene beneath, an Add flare lifts it. The container is whatever this sprite
                                       // layer draws INTO — for a sprite in a plain (direct-to-accumulator) layer that
                                       // is the scene beneath plus this layer's earlier-z content already drawn; for a
                                       // sprite in an ISOLATED layer (one being scratch-rendered for its own effects or
                                       // blend) it is the layer's own scratch — within-layer content only, so a
                                       // non-Normal sprite over the layer's transparent scratch has nothing to grade
                                       // against. Discrete like the flips / z / rotation — it snaps to the submission,
                                       // never eases (a game eases toward a blend by easing `alpha` via Tween, or by
                                       // resubmitting, not by interpolating the mode).
    bool          flipX     = false;
    bool          flipY     = false;
    Rotation      rotation  = Rotation::None;  // 90° texture rotation; composes with the flips
    Transform     transform{};         // per-sprite geometric transform, sprite-local space, about `pivot`
    Point         pivot{};             // transform centre — the point `transform` spins about, QUAD-space px; {0,0} = top-left
    Point         origin{};            // placement handle — the QUAD-space point `x/y` place, QUAD-space px; {0,0} = top-left
    std::span<const Anchor> anchors;   // published art-space points; game-owned (empty = none)

    // The sprite as an effect CARRIER — the same container grammar a DrawLayer / Region / the frame
    // speaks. `effects` is a whole-silhouette effect chain over the sprite's OWN visible pixels; `regions`
    // is a list of confined, containered effects. Both default empty; a sprite that sets neither carries no
    // effect and composites as a plain sprite (byte-identical to one with no effect fields). The effect
    // domain is the SPRITE, not the atlas texture behind it: the art sits in an infinite transparent field,
    // so a displacing effect that pulls
    // the art aside exposes transparency (the layers below show through), never black and never a smeared
    // edge; a displacing effect inflates the sprite's render footprint by its displacement so a wobble
    // crest is never clipped at the static quad.
    //
    // `effects` applies FIRST, in list order, to the sprite's own pixel: ColorFill paints, Gleam adds a
    // sheen, ColorSaturation desaturates, Transparency punches a shape-hole, RowDisplacement / Ripple re-read the art at a displaced
    // within-sprite position (out-of-art reads are transparent under the default Blank edge, or clamp to the
    // art border under Stretch). Displacing kinds accumulate their within-art displacement and resolve it
    // before the art is read; the colour kinds then apply to the read colour in order — so a colour effect and
    // a displacing effect compose cleanly in one pass. A displacing effect's `amplitude` and `center` are in
    // the sprite's OWN art pixels (the re-read space), not the viewport pixels they mean on a layer; a sprite
    // that displaces its art renders on the crisp viewport grid (like a transformed sprite).
    //
    // An effect whose `scope` is Below turns the sprite into a refraction lens: it does not transform the
    // sprite's own pixels but distorts / grades the composited SCENE beneath the sprite's layer, confined to
    // the silhouette (the art's alpha coverage). A Below sprite draws no art of its own — the art is purely
    // the coverage mask, so its alpha sets the lens strength (an opaque mask fully replaces the scene on the
    // silhouette; a partial-alpha mask blends the distortion with the original scene). A Below displacement's
    // amplitude / centre are VIEWPORT px (it distorts the scene), where a Layer displacement reads them as the
    // sprite's own art px. Every effect kind is first-class at Below scope: ColorFill / Gleam / ColorSaturation
    // / RowDisplacement / Ripple grade or distort the scene whole-silhouette, a Below Transparency scales the lens strength
    // (blending the grade back toward the untouched scene), a Below Custom runs a game shader over the scene
    // through the silhouette, and a Below-scope region grades the scene over its shape ∩ the silhouette.
    // Layer-scope effects on a lens are ignored (the renderer logs it — the art doesn't draw). For a sprite
    // that shows art AND lenses the scene, use two sprites.
    //
    // `regions` applies AFTER `effects`, in list order (matching the layer's effects-then-regions order).
    // Each Region confines its own effects to its `shape` INTERSECTED with the sprite's silhouette and
    // grades them over the sprite's pixel by the Region's own `alpha` / `blend`. The shape is read in the
    // sprite's QUAD space (the pivot / origin / anchor space, art-pixel units), evaluated
    // pre-transform so it rides the sprite's transform exactly as the art does; an empty shape covers the
    // whole silhouette (flash the whole sprite), a circle / capsule / polygon confines to part of it
    // (flash only the bridge). Region keys are required like every drawable's, but a sprite-carried region
    // is not interpolated (uniform with layer / frame regions). Effect PARAMETERS are per-tick data on
    // both lists — the interpolator never eases them; a game eases via Tween and resubmits.
    std::vector<ScreenSpaceEffect> effects;  // whole-silhouette effect chain over the sprite's own pixels
    std::vector<Region>            regions;  // confined + containered effects; QUAD-space shape ∩ silhouette

    // An anchor's position, in the space you ask for. Space::Quad answers on the placed QUAD —
    // orientation ops applied (a flipped leg's socket mirrors with the leg), before transform/placement.
    // That is the transform-input space, so it is the bridge from an art feature to a pivot/origin:
    //   claw.pivot = claw.anchor("hinge", Space::Quad);
    // Space::Layer answers in the LAYER's coordinate space — the point mapped through this sprite's
    // transform + placement ((x, y) + (pivot − origin) + transform·(quad point − pivot), perspective
    // divide included). That value other same-layer sprites consume
    // ("forearm.x = upperArm.anchor(ELBOW, Space::Layer)") and a Curve / PathWalker / tween / emitter
    // pins to. A pure resolver on the sprite's own fields — it never sees the layer's scroll or transform;
    // cross-layer consumers compose that themselves.
    // Throws std::out_of_range on an unknown label / out-of-range index — an anchor address fails loudly.
    // A duplicated label resolves to the FIRST match (findDuplicateAnchorLabel is the static_assert seam).
    [[nodiscard]] constexpr Point anchor(std::string_view label, Space space) const {
        for (const Anchor& a : anchors) {
            if (a.label == label) {
                const Point q = orientPoint(Point{a.x, a.y}, size.width, size.height, rotation, flipX, flipY);
                return space == Space::Quad ? q : toLayer(q);
            }
        }
        throw std::out_of_range("anchor: no anchor labeled \"" + std::string(label) + "\"");
    }
    [[nodiscard]] constexpr Point anchor(std::size_t index, Space space) const {
        if (index >= anchors.size()) {
            throw std::out_of_range("anchor: anchor index " + std::to_string(index) +
                                    " out of range (sprite has " + std::to_string(anchors.size()) +
                                    " anchors)");
        }
        const Anchor& a = anchors[index];
        const Point q = orientPoint(Point{a.x, a.y}, size.width, size.height, rotation, flipX, flipY);
        return space == Space::Quad ? q : toLayer(q);
    }

    // Map any QUAD-space point through this sprite's transform + placement into the LAYER's space —
    // the geometric chain makeGpuSprite bakes, applied to one point:
    // dest = (x, y) + (pivot − origin) + transform·(p − pivot). At identity this cancels to
    // (x, y) + (p − origin) — the pivot drops out, so a pivot change never moves an untransformed sprite.
    [[nodiscard]] constexpr Point toLayer(Point p) const noexcept {
        const float lx = p.x - pivot.x;
        const float ly = p.y - pivot.y;
        const float px = pivot.x - origin.x;
        const float py = pivot.y - origin.y;
        return Point{static_cast<float>(x) + px + transform.applyX(lx, ly),
                     static_cast<float>(y) + py + transform.applyY(lx, ly)};
    }

    // The centre of the sprite, in the space you ask for. Space::Quad = {size.width / 2, size.height / 2},
    // the raw art midpoint — the transform-input space, so this is the one you place and spin by:
    //   s.origin = s.center(Space::Quad);   // place the sprite by its middle
    //   s.pivot  = s.center(Space::Quad);   // spin it about its middle
    // Space::Layer = that midpoint mapped through transform + placement (toLayer) — the centre of the
    // sprite AS DRAWN, what a consumer reads to sit something at the sprite's visible centre. Feeding a
    // Space::Layer centre back into origin/pivot is circular: the Layer centre is computed through them.
    [[nodiscard]] constexpr Point center(Space space) const noexcept {
        const Point q{static_cast<float>(size.width) * 0.5f, static_cast<float>(size.height) * 0.5f};
        return space == Space::Quad ? q : toLayer(q);
    }

    // The sprite MASK family — the sprite's image as a mask, decoupled from the texture that defined it:
    // test points against it (pixel-exact collision), or take it as geometry to use as a Region. A Sprite
    // always carries a texture, so only the mask can give a textureless shape-of-the-sprite.
    // (To re-draw the ART itself, a second Sprite with the same atlas/tile/transform is
    // the cheap route — one instanced quad; that covers re-drawing and nothing else.) Each form takes a
    // `Space` (Quad = the placed art rectangle before placement; Layer = through transform + placement, the
    // same frames the anchors answer in); the coverage is the sprite's own sheet, resolved from `atlas`
    // against the renderer's uploaded pixels. Every form reads the CURRENT tile — re-query after a frame
    // change. Pure CPU / tick-state (defined in sprite_mask.cpp; they trace/copy, so they are not constexpr):
    //   mask        — a BORROW: the exact mask as a live, non-owning view (frame lifetime; must not outlive
    //                 the sprite). The default — answers contains(point) with one coverage read, O(1).
    //   freezeMask  — OWN it: an exact snapshot detached from the sprite, storable past its lifetime.
    //                 Copies the coverage mask per call — per-frame-per-object use is allocation churn.
    //   maskShape   — OWN the mask as GEOMETRY: a coarse ≤ maxPoints polygon (a real ShapePoints, so it
    //                 drops into a Region / stencil() / physics — the one form that yields a shape).
    //                 Conservative (default) contains the mask; Balanced hugs it tight. maxPoints < 3
    //                 throws. Traces the coverage on every call — cache it, don't re-trace per frame.
    // The cost intuition is inverted: exactness is expensive to DRAW, not to test. mask().contains() is one
    // coverage read; a ShapePoints test is O(vertices), and a DRAWN polygon's vertex count is a per-pixel
    // cost while it is on screen (the region gate carries at most 64 vertices — longer polygons truncate
    // there with a logged warning).
    [[nodiscard]] SpriteMask       mask(Space space) const;
    [[nodiscard]] FrozenSpriteMask freezeMask(Space space) const;
    [[nodiscard]] ShapePoints      maskShape(int maxPoints, Space space,
                                             ShapeTrace trace = ShapeTrace::Conservative) const;
};

// A sprite layer's content: the layer's placed sprites, each naming its own indexed sheet + palette
// directly (so one sprite layer mixes sheets and palettes freely). `sprites` is game-owned; valid for
// the duration of the renderFrame() call. An empty `sprites` span is a valid (degenerate) submission.
struct SpriteContent {
    std::span<const Sprite> sprites;            // the layer's placed sprites
};

// The sprite storage-buffer record the sprite shaders read (one per sprite). std430-style 16-byte
// alignment → 128 bytes, laid out as the vertex shader's
// { float4 row0; float4 row1; float4 row2; float4 inv0; float4 inv1; float4 inv2; uint4 attr; uint4 fx; }:
//   row0/row1/row2 = the nine coefficients (row-major; row1/row2's 4th lane is padding, row0's 4th lane
//          carries the per-sprite alpha — see the alpha note below) of the COMPOSED
//          clip-space FORWARD homography H the vertex stage rasterizes: clip = H · (cx, cy, 1) for a
//          UNIT-quad corner (cx, cy) ∈ {0,1}². H bakes the whole chain CPU-side — unit→sprite-pixel
//          scale, the per-sprite Transform, the scrolled top-left translation, the per-layer Transform,
//          and screen→clip (viewport scale + top-left-origin V-flip) — so the vertex stage stays a pure
//          storage-buffer read with NO uniform. That single-buffer constraint is load-bearing: a vertex
//          stage carrying both a storage buffer AND a uniform buffer collides in Metal's [[buffer]]
//          namespace under the single-pass HLSL→SPIR-V→MSL toolchain (SDL_GPU offsets storage buffers
//          past the uniform buffers, which the toolchain can't express alongside Vulkan's descriptor
//          layout). The bottom row (m20, m21) carries the perspective terms — non-zero ⇒ the per-vertex w
//          varies ⇒ the GPU perspective-divides and interpolates the within-sprite UV perspective-correct
//          for free; zero ⇒ the affine case (w ≡ 1), a plain axis-aligned quad. For a sprite drawn on the
//          crisp (Viewport) grid, H is the true forward map INFLATED by a thin margin (the analytic bit
//          is set — see below); otherwise H is the exact forward map.
//   inv0/inv1/inv2 = the screen→unit INVERSE homography (the true, un-inflated screen-pixel → unit-quad
//          map). The fragment reads it on the analytic branch to decide, per viewport cell, whether the
//          cell centre lies inside the true quad and which sprite texel it reads. Stored for EVERY sprite
//          (the record is uniform and roundtrip-testable) even though only an analytic sprite consults it.
//   attr = (tile, atlasPalette, flags, size): `atlasPalette` packs the sprite's atlas handle (low 16,
//          an AtlasId, indexing the global atlas-region table) and its palette flat offset (high 16, a
//          PaletteId — already the offset). `flags` is packSpriteFlags (flip / rotation / the analytic
//          coverage bit); `size` is the pixel size packed (width<<16)|height for the fragment's
//          within-sprite addressing. The unit-tested CPU↔GPU mirror, same discipline as packTileCell.
//   row0[3] (row0.w) = the per-sprite alpha in [0,1] (Sprite::alpha) — the fragment's third opacity
//          factor (palette α × sprite α × layer α), forwarded out of the vertex stage as a varying. It
//          rides row0's otherwise-padding 4th lane, so the alpha adds zero frame-data growth. ⚠ THIS
//          LANE'S SAFE DEFAULT IS 1.0, NOT 0: in the shader it is opacity, so 0 means
//          fully transparent. makeGpuSprite writes s.alpha here (never the 0 a bare padding lane holds),
//          or every sprite renders invisible. The default-lane unit test pins a default-constructed
//          sprite's lane at exactly 1.0, catching a zero-fill in ctest device-free rather than as a black frame.
struct GpuSprite {
    float         row0[4];        // forward H row 0: m00 m01 m02 _   (unit-quad corner → clip; inflated when analytic)
    float         row1[4];        // forward H row 1: m10 m11 m12 _
    float         row2[4];        // forward H row 2: m20 m21 m22 _   (m20,m21 = perspective; w = m20·x + m21·y + m22)
    float         inv0[4];        // screen→unit inverse row 0: m00 m01 m02 _  (the true, un-inflated map)
    float         inv1[4];        // screen→unit inverse row 1: m10 m11 m12 _
    float         inv2[4];        // screen→unit inverse row 2: m20 m21 m22 _  (perspective; w = m20·x + m21·y + m22)
    std::uint32_t tile;           // top-left atlas cell within the sprite's sheet
    std::uint32_t atlasPalette;   // atlas (low 16, AtlasId) | palette flat offset (high 16, PaletteId)
    std::uint32_t flags;          // bit0 flipX | bit1 flipY | rotation (bits 2..3) | bit4 analytic coverage |
                                  //   bit5 has-displacement (runs the fragment's displacement pre-pass)
    std::uint32_t size;           // pixel size packed (width<<16)|height
    std::uint32_t fxOffset;       // index of this sprite's first SpriteFxRecord in the per-frame sprite-effect
                                  // store; meaningful only when fxCount > 0. makeGpuSprite writes 0; the renderer
                                  // patches it after packing the store (the store is built once the frame's
                                  // sprites are known, so the offset isn't a compile-time property of one sprite).
    std::uint32_t fxCount;        // number of SpriteFxRecords for this sprite (its effects chain + its regions,
                                  // flattened). 0 (the default) ⇒ the fragment takes the no-effect early-out and
                                  // the sprite composites exactly as a plain sprite — the byte-identity guarantee.
    std::uint32_t fxPad0;         // pad to a 16-byte (float4) step so the record is 128 bytes
    std::uint32_t fxPad1;
};
static_assert(sizeof(GpuSprite) == 128);

// The maximum inline quad-space shape vertices one sprite-region step carries. A circle (1) / capsule (2)
// / triangle (3) / rectangle (4) / small polygon fit; a longer polygon is truncated at pack time with a
// logged warning (sprite regions are small local shapes — the eyes, a bridge — not scene-scale outlines).
inline constexpr std::size_t kSpriteRegionMaxPoints = 8;

// One packed sprite-effect step the sprite fragment reads from the per-frame sprite-effect record store.
// A sprite's `effects` chain and its `regions` flatten into a contiguous run of these (addressed by
// GpuSprite.fxOffset / fxCount). Each step is EITHER a whole-silhouette chain effect (isRegion bit clear:
// the kind's colour transform applies to every visible sprite pixel, in list order) OR one region effect
// (isRegion bit set: gated by a quad-space shape ∩ the sprite's silhouette, graded over the sprite's pixel
// by the region's own alpha + blend). 160 bytes, ten 16-byte (float4/uint4) chunks so the std430
// StructuredBuffer layout is unambiguous across every backend; the CPU packer (buildSpriteFxRecords) and
// the sprite fragment mirror this layout exactly (the packTileCell / makeGpuSprite discipline).
struct SpriteFxRecord {
    std::uint32_t kind;        // ScreenSpaceEffectKind
    std::uint32_t flags;       // bit0 isRegion | bit1 invert (region shape outside) | bit2 hasShapeTransform
    std::uint32_t blend;       // BlendMode of the owning region (Normal for a chain step)
    std::uint32_t pointCount;  // region shape vertex count (0 = whole silhouette: a chain step, or an empty-shape region)
    float alpha;               // owning region's container alpha (1.0 for a chain step)
    float radius;              // region shape SDF radius, quad px (0 for a chain step); a Ripple chain step reuses
                               //   this idle lane for its centre X (art px), a Glow chain step for its tint R
    float strokeWidth;         // region shape stroke-band width, quad px (0 = filled); a Ripple chain step reuses
                               //   this idle lane for its centre Y (art px), a Glow chain step for its tint G
    float pad0;                // a Ripple chain step reuses this idle lane for its decay, a Glow chain step for
                               //   its tint B
    float params[4];           // resolved kind params: ColorFill (r,g,b, 0) already ×fillIntensity, normalized;
                               //   Gleam (sweep, width, gain, slant); ColorSaturation (saturation, 0, 0, 0),
                               //   normalized 0..1; Transparency (stencilMode, feather, 0, 0);
                               //   RowDisplacement (amplitude, frequency, phase, axis); Ripple (amplitude,
                               //   frequency, phase, 0); Bloom / Glow (radius art px, threshold, intensity,
                               //   invNorm). Displacing amplitudes/centres are the sprite's own art px.
    float invRow0[4];          // region shape transform INVERSE, row 0 (m00,m01,m02, _); identity for a chain step
    float invRow1[4];          // row 1
    float invRow2[4];          // row 2 (perspective terms in .0/.1/.2)
    float points[kSpriteRegionMaxPoints * 2];  // quad-space vertices (x0,y0,x1,y1,…); lanes past pointCount are 0
};
static_assert(sizeof(SpriteFxRecord) == 160);

// SpriteFxRecord::flags bits (shared by the packer and the sprite fragment).
inline constexpr std::uint32_t kSpriteFxIsRegion   = 1u;  // this step is a confined region effect (else a chain effect)
inline constexpr std::uint32_t kSpriteFxInvert     = 2u;  // region shape is inverted (the OUTSIDE is the region)
inline constexpr std::uint32_t kSpriteFxEdgeStretch = 4u; // displacing chain step: out-of-art read clamps (else transparent)

// The analytic (crisp-coverage) flag — bit 4 of GpuSprite::flags. Set when the sprite renders through
// the fragment's viewport-cell coverage branch (a transformed sprite on the Viewport grid, or any sprite
// that displaces its own art); clear otherwise (untransformed, non-displacing sprites, and every sprite on
// the Output grid, take the plain quad path).
inline constexpr std::uint32_t kSpriteAnalyticFlag = 16u;

// The has-displacement flag — bit 5 of GpuSprite::flags. Set when the sprite carries a displacing chain
// effect (RowDisplacement / Ripple). Only such a sprite runs the fragment's displacement pre-pass; a plain or
// colour-only sprite skips the record scan entirely, keeping its read at the plain coordinate.
inline constexpr std::uint32_t kSpriteHasDisplacementFlag = 32u;

// Bit 6 of GpuSprite::flags is unused.

// Which chain record carries the emission, in bits 8..15 of an EMISSION record's flags word. The renderer
// copies a glowing sprite's art record, ORs the index in, and draws the copy through the sprite-emission
// pipeline: records before that index run as ordinary colour steps feeding the emission, and the record AT
// it resolves the emission itself. The art pipeline masks every field it reads, so these bits are invisible
// to it. Eight bits is a chain of 256 steps — far past what an effect list is ever authored with.
inline constexpr std::uint32_t kSpriteEmissionIndexShift = 8u;
inline constexpr std::uint32_t kSpriteEmissionIndexMask  = 0xFFu;

[[nodiscard]] constexpr std::uint32_t packSpriteFlags(bool flipX, bool flipY,
                                                      Rotation rot = Rotation::None,
                                                      bool analytic = false) noexcept {
    return (flipX ? 1u : 0u) | (flipY ? 2u : 0u) | (static_cast<std::uint32_t>(rot) << 2)
         | (analytic ? kSpriteAnalyticFlag : 0u);
}

// Pack an asset's pixel dimensions into one uint (width in the high 16 bits). The fragment shader
// unpacks this to map the interpolated within-sprite UV back to an atlas pixel.
[[nodiscard]] constexpr std::uint32_t packAssetSize(const AssetDimensions& sz) noexcept {
    return (static_cast<std::uint32_t>(sz.width) << 16) | static_cast<std::uint32_t>(sz.height & 0xFFFF);
}

// Pack a sprite's atlas handle + palette into the GpuSprite::atlasPalette word: atlas in the low 16
// bits (an AtlasId, indexing the global atlas-region table), the palette flat offset in the high 16
// (a PaletteId IS its offset). Mirrors the sprite fragment shader's unpack.
[[nodiscard]] constexpr std::uint32_t packSpriteAtlasPalette(AtlasId atlas, PaletteId palette) noexcept {
    return static_cast<std::uint32_t>(atlas) | (static_cast<std::uint32_t>(palette) << 16);
}

namespace detail {

// Absolute value, constexpr (std::abs is not core-constant until C++23) — used by the inflation bound.
[[nodiscard]] constexpr float absf(float v) noexcept { return v < 0.0f ? -v : v; }

// The unit-space inflation a transformed sprite's forward quad needs so that, when rasterized at output
// resolution, it covers every output pixel whose VIEWPORT-cell centre lies inside the true quad — the
// fragment then trims back to exact coverage. `ok == false` is the degenerate fallback: a corner behind
// the projection (weight ≤ 0), a singular corner Jacobian, or an inflated corner that crosses the horizon
// — the sprite falls back to the smooth quad path.
//
// εu/εv are the max over the four unit corners of the L1 norm of the inverse Jacobian's rows: at each
// corner the screen→unit sensitivity |∂u/∂sx| + |∂u/∂sy| (and the same for v) tells how much unit space to
// grow to cover the screen-space margin `m` in ANY direction. L1 ≥ L2, so the bound is conservative; for
// an affine map it is exact, and under perspective the corner-max plus the 2× margin (m = 1 viewport px,
// while the true need is (S−1)/(2S) < 0.5 px) plus the machine-checked parity gate is the proof.
struct SpriteInflation {
    float eu = 0.0f;
    float ev = 0.0f;
    bool  ok = false;
};

[[nodiscard]] constexpr SpriteInflation spriteInflation(const Transform& S, float m) noexcept {
    const float corners[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    float eu = 0.0f, ev = 0.0f;
    for (const auto& c : corners) {
        const float u = c[0], v = c[1];
        const float w = S.m20 * u + S.m21 * v + S.m22;
        if (w <= 0.0f) return SpriteInflation{};            // corner behind the projection
        const float xn = S.m00 * u + S.m01 * v + S.m02;
        const float yn = S.m10 * u + S.m11 * v + S.m12;
        const float w2 = w * w;
        const float dxdu = (S.m00 * w - xn * S.m20) / w2;
        const float dxdv = (S.m01 * w - xn * S.m21) / w2;
        const float dydu = (S.m10 * w - yn * S.m20) / w2;
        const float dydv = (S.m11 * w - yn * S.m21) / w2;
        const float det  = dxdu * dydv - dxdv * dydu;
        const float ad   = absf(det);
        if (ad < 1e-12f) return SpriteInflation{};          // singular corner Jacobian
        const float euC = (absf(dxdv) + absf(dydv)) / ad;
        const float evC = (absf(dxdu) + absf(dydu)) / ad;
        if (euC > eu) eu = euC;
        if (evC > ev) ev = evC;
    }
    eu *= m;
    ev *= m;
    // The inflated quad must stay in front of the projection on every corner, else its rasterized weight
    // sign flips and the coverage math is undefined — fall back instead.
    const float iu[2] = {-eu, 1.0f + eu};
    const float iv[2] = {-ev, 1.0f + ev};
    for (const float u : iu)
        for (const float v : iv)
            if (S.m20 * u + S.m21 * v + S.m22 <= 0.0f) return SpriteInflation{};
    return SpriteInflation{eu, ev, true};
}

// The largest art-space excursion a sprite's displacing chain effects (RowDisplacement / Ripple / Swirl)
// push the re-read to, per axis, in the sprite's OWN art pixels. RowDisplacement reaches |amplitude| on its
// modulated axis; Ripple reaches |amplitude| on both (radial); Swirl rotates within its disc, so the read
// moves along a chord of at most min(|turn in radians|, 2) * radius on both axes (the chord 2R*sin(t/2)
// never exceeds either tR or 2R). makeGpuSprite grows the sprite's render footprint by
// this so a displaced crest is never clipped at the static quad; {0,0} for a sprite with no displacing
// effect. A Bloom / Glow halo is outside this bound: it rasterizes into the shared emission buffer and
// spreads there, so the art footprint is the whole footprint.
struct SpriteDisplaceBound {
    float u = 0.0f;
    float v = 0.0f;
};

[[nodiscard]] constexpr SpriteDisplaceBound spriteDisplaceBound(const Sprite& s) noexcept {
    SpriteDisplaceBound b;
    for (const ScreenSpaceEffect& e : s.effects) {
        const float a = absf(e.amplitude);
        if (e.kind == ScreenSpaceEffectKind::RowDisplacement) {
            if (e.axis == Axis::Horizontal) b.u = b.u > a ? b.u : a;
            else                            b.v = b.v > a ? b.v : a;
        } else if (e.kind == ScreenSpaceEffectKind::Ripple) {
            b.u = b.u > a ? b.u : a;
            b.v = b.v > a ? b.v : a;
        } else if (e.kind == ScreenSpaceEffectKind::Swirl) {
            constexpr float kDegToRad = 0.017453292519943295f;  // pi / 180
            const float turn  = absf(e.amplitude + e.phase) * kDegToRad;  // the surface is degrees
            const float chord = (turn < 2.0f ? turn : 2.0f) * (e.radius > 0.0f ? e.radius : 0.0f);
            b.u = b.u > chord ? b.u : chord;
            b.v = b.v > chord ? b.v : chord;
        }
    }
    return b;
}

}  // namespace detail

// The linear scale a sprite's composed placement applies to its art: the square root of the absolute
// determinant of the affine part of (per-sprite transform ∘ per-layer transform) — the geometric mean of the
// two axis scales, so a length authored in art pixels times this is the viewport-space length it covers.
// 1 for the identity pair, and 1 for a pure rotation or translation. A Bloom / Glow reach is authored in art
// pixels but blurs in viewport space, so this is the map between them; a non-uniform or perspective
// placement resolves to one isotropic factor, which is what a single separable blur can express.
[[nodiscard]] inline float spriteLinearScale(const Sprite& s, const Transform& layerTransform) noexcept {
    const Transform t   = s.transform.then(layerTransform);
    const float     det = t.m00 * t.m11 - t.m01 * t.m10;
    return std::sqrt(det < 0.0f ? -det : det);
}

// Build the GPU record for one sprite. `viewportW`/`viewportH` are the internal viewport pixel size;
// `x`/`y` the sprite's screen position and `scrollX`/`scrollY` the layer scroll (all in viewport px —
// carried as float so a sub-pixel interpolated position places between whole viewport pixels);
// `layerTransform` the per-layer DrawLayer::transform. `grid` selects the evaluation grid: on the
// Viewport grid (the crisp default) a geometrically-transformed sprite renders through the fragment's
// analytic coverage branch — the forward quad is inflated a thin margin so it covers every output pixel
// whose viewport-cell centre lies in the true quad, and the screen→unit inverse (the inv rows) lets the
// fragment trim to exact per-viewport-cell coverage; on the Output grid (and for any untransformed
// sprite) the plain quad path renders it with smooth sub-pixel placement.
// A sprite whose chain re-reads beyond the sampled texel (a RowDisplacement / Ripple / Swirl displacement)
// always renders through the analytic branch — regardless of grid or transform — because only that branch
// reconstructs the true quad coordinate across the footprint the re-read inflates; the footprint grows by
// the displacement bound (spriteDisplaceBound, in art px → quad units) so a displaced crest is never
// clipped at the static quad.
// The composed clip-space homography is baked here so the vertex shader is a pure storage-buffer read (no
// uniform). Pure + constexpr — the unit-tested CPU↔GPU mirror.
//
// The chain a unit-quad corner (cx, cy) travels, via the constexpr Transform::then():
//   S = scale(w, h)                          // unit corner → sprite-local content pixel
//         .then(translation(−pivot))         // re-anchor: the pivot to (0,0), so transform spins about it
//         .then(s.transform)                 // per-sprite transform — about the pivot by construction
//         .then(translation(sox + pivot − origin))  // origin lands at the scrolled position; the pivot's
//                                            //   image is offset from it by (pivot − origin)
//                                            //   (sox = x − scrollX, soy = y − scrollY)
//         .then(layerTransform)              // per-layer transform, viewport-pixel space
//   H = S.then(screenToClip)                 // + viewport scale + top-left-origin V-flip → the forward map
// A local quad point p therefore lands at position + (pivot − origin) + s.transform·(p − pivot) — the CPU
// mirror is Sprite::toLayer. With the default origin = pivot = {0,0} both the re-anchor translation and
// the (pivot − origin) offset are the exact identity, every float product preserves its operand
// bit-for-bit, and the default composes out of the chain entirely — which the golden captures pin.
// S (the unit→viewport-pixel map) yields the screen→unit inverse (Sinv, the inv rows, stored for every
// sprite). Scroll is subtracted BEFORE the layer transform — matching the tile path — so a tile layer and
// a sprite layer carrying the same Transform line up and share one pivot space. With identity sprite +
// layer transforms H reduces to a plain axis-aligned quad (w ≡ 1) and no inflation applies.
//
// The clip is baked against the VIEWPORT dimensions regardless of the offscreen target's raster
// resolution: clip space is [−1,1], so the rasterizer maps it onto whatever target is bound — a
// larger (output-resolution) target rasterizes the same clip quad onto a finer grid automatically.
// A fractional `x`/`y` therefore shifts the quad by a sub-viewport-pixel amount, which on a target
// scaled S× lands on a different output pixel — smooth motion. Nothing here scales by the compose factor;
// placement granularity lives entirely in the float position + the target resolution.
[[nodiscard]] constexpr GpuSprite makeGpuSprite(const Sprite& s,
                                                int viewportW, int viewportH,
                                                float x, float y,
                                                float scrollX, float scrollY,
                                                const Transform& layerTransform = Transform{},
                                                EvaluationGrid grid = EvaluationGrid::Viewport) noexcept {
    const float vw  = static_cast<float>(viewportW);
    const float vh  = static_cast<float>(viewportH);
    const float sox = x - scrollX;  // screen-space top-left (viewport px; may be fractional)
    const float soy = y - scrollY;

    // screen→clip: x' = sox·(2/vw) − 1,  y' = 1 − soy·(2/vh)  (top-left-origin V-flip).
    const Transform screenToClip{2.0f / vw, 0.0f,       -1.0f,
                                 0.0f,      -2.0f / vh,  1.0f,
                                 0.0f,      0.0f,        1.0f};

    // S: unit-quad corner → viewport pixel (the chain without screen→clip). Its inverse is the exact
    // screen→unit map the fragment's analytic branch consults; stored for every sprite so the record is
    // uniform and roundtrip-testable regardless of path.
    const Transform S =
        Transform::scale(static_cast<float>(s.size.width), static_cast<float>(s.size.height))
            .then(Transform::translation(-s.pivot.x, -s.pivot.y))
            .then(s.transform)
            .then(Transform::translation(sox + s.pivot.x - s.origin.x, soy + s.pivot.y - s.origin.y))
            .then(layerTransform);
    const Transform Sinv = S.inverse();

    // The chain's art-space displacement reach, in quad units — the footprint must grow by this so a
    // displaced crest is never clipped at the static quad. A displacing sprite renders through the analytic
    // branch regardless of transform: only that branch reconstructs the true quad coordinate (via the
    // inverse map) across the inflated footprint. A Bloom / Glow halo is outside this bound: it rasterizes
    // into the shared emission buffer and spreads there.
    const detail::SpriteDisplaceBound db = detail::spriteDisplaceBound(s);
    const float dispEu = s.size.width  > 0 ? db.u / static_cast<float>(s.size.width)  : 0.0f;
    const float dispEv = s.size.height > 0 ? db.v / static_cast<float>(s.size.height) : 0.0f;
    const bool  hasDisp = (db.u > 0.0f || db.v > 0.0f);  // displacement presence — the pre-pass flag (below)
    const bool  hasInflation = dispEu > 0.0f || dispEv > 0.0f;

    // Analytic crisp coverage engages on the Viewport grid for a genuinely transformed sprite (an identity
    // sprite AND layer take the cheap plain path — their sub-pixel placement is already crisp), OR whenever
    // the chain re-reads beyond the sampled texel (a displacement — it needs the analytic reconstruction
    // across the inflated footprint).
    const bool crispTransform = grid == EvaluationGrid::Viewport &&
                                !(s.transform.isIdentity() && layerTransform.isIdentity());
    bool analytic = crispTransform || hasInflation;

    Transform H = S.then(screenToClip);  // the exact forward map (inflated below when analytic)
    if (analytic) {
        float eu = dispEu, ev = dispEv;
        bool  ok = true;
        if (crispTransform) {
            const detail::SpriteInflation infl = detail::spriteInflation(S, 1.0f);  // margin 1.0 viewport px
            ok = infl.ok;
            eu += infl.eu;
            ev += infl.ev;
        }
        if (ok) {
            const Transform unitInflate{1.0f + 2.0f * eu, 0.0f,            -eu,
                                        0.0f,             1.0f + 2.0f * ev, -ev,
                                        0.0f,             0.0f,             1.0f};
            H = unitInflate.then(S).then(screenToClip);
        } else {
            analytic = false;  // degenerate / extreme transform ⇒ the smooth quad path (the reach clips at the quad)
        }
    }

    GpuSprite g{};
    g.row0[0] = H.m00; g.row0[1] = H.m01; g.row0[2] = H.m02; g.row0[3] = s.alpha;  // row0.w carries the per-sprite alpha
    g.row1[0] = H.m10; g.row1[1] = H.m11; g.row1[2] = H.m12; g.row1[3] = 0.0f;
    g.row2[0] = H.m20; g.row2[1] = H.m21; g.row2[2] = H.m22; g.row2[3] = 0.0f;
    g.inv0[0] = Sinv.m00; g.inv0[1] = Sinv.m01; g.inv0[2] = Sinv.m02; g.inv0[3] = 0.0f;
    g.inv1[0] = Sinv.m10; g.inv1[1] = Sinv.m11; g.inv1[2] = Sinv.m12; g.inv1[3] = 0.0f;
    g.inv2[0] = Sinv.m20; g.inv2[1] = Sinv.m21; g.inv2[2] = Sinv.m22; g.inv2[3] = 0.0f;
    g.tile         = s.tile;
    g.atlasPalette = packSpriteAtlasPalette(s.atlas, s.palette);
    g.flags        = packSpriteFlags(s.flipX, s.flipY, s.rotation, analytic);
    if (hasDisp)          g.flags |= kSpriteHasDisplacementFlag;  // gates the fragment's displacement pre-pass
    g.size         = packAssetSize(s.size);
    g.fxOffset = 0;  // no effect by default; the renderer patches offset+count for a sprite that carries effects
    g.fxCount  = 0;  // (the store isn't a property of one sprite in isolation — it is packed once per frame)
    return g;
}

// The sprite's own integer position (Sprite::x/y) as the placement — the non-interpolated path (and
// the test path). Forwards to the float-position overload above; identical output to placing at the
// sprite's whole-pixel position.
[[nodiscard]] constexpr GpuSprite makeGpuSprite(const Sprite& s,
                                                int viewportW, int viewportH,
                                                float scrollX, float scrollY,
                                                const Transform& layerTransform = Transform{},
                                                EvaluationGrid grid = EvaluationGrid::Viewport) noexcept {
    return makeGpuSprite(s, viewportW, viewportH,
                         static_cast<float>(s.x), static_cast<float>(s.y),
                         scrollX, scrollY, layerTransform, grid);
}

// The coverage decision for one fragment of a sprite on the analytic (Viewport-grid) branch — the CPU
// mirror of the sprite fragment shader, the makeGpuSprite / packTileCell mirror discipline. Given the
// sprite's screen→unit inverse (the GpuSprite inv rows), a fragment's VIEWPORT-space position, and the
// sprite pixel size, it snaps to the viewport-cell centre, maps that through the inverse (perspective
// divide; a weight ≤ 0 is behind the projection ⇒ not covered), applies the half-open coverage test
// 0 ≤ u,v < 1, and reads the cell-centre texel. `covered == false` ⇒ the fragment discards (px/py are
// meaningless). Not constexpr: std::floor is not core-constant until C++23 (the snapFragToCellCenter
// concession).
struct SpriteCellSample {
    bool covered = false;
    int  px      = 0;
    int  py      = 0;
    [[nodiscard]] constexpr bool operator==(const SpriteCellSample&) const noexcept = default;
};

[[nodiscard]] inline SpriteCellSample
sampleSpriteCell(const Transform& inverse, float fragViewportX, float fragViewportY,
                 int width, int height) noexcept {
    const float cx = std::floor(fragViewportX) + 0.5f;   // viewport-cell centre
    const float cy = std::floor(fragViewportY) + 0.5f;
    const float cw = inverse.m20 * cx + inverse.m21 * cy + inverse.m22;
    if (cw <= 0.0f) return SpriteCellSample{};           // behind the projection
    const float u = (inverse.m00 * cx + inverse.m01 * cy + inverse.m02) / cw;
    const float v = (inverse.m10 * cx + inverse.m11 * cy + inverse.m12) / cw;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return SpriteCellSample{};  // outside the true quad
    int px = static_cast<int>(std::floor(u * static_cast<float>(width)));
    int py = static_cast<int>(std::floor(v * static_cast<float>(height)));
    px = px < 0 ? 0 : (px > width  - 1 ? width  - 1 : px);   // clamp the trailing edge
    py = py < 0 ? 0 : (py > height - 1 ? height - 1 : py);
    return SpriteCellSample{true, px, py};
}

// A layer carries exactly one content alternative. The active alternative is the variant's
// identity; LayerContentKind mirrors it for explicit, switch-friendly dispatch.
enum class LayerContentKind : std::uint8_t { Tiles, Sprites, GuestFrame };
using LayerContent = std::variant<TileContent, SpriteContent, GuestFrameContent>;
[[nodiscard]] constexpr LayerContentKind contentKind(const LayerContent& c) noexcept {
    // One kind per alternative, in the variant's own order. An alternative added to LayerContent with no
    // kind beside it here is a compile error, rather than a layer reported — and so drawn — as whichever
    // alternative sits next to it.
    constexpr LayerContentKind kinds[]{
        LayerContentKind::Tiles,
        LayerContentKind::Sprites,
        LayerContentKind::GuestFrame,
    };
    static_assert(std::variant_size_v<LayerContent> == sizeof(kinds) / sizeof(kinds[0]),
                  "every LayerContent alternative declares its LayerContentKind");
    return kinds[c.index()];
}

// ── Layer + frame ─────────────────────────────────────────────────────────────────────

struct LayerScroll {
    int x = 0;
    int y = 0;
    [[nodiscard]] constexpr bool operator==(const LayerScroll&) const noexcept = default;
};

// One layer in the frame's arbitrary, Z-sorted stack. No semantic role is imposed by the
// platform. Runtime-dynamic: every field is fresh each frame.
struct DrawLayer {
    ObjectKey         key;                  // required reconciliation identity — first member; the stable
                                            // developer-supplied name the interpolator matches this layer to its
                                            // previous tick state by, AND the unique-within-a-frame name the
                                            // collision policy enforces. Not a depth key — z alone orders.
    std::int32_t      z = 0;                // back-to-front sort key; unique within a frame
    PixelSize         size{};               // independent per-layer dimensions
    LayerScroll       scroll{};             // independent scroll offset
    float             alpha = 1.0f;         // [0,1], default opaque
    BlendMode         blend = BlendMode::Normal;  // how this layer composites over the accumulator; Normal = alpha-over
    LayerContent      content{ TileContent{} };
    std::vector<ScreenSpaceEffect> effects; // per-layer WHOLE-REACH effect chain (no shape); empty = none
    std::vector<Region> regions;            // per-layer confined effects; each region's effects fill its shape (optional)
    Transform         transform{};          // per-layer geometric transform (scale/rotate/skew/perspective); identity default
    DisplacementEdge  transformEdge = DisplacementEdge::Blank;  // exposed-footprint policy: Blank reveals below / Stretch clamps
    // How many ticks pass between one advance of this layer's world and the next. Unset takes the frame's
    // `advancesEvery`. A layer whose state changes every other tick declares 2 and the renderer eases its
    // motion across two ticks, which also places the drawn position two ticks behind the simulation
    // instead of one. Sprites in the layer take the layer's declaration. A declared 0 reads as 1.
    std::optional<std::uint32_t> advancesEvery{};
};

// The whole frame's draw state. The game clears() + refills `layers` each frame (clear()
// preserves capacity → arbitrary N with no steady-state heap churn). This is RUNTIME platform
// state, not ROM data — the BoundedVec fixed-cap idiom does not apply.
struct FrameDrawState {
    std::vector<DrawLayer>         layers;           // arbitrary N; compositor stable-sorts by z
    // How the frame's WHOLE-FRAME postEffects / regions combine over the composited image — the container
    // blend mode beside `Region::blend` and `DrawLayer::blend`. Normal = the alpha-over default; the other
    // modes apply applyBlendMode. (Whole-frame colour — day/night, fades, flash, tints — is an effect:
    // a ColorFill region with the blend mode and alpha the look wants; the frame carries no bespoke
    // colour member.)
    BlendMode                      blend = BlendMode::Normal;
    std::vector<ScreenSpaceEffect> postEffects;      // frame-level WHOLE-FRAME effects on the composited image
    std::vector<Region>            regions;          // frame-level confined effects; each region's effects fill its shape (optional)
    // How many ticks pass between one advance of the world and the next, for every layer that declares no
    // `advancesEvery` of its own. 1 — a tick advances the world — is the ordinary case. A game whose whole
    // simulation runs on a slower divider declares it once here rather than on every layer.
    std::uint32_t                  advancesEvery = 1;
};

// ── Pure helpers (headlessly unit-tested) ─────────────────────────────────────────────

[[nodiscard]] constexpr float clampAlpha(float a) noexcept {
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

// A texel coordinate in the flat palette store. The store is an arbitrary-size flat array
// of colours wrapped `storeWidth` wide into a 2-D texture.
struct PaletteTexel {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    [[nodiscard]] constexpr bool operator==(const PaletteTexel&) const noexcept = default;
};

// CPU mirror of the tile + sprite fragment shaders' palette lookup: a palette's flat offset plus a
// colour index give a flat position into the store, which wraps to the texel (flat % W, flat / W).
// The store being flat (not a row-per-palette) is what makes palettes arbitrary size — a palette may
// straddle rows, and the flat space is bounded only by the store texture's capacity. Pure + constexpr
// so the flat→2-D addressing stays identical to the shaders (the packTileCell / sampleTilemap mirror
// discipline). Precondition: storeWidth > 0.
[[nodiscard]] constexpr PaletteTexel paletteStoreTexel(std::uint32_t paletteOffset,
                                                       std::uint32_t colorIndex,
                                                       std::uint32_t storeWidth) noexcept {
    const std::uint32_t flat = paletteOffset + colorIndex;
    return PaletteTexel{flat % storeWidth, flat / storeWidth};
}

// --- Layer-key collision detection (compile-time-capable) ---

// A detected violation of layer-key rules within one frame. Two distinct layers MUST NOT share a z (their
// front-to-back order would be undefined) nor a key (the reconciliation identity must be unambiguous), and
// no layer's key may be empty (a key must be present AND meaningful — it is a required identity, not a
// silent blank). `kind` is the identity — first member.
struct LayerKeyCollision {
    enum class Kind : std::uint8_t { DuplicateZ, DuplicateKey, EmptyKey };
    Kind             kind;    // which invariant was violated — identity, first member
    std::string_view first;   // DuplicateZ: the earlier layer's key; DuplicateKey/EmptyKey: the offending key
    std::string_view second;  // DuplicateZ: the later layer's key;   DuplicateKey: == first; EmptyKey: == first
    std::int32_t     z;       // DuplicateZ: the shared z;             DuplicateKey/EmptyKey: that layer's z
};

// Scan a layer set for the first key violation (empty key, duplicate key, OR duplicate z). constexpr and
// pure, so a static_assert over a compile-time-known layer set turns a fixed layer stack's violation into
// a BUILD error — caught before the game ever runs. Returns nullopt when every key is present, meaningful,
// and unique and every z is unique. O(n²); the layer population is small (compositing planes, not
// per-sprite primitives).
[[nodiscard]] constexpr std::optional<LayerKeyCollision>
findLayerKeyCollision(std::span<const DrawLayer> layers) noexcept {
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const std::string_view ki = layers[i].key;
        if (ki.empty()) {
            return LayerKeyCollision{LayerKeyCollision::Kind::EmptyKey, ki, ki, layers[i].z};
        }
        for (std::size_t j = i + 1; j < layers.size(); ++j) {
            if (layers[i].key == layers[j].key) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateKey,
                                         layers[i].key, layers[j].key, layers[i].z};
            }
            if (layers[i].z == layers[j].z) {
                return LayerKeyCollision{LayerKeyCollision::Kind::DuplicateZ,
                                         layers[i].key, layers[j].key, layers[i].z};
            }
        }
    }
    return std::nullopt;
}

// True when every layer key is present + meaningful + unique and every z is unique. constexpr — the
// static_assert seam:
//   static_assert(layerKeysAreUnique(kMyFixedLayers), "z/key collision in layer stack");
// gives compile-time detection for any layer set known at compile time.
[[nodiscard]] constexpr bool layerKeysAreUnique(std::span<const DrawLayer> layers) noexcept {
    return !findLayerKeyCollision(layers).has_value();
}

// --- Runtime draw order + collision policy ---

// How layerDrawOrder() reacts to a key collision detected at RUNTIME. Orthogonal to the
// compile-time static_assert path (layerKeysAreUnique), which is always available.
//   Throw          — throw std::invalid_argument naming the colliding keys (fail fast; the
//                    development default, so a mistake surfaces the instant its frame runs).
//   WarnAndResolve — log a warning naming the colliding keys, then return the deterministic
//                    z → submission order anyway (a shipped game stays up).
enum class LayerKeyCollisionPolicy : std::uint8_t { Throw, WarnAndResolve };

// Default runtime policy, derived from build config: dev builds fail fast; release builds
// keep a shipped game running. Overridable at the call site / on the Renderer (the toggle).
inline constexpr LayerKeyCollisionPolicy kDefaultCollisionPolicy =
#ifdef NDEBUG
    LayerKeyCollisionPolicy::WarnAndResolve;
#else
    LayerKeyCollisionPolicy::Throw;
#endif

// Back-to-front draw order as indices into `layers`: ascending z (the sole depth key — the id is
// a pure label with no ordering role), equal z falling back to submission order (stable). Returns
// indices so the caller composites without copying. Validates key uniqueness first and reacts per
// `policy` (see above); under WarnAndResolve the returned order is still fully deterministic.
// Throws std::invalid_argument on a collision under Throw.
[[nodiscard]] std::vector<std::size_t>
layerDrawOrder(std::span<const DrawLayer> layers,
               LayerKeyCollisionPolicy policy = kDefaultCollisionPolicy);

// --- Within-layer sprite draw order ---

// Back-to-front draw order as indices into `sprites`: ascending Sprite::z, equal z keeping submission
// order (stable). The within-layer sibling of layerDrawOrder, with the deliberate asymmetry that sprite
// z is NON-unique — any values are legal, so there is no collision policy and nothing throws. The
// renderer emits GPU records in this order (records draw in instance order), which is what puts a hand
// in front of its arm regardless of the order the game's chain math produced them in.
[[nodiscard]] std::vector<std::size_t> spriteDrawOrder(std::span<const Sprite> sprites);

// --- Sprite blend runs (container completion) ---

// A contiguous run of same-(blend, pipeline) sprites within a sprite layer's DRAW ORDER. `start`/`count`
// index into the spriteDrawOrder() sequence (positions in that ordered list, NOT indices into the raw sprites
// span); `blend` is the run's shared mode; `pipelineKey` is the run's shared sprite pipeline (0 = the stock
// sprite pipeline; a positive value selects a custom sprite-inline pipeline the renderer assigns per sprite).
// The renderer draws each run from its own records slice with the run's pipeline — a Normal, stock-pipeline
// run composites straight into the container (the byte-identical instanced draw), a non-Normal run renders
// isolated and grades onto the container via applyBlendMode. Runs appear in draw order and each run's members
// keep their draw-order sequence, so within-layer z is exact across the split.
struct SpriteBlendRun {
    std::size_t start;            // first position in the ordered sequence this run covers
    std::size_t count;            // number of consecutive same-(blend, pipeline) sprites (≥ 1)
    BlendMode   blend;            // the mode shared by every sprite in the run
    int         pipelineKey = 0;  // 0 = the stock sprite pipeline; > 0 = a custom sprite-inline pipeline
    [[nodiscard]] constexpr bool operator==(const SpriteBlendRun&) const noexcept = default;
};

// Partition a sprite layer's draw-ordered sprites into contiguous runs sharing both a blend mode AND a
// pipeline. `order` is the spriteDrawOrder() output for `sprites`; `pipelineKeys` (optional) is a per-ORDER-
// position pipeline key (parallel to `order`) — empty means every sprite is key 0 (the stock pipeline), the
// exact same partition as blend alone. Adjacent ordered sprites sharing both keys coalesce into one run. An
// all-Normal, all-stock layer (the overwhelming default) yields exactly ONE run spanning everything — the
// fast path the renderer detects with `runs.size() == 1 && runs[0].blend == Normal && runs[0].pipelineKey ==
// 0` to keep the single instanced draw untouched. Empty `order` yields no runs. `order` and `sprites` are the
// same-frame pair; out-of-range order entries are undefined (the renderer always passes spriteDrawOrder's own
// output).
[[nodiscard]] std::vector<SpriteBlendRun>
spriteBlendRuns(std::span<const Sprite> sprites, std::span<const std::size_t> order,
                std::span<const int> pipelineKeys = {});

// --- Sprite-key collision detection ---

// A detected violation of sprite-key rules within one frame. The interpolator holds ONE sprite map across
// every sprite layer, so two sprites that share a key reconcile to the same slot (last wins), and an empty
// key is never a valid identity. `kind` is the identity — first member.
struct SpriteKeyCollision {
    enum class Kind : std::uint8_t { DuplicateKey, EmptyKey };
    Kind             kind;    // which rule was violated — identity, first member
    std::string_view first;   // DuplicateKey: the shared key; EmptyKey: the empty key (== "")
    std::string_view second;  // DuplicateKey: == first;       EmptyKey: == first
};

// Scan every sprite across every SPRITES layer for the first key violation (empty key OR a key already used
// by an earlier sprite this frame). O(n) via a hash set — the sprite population can be large, unlike the
// small-N layer scan. Returns nullopt when every sprite key is present, meaningful, and unique frame-wide.
[[nodiscard]] std::optional<SpriteKeyCollision>
findSpriteKeyCollision(std::span<const DrawLayer> layers);

// Validate sprite-key uniqueness frame-wide, reacting per `policy` (Throw = throw std::invalid_argument
// naming the offending key; WarnAndResolve = log a warning and continue). The runtime sibling of
// layerDrawOrder's layer-key validation — called once per frame beside it.
void validateSpriteKeys(std::span<const DrawLayer> layers,
                        LayerKeyCollisionPolicy policy = kDefaultCollisionPolicy);

// --- Tilemap sampling math (mirrors the tile fragment shader) ---

// `outside` is set only under TileWrap::Blank, when the world coord falls outside [0, mapPx) on
// either axis (the finite-map hole the shader discards); Repeat/Clamp never set it.
struct TileSample { int tileX; int tileY; int pixelX; int pixelY; bool outside = false; };

namespace detail {
// Floor-division / floor-modulo so negative scroll wraps correctly (C++ `/` and `%`
// truncate toward zero, which would mis-wrap negative world coordinates).
[[nodiscard]] constexpr int floorDiv(int a, int b) noexcept {
    const int q = a / b;
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
[[nodiscard]] constexpr int floorMod(int a, int n) noexcept {
    const int r = a % n;
    return (r < 0) ? r + n : r;
}
}  // namespace detail

// Given an output pixel within the layer (origin top-left, before scroll), the layer's scroll, the
// tilemap dimensions, and the wrap mode, return the wrapped tile coordinate and the within-tile
// pixel offset (and, for Blank, whether the pixel is a finite-map hole). Pure + constexpr so the
// (scroll, wrap, negative-scroll) mapping is unit-testable independent of the GPU; the tile
// fragment shader runs the identical math. Precondition: tilePx > 0. Degenerate (≤0) tilemap
// dimensions yield tile coord 0 on that axis (Repeat/Clamp) or a hole (Blank) rather than dividing
// by zero. `wrap` defaults to Repeat (toroidal).
[[nodiscard]] constexpr TileSample sampleTilemap(int px, int py, LayerScroll scroll,
                                                 int widthInTiles, int heightInTiles,
                                                 TileWrap wrap = TileWrap::Repeat,
                                                 int tilePx = 8) noexcept {
    const int worldX = px + scroll.x;
    const int worldY = py + scroll.y;
    const int mapPxX = widthInTiles  * tilePx;
    const int mapPxY = heightInTiles * tilePx;

    if (wrap == TileWrap::Blank) {
        // Finite map: outside [0, mapPx) on either axis is a hole. A degenerate (≤0) dimension is
        // entirely outside, so the whole layer is a hole.
        if (widthInTiles <= 0 || heightInTiles <= 0 ||
            worldX < 0 || worldX >= mapPxX || worldY < 0 || worldY >= mapPxY) {
            return TileSample{0, 0, 0, 0, /*outside=*/true};
        }
        return TileSample{worldX / tilePx, worldY / tilePx, worldX % tilePx, worldY % tilePx, false};
    }

    if (wrap == TileWrap::Clamp) {
        // Clamp the world coord to the map's last pixel, then decompose (non-negative ⇒ truncating
        // division == floor). A degenerate dimension pins that axis to tile 0.
        const int cx = widthInTiles  > 0 ? std::clamp(worldX, 0, mapPxX - 1) : 0;
        const int cy = heightInTiles > 0 ? std::clamp(worldY, 0, mapPxY - 1) : 0;
        return TileSample{widthInTiles  > 0 ? cx / tilePx : 0,
                          heightInTiles > 0 ? cy / tilePx : 0,
                          cx % tilePx, cy % tilePx, false};
    }

    // Repeat — toroidal (the default).
    const int tileX  = widthInTiles  > 0 ? detail::floorMod(detail::floorDiv(worldX, tilePx), widthInTiles)  : 0;
    const int tileY  = heightInTiles > 0 ? detail::floorMod(detail::floorDiv(worldY, tilePx), heightInTiles) : 0;
    const int pixelX = detail::floorMod(worldX, tilePx);
    const int pixelY = detail::floorMod(worldY, tilePx);
    return TileSample{tileX, tileY, pixelX, pixelY, false};
}

}  // namespace retropp
