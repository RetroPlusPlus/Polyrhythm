# Anchors, pivots & articulation

How multi-part figures are built from plain sprites: named points published by a sprite (**anchors**),
the point a sprite is placed by (**the origin**) and the point it transforms about (**the pivot**), and
the per-sprite **z** that stacks the parts within one layer. The platform's part is pure geometry — a few
resolvers and a sort key on `Sprite`; the game owns what the points *mean* (joints, mounts, muzzles) and
any chain logic.

```cpp
#include "retropp/draw_state.h"   // Sprite, Anchor, Point, Space, orientPoint, spriteDrawOrder
```

## Contents

- [The model](#the-model)
- [Anchors: published points](#anchors-published-points)
- [Origin & pivot: placement and spin](#origin--pivot-placement-and-spin)
- [Attaching: mount on an anchor](#attaching-mount-on-an-anchor)
- [Flips and rotation: anchors follow the art](#flips-and-rotation-anchors-follow-the-art)
- [Stacking the parts: `Sprite::z`](#stacking-the-parts-spritez)
- [Anything attaches to an anchor](#anything-attaches-to-an-anchor)
- [Interpolation](#interpolation)
- [Gotchas](#gotchas)
- [Where to change things](#where-to-change-things)

## The model

One sentence: **a sprite places by its origin and spins about its pivot; sprites publish points.**

- An **anchor** is a named point on a sprite's *art* — a socket, hinge, muzzle, emitter. Query it and
  get back where that point is *right now*, the sprite's transform applied.
- The **origin** is the quad-space point `Sprite::x/y` place — the placement handle. The **pivot** is
  the quad-space point `Sprite::transform` spins about — the transform center. They are independent;
  set them to the SAME point to place and spin on one spot (a joint).
- **Attachment is re-feeding**: each tick the game writes a parent's anchor into a child's `x/y`. No
  binding is held anywhere — the anchor query is a pure resolver like `sampleAnimationFrame(tick)` or
  `sampleTweenValue(t)`; the value flows wherever the game routes it.

There is no skeleton, no bone hierarchy, no parenting registry. A chain of joints is a few lines of
game code walking outward (see [Attaching](#attaching-mount-on-an-anchor)); the platform answers one
question per query: *where is stored point K under this sprite's current placement.*

## Anchors: published points

```cpp
struct Anchor {
    std::string_view label;   // the durable address — also addressable by index
    float            x = 0.0f;  // ART-space px (the art as it sits on the sheet)
    float            y = 0.0f;
};
```

A sprite carries `std::span<const Anchor> anchors` — game-owned, like a layer's `cells`/`sprites`
spans. A `static constexpr` table is the natural shape:

```cpp
static constexpr Anchor kArmAnchors[] = {
    {.label = "root",  .x = 2.0f,  .y = 4.0f},   // socket end — pivots on the parent
    {.label = "elbow", .x = 13.0f, .y = 4.0f},   // far end — the next segment's socket
};
static_assert(!findDuplicateAnchorLabel(kArmAnchors));   // compile-time label-uniqueness check

Sprite arm{.key = "arm", .size = AssetDimensions{16, 8}, .anchors = kArmAnchors};
```

One query, `anchor(k, Space)`, addressable by label **or** index and answering in the space you pass:

```cpp
Point q = arm.anchor("elbow", Space::Quad);    // QUAD space: where the art feature sits on the placed quad
                                               //   (flips/rotation applied; before transform + placement)
Point p = arm.anchor("elbow", Space::Layer);   // LAYER space: through transform + placement — where it IS now,
                                               //   rotation included. p == Point{x,y} + (pivot − origin) + transform·(q − pivot)
Point i = arm.anchor(std::size_t{1}, Space::Layer);  // the same point by index
```

The space is a value the call takes, so a caller can pass a computed or stored `Space`, or loop over both.

Labels are the durable address: reorder the table and an index silently names a different point,
while a label keeps naming the same one — and a *missing* label **throws** `std::out_of_range`
(so a typo fails loudly, the same posture as a missing save document or an unknown action id). A
duplicated label resolves to the first match; `findDuplicateAnchorLabel` is the `constexpr`
uniqueness check to `static_assert` over a fixed table.

`Space::Layer` answers in the **layer's coordinate space** — the space `x`/`y` live in, which is what a
sibling sprite on the same layer consumes. It is a pure function of the sprite's own fields; it never
sees the layer's scroll or transform. Consumers on *other* layers map between layer spaces themselves
(the game owns both layers' scroll and transforms).

## Origin & pivot: placement and spin

```cpp
Sprite s{.key = "wheel", .size = AssetDimensions{16, 16}};
s.origin    = s.center(Space::Quad);          // QUAD-space px; default {0,0} = the top-left
s.pivot     = s.center(Space::Quad);          // QUAD-space px; default {0,0} = the top-left
s.x         = 80;  s.y = 72;                  // x/y place the ORIGIN — the wheel's center sits here
s.transform = Transform::rotation(angle);     // …and it spins about the PIVOT
```

A local quad point `p` lands at `(x, y) + (pivot − origin) + transform·(p − pivot)`. Two points, two
jobs:

- **`origin`** is the placement handle: `x/y` place it. At the identity transform the pivot drops out
  and a point lands at `(x, y) + (p − origin)`, so `origin = {0,0}` places by the top-left corner and
  `origin = s.center(Space::Quad)` places by the middle.
- **`pivot`** is the spin center: `transform` applies about it. Under any origin-fixing transform (the
  plain `rotation(θ)` / `scale` / `skew` forms) the pivot's own image is `(x, y) + (pivot − origin)`,
  the one point the transform holds still. A pivot change never moves an untransformed sprite.

`Sprite::center(Space)` returns the sprite's middle in the space you ask for. `Space::Quad` is the raw
art midpoint (`{size.width / 2, size.height / 2}`) — the quad-space value you place and spin by.
`Space::Layer` is that midpoint mapped through transform + placement — the center of the sprite *as
drawn*, which a consumer reads to sit something at the visible center. **Origin and pivot are transform
inputs, so feed them from `Space::Quad`** — a `Space::Layer` center is computed *through* origin and
pivot, so feeding it back into them is circular.

Set `origin` and `pivot` to the **same** point to place and spin on one spot: `origin = pivot =
s.center(Space::Quad)` sits the sprite by its middle and turns it about its middle; `origin = pivot =
anchor("hinge", Space::Quad)` puts a mount point at `x/y` and turns the sprite about it — that is a joint
(see [Attaching](#attaching-mount-on-an-anchor)).

Neither point renumbers the sprite's local coordinates: anchors and art keep `{0,0}` at the sheet's
top-left, and an anchor at `(13, 4)` stays `(13, 4)` whatever `origin`/`pivot` are — they pick *which*
local points are the handle and the hinge, not where the ruler starts.

This differs from a pivot baked into the matrix (`Transform::rotation(deg, 8, 8)`): the baked form
composes inside the transform and only affects the spin, while `Sprite::pivot` is the spin center as a
first-class field and `Sprite::origin` is the separate placement handle.

`Sprite::toLayer(Point)` is the same mapping for any quad-space point you already have — `anchor(k,
Space::Layer)` is `toLayer(anchor(k, Space::Quad))`.

## Attaching: mount on an anchor

"Attach the forearm to the elbow" is literal: set the forearm's origin AND pivot to its own socket, and
write the upper arm's elbow anchor into the forearm's position. The socket sits at the joint (it is the
origin) and rotation about the pivot is rotation about the joint (it is the pivot too) — placement handle
and hinge are the same point.

```cpp
// One articulated chain, walked outward each tick — angles accumulate.
auto attach = [](Sprite& child, const char* socket, const Sprite& parent, const char* joint,
                 float degrees) {
    child.origin    = child.anchor(socket, Space::Quad);   // the socket, mirrored with the art if flipped
    child.pivot     = child.anchor(socket, Space::Quad);   // origin = pivot: mount and hinge coincide
    const Point p   = parent.anchor(joint, Space::Layer);  // where the joint is right now
    child.x         = static_cast<int>(std::lround(p.x));
    child.y         = static_cast<int>(std::lround(p.y));
    child.transform = Transform::rotation(degrees);
};

attach(upperArm, "root",  body,     "shoulder", shoulderAngle);
attach(forearm,  "root",  upperArm, "elbow",    shoulderAngle + elbowAngle);
attach(claw,     "hinge", forearm,  "wrist",    shoulderAngle + elbowAngle + wristAngle);
```

That lambda is the ceiling of built-in convenience — the chain itself (which joint parents
which, how angles accumulate) is game logic and stays in the game. The working end-to-end example is
[`examples/articulation_demo/`](../../examples/articulation_demo/main.cpp).

## Flips and rotation: anchors follow the art

`flipX`/`flipY`/`rotation` are texture operations — they reorient which art pixel each quad pixel
reads, and the quad itself never moves. Stored points split accordingly:

- **Anchors live on the art and ride those ops.** Flip a leg and its socket mirrors with the pixels;
  `anchor(k, Space::Quad)` / `anchor(k, Space::Layer)` return the mirrored position automatically — no
  per-facing anchor tables.
- **Origin and pivot live on the quad and ignore them.** They are the placement handle and the spin
  center; the default `{0,0}` is always the quad's top-left, flipped art or not.

When the mount *is* an art feature (it usually is), bridge with one line each — re-read the anchor into
origin and pivot after setting the flip:

```cpp
claw.flipX  = facingLeft;
claw.origin = claw.anchor("hinge", Space::Quad);   // follows the flipped art
claw.pivot  = claw.anchor("hinge", Space::Quad);
```

**What goes wrong without the bridge.** Suppose the claw's hinge is drawn at `(2, 4)` on 8-wide art and
you set `origin = pivot = {2, 4}` by hand. Unflipped, that is exactly what `anchor("hinge", Space::Quad)` returns
— no difference. Flip the claw and they diverge: the drawn hinge now sits at `(6, 4)` within the quad,
but origin and pivot still name `(2, 4)` — which is now where the claw's *tip* is drawn. Nothing breaks
mechanically; the sprite is still placed by its origin and still rotates about its pivot — but those are
the wrong point, mounted through the wrong pixels:

1. **Placement attaches the wrong pixels.** `x/y` put quad point `(2, 4)` (the origin) on the wrist, so
   the claw's tip sits welded to the joint while the drawn hinge floats off to the side — the joint looks
   dislocated.
2. **It swings about the wrong point.** Rotation stays welded to the pivot (that part always works), but
   the pivot is the tip — the claw sweeps a wrong arc and the drawn hinge *orbits* the wrist instead of
   staying pinned to it. The classic off-pivot wobble.

Set them by hand and you own keeping them in sync with every flip/rotation state. Set them from
`anchor(k, Space::Quad)` and they re-answer "where is the drawn hinge in my quad *right now*" each tick — the
desync is structurally impossible.

The underlying map is `orientPoint(p, w, h, rotation, flipX, flipY)` — a pure helper, exposed for
game-side use on any art-space point. For a non-square sprite, `Rot90`/`Rot270` transpose the art
extents, the same transpose the texture read makes.

## Stacking the parts: `Sprite::z`

An articulated figure lives on **one** layer, and its parts need explicit stacking that survives
whatever order the chain math produced them in — hand in front of arm, arm behind torso:

```cpp
claw.z  = 30;   // frontmost
fore.z  = 20;
upper.z = 10;
body.z  = 0;    // default
```

Within a sprite layer, sprites draw back-to-front by ascending `z`. Unlike `DrawLayer::z` it is
**not unique**: any values are legal (negative included), and equal-z sprites keep their submission
order (the sort is stable — deterministic, no frame-to-frame reshuffle). Nothing validates it and
nothing throws. The classic top-down Y-sort is one assignment per sprite: `s.z = s.y + s.size.height`
(sort by feet), no layer per row. `spriteDrawOrder(sprites)` is the pure ordering function if game
code wants the same order the renderer draws.

## Anything attaches to an anchor

`anchor(k, Space::Layer)` returns a `Point`, and points are the platform's common currency — the query's
answer feeds anything that takes a position, not just another sprite:

```cpp
const Point tail = body.anchor("tail", Space::Layer);
Curve trail = Curve::quadratic(Vec2{tail.x, tail.y},          // a curve ORIGIN riding the body
                               Vec2{tail.x - 18.0f, tail.y - 6.0f},
                               Vec2{tail.x - 34.0f, tail.y + 10.0f});
```

A curve origin, a `PathWalker` frame of reference, a tween target, a particle-emitter position, a
region center — the anchor never knows what consumes it; the game reads a point per tick and routes
it. Re-author the consumer from the fresh point each tick (the immediate-mode model), and the whole
construction rides the sprite's motion.

## Interpolation

Origin and pivot are continuous fields: like position, alpha, and the transform, each eases between
simulation ticks under the automatic interpolator (keyed by the sprite's `key`), so a pivot that moves
between ticks eases as a moving hinge and a moving origin eases as a sliding placement handle. `z` is
discrete — it snaps to the current submission like the flips.

Re-fed chains stay glued mid-ease with no help needed: the interpolator lerps each part's
transform coefficient-wise and each part's position, and a matrix lerp applied to a fixed point
equals the lerp of the transformed points — parent and child ease along the same track. The residual
is the integer rounding of `x/y` (under one pixel). Anchor queries always answer from the tick state
the game holds; there is no render-time (eased) anchor query.

## Gotchas

- **`x/y` place the origin, and the default origin is the top-left.** A sprite that never sets `origin`
  behaves exactly as its `x/y` read — top-left placement. Set `origin = s.center(Space::Quad)` and the
  same `x/y` now name the center; adjust positions when introducing one.
- **Feed origin and pivot from `Space::Quad` only.** They are transform inputs; a `Space::Layer` query
  (`center(Space::Layer)`, `anchor(k, Space::Layer)`) is computed *through* origin and pivot, so writing
  one back into them is circular. `Space::Layer` values are for consumers — a sibling, a path, an effect.
- **A pivot alone never moves an untransformed sprite.** The pivot is the spin center; with an identity
  transform it drops out of the placement entirely — placement is `origin`.
- **A transform with its own translation moves the pivot's held image.** The pivot's image is
  `(x, y) + (pivot − origin)` for origin-fixing transforms (`rotation(θ)`, `scale(sx, sy)`,
  `skew(kx, ky)` — the no-pivot-argument forms). A matrix carrying a baked pivot or a `translation(...)`
  component adds that displacement, as authored.
- **Unknown anchor names throw.** `anchor(k, Space)` throws `std::out_of_range` for a missing label or
  index — catch nothing; fix the name.
- **Anchor positions quantize at the write into `x/y`.** The queries are exact floats; `Sprite::x/y`
  are ints, so round once at the write (`std::lround`) — the sub-pixel remainder is below the
  viewport grid anyway.
- **A z change pops.** It is discrete: changing a part's rank mid-motion restacks it immediately,
  never a cross-fade.

## Where to change things

- **Publish a point on a sprite:** add it to the sprite's `Anchor` table (label it).
- **Place a sprite by a different point:** `Sprite::origin` (quad-space px; `Sprite::center(Space::Quad)`
  for the middle).
- **Spin a sprite about a different point:** `Sprite::pivot` (quad-space px).
- **Put a part in front of / behind its siblings on the same layer:** `Sprite::z`.
- **Whole-object stacking against other layers:** the layer's `z`, as ever — see
  [draw-state.md](draw-state.md).
- **Per-sprite spin/scale/foreshorten:** `Sprite::transform`, applied about the pivot — see
  [draw-state.md](draw-state.md#per-sprite-transforms).
- **Drive a part along a route instead of a joint:** [sprite-path.md](sprite-path.md) — paths and
  anchors compose as world-space points, not as a parenting system.
