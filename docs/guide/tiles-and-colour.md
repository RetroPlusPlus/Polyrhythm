# Tiles & colour

The faithful 8-/16-bit colour model: art is **palette indices**, and colour comes from a **palette
chosen at render time** — never baked into the art. This page covers indexed atlases, palette upload
and storage, and how each tile and sprite names its own sheet and palette directly. The submission
types (`TileContent`, `Sprite`) are in [draw-state.md](draw-state.md); loading art from PNG is
[images-and-transparency.md](images-and-transparency.md).

```cpp
#include "retropp/palette.h"      // Rgba8, PaletteId, PaletteSize
#include "retropp/draw_state.h"   // TileCell, Sprite, AtlasId
```

## Contents

- [The colour model](#the-colour-model)
- [`Rgba8` — a final output colour](#rgba8--a-final-output-colour)
- [Uploading palettes: `uploadPalette` → `PaletteId`](#uploading-palettes-uploadpalette--paletteid)
- [Loading a palette from an image: `loadPaletteImage` → `PaletteId`](#loading-a-palette-from-an-image-loadpaletteimage--paletteid)
- [Changing a palette's colors: `updatePalette`](#changing-a-palettes-colors-updatepalette)
- [Uploading art: `uploadAtlas` → the sheet](#uploading-art-uploadatlas--the-sheet)
- [Each tile / sprite names its own sheet + palette](#each-tile--sprite-names-its-own-sheet--palette)
- [The `tiles()` helper — a single-combo cell run](#the-tiles-helper--a-single-combo-cell-run)
- [Flip and rotate](#flip-and-rotate)
- [Wrap (finite vs infinite maps)](#wrap-finite-vs-infinite-maps)
- [Where to change things](#where-to-change-things)

## The colour model

An **indexed atlas** holds one palette index per pixel (`0..N-1`), uploaded once. A **palette** maps
each index to a final output colour. A tile or sprite names *which* sheet (`AtlasId`) and *which*
palette (`PaletteId`) it draws from, directly. The shader applies the colour per-pixel: `index →
named palette → output colour`.

This is the faithful Game Boy / Game Boy Color model — colour is an index plus a palette selected at
render time — **not** a baked-RGBA atlas and **not** a hardware palette-RAM poke. Because the lookup
happens per-pixel in the shader, palette swaps, day/night, and animation stay data/shader concerns,
never a register write. The same indexed art renders in any colour scheme by naming a different
palette.

## `Rgba8` — a final output colour

```cpp
struct Rgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;   // named channels; opaque black by default
};
static_assert(sizeof(Rgba8) == 4);
```

The platform's output colour is `Rgba8` regardless of the source console — four named channels, opaque
by default, 4 bytes so it uploads as a tightly-packed RGBA8 texture row. A palette's colours come from
the game's assets at upload time (decoded from the console's own palette format consumer-side, or
hand-built).

For colour sources carrying more than 8 bits per channel (a 16-bit truecolour PNG) there is a
16-bit-per-channel sibling, `Rgba16` — the same named-channel aggregate, opaque black by default:

```cpp
struct Rgba16 {
    std::uint16_t r = 0, g = 0, b = 0, a = 65535;   // named channels; opaque black by default
};
static_assert(sizeof(Rgba16) == 8);
```

The palette store keeps 16-bit channels internally, so an `Rgba8` entry widens into it losslessly:
`widen8(std::uint8_t)` maps `0 → 0` and `255 → 65535` (× 257, not `<< 8`), and `widen(Rgba8)` widens a
whole colour. The `span<const Rgba8>` upload overload calls `widen` for you; the `span<const Rgba16>`
overload stores 16-bit source colours directly.

## Uploading palettes: `uploadPalette` → `PaletteId`

```cpp
PaletteId Renderer::uploadPalette(std::span<const Rgba8>  colors);  // 8-bit colours, widened into the store
PaletteId Renderer::uploadPalette(std::span<const Rgba16> colors);  // 16-bit colours, stored losslessly
```

Upload one palette's colours once (amortized — at load time / on change). The renderer writes them
into a row of its internal 16-bit palette store and returns a `PaletteId` handle. The store keeps
16-bit-per-channel colour, so an `Rgba8` entry widens losslessly and an `Rgba16` (16-bit source) entry
is stored directly. The entry count is the **span length** — `uploadPalette` takes no separate count.
`PaletteSize` is a set of readability **count mnemonics** whose enumerator value *is* the entry count
(`static_cast<std::uint32_t>(PaletteSize::GameBoy) == 4`) — for sizing a buffer or documenting intent,
not a parameter any call requires:

```cpp
enum class PaletteSize : std::uint32_t {
    GameBoy = 4, GameBoyColor = 4, Nes = 4,
    MasterSystem = 16, Genesis = 16, Snes = 16,
};
```

These set only *how many* entries, not a per-console colour model. A 4-colour Game Boy palette and a
16-colour SNES palette are the same call with a different span length.

## Loading a palette from an image: `loadPaletteImage` → `PaletteId`

```cpp
PaletteId Renderer::loadPaletteImage(LiteralPath path,
                                     ReadOrder order = ReadOrder::LeftRightThenDown,
                                     int count = 0,
                                     std::optional<AssetPolicy> policy = {});
```

A **palette image** is a colour PNG read **one pixel per palette entry** — the colour-store sibling of
`loadAtlas` (which reads an indexed PNG into the atlas store). `loadPaletteImage` decodes the image,
slices it one pixel at a time in `order` (the same `ReadOrder` traversal `loadAtlas` slices a sheet
with — entry *k* is the *k*-th pixel in that order), uploads the entries, and returns their base
`PaletteId`. The entries are contiguous, so a tile/sprite that names this palette selects entry *k* by
index *k*.

Palette images are **16-bit per channel with alpha**: a 16-bit PNG lands losslessly, an 8-bit one
widens, and an entry's alpha is **material transparency** — alpha 0 reads through to whatever is drawn
behind it, a partial alpha blends. `count` caps how many entries are taken (0 = every pixel). The
`path` is a build-managed literal; the default asset policy is **Embed** (a palette image is bespoke
build-time colour data, like a map PNG) — see [assets-and-embedding.md](assets-and-embedding.md).

There is no `loadPaletteImageFromMemory`: a palette is already buildable from colour data via
`uploadPalette`, so for a runtime-supplied PNG compose the public primitives —
`uploadPalette(slicePaletteImage(loadPngFromMemory(bytes), order, count))`. The pure slicer is
available headless:

```cpp
std::vector<Rgba16> slicePaletteImage(const LoadedImage& img,
                                      ReadOrder order = ReadOrder::LeftRightThenDown, int count = 0);
```

[`examples/palette_image_demo`](../../examples/palette_image_demo/main.cpp) loads a 16-bit hue grid (Embed) and an alpha ramp (LoadFromPath), walks
all 8 read orders, and draws the entries over a checker so the per-entry alpha reads as transparency.

## Changing a palette's colors: `updatePalette`

```cpp
void Renderer::updatePalette(PaletteId id, std::span<const Rgba8>  colors);  // 8-bit colors, widened into the store
void Renderer::updatePalette(PaletteId id, std::span<const Rgba16> colors);  // 16-bit colors, stored losslessly
```

The palette at `id` takes `colors` and keeps its handle: every cell and sprite that names it draws
the new colors from the next frame on, with no change to the draw state. `colors` holds exactly the
palette's entry count as uploaded — a cell may index any entry, so the count is part of what the
handle means. `Rgba8` widens as it does on upload; `Rgba16` is stored directly.

```cpp
std::array<Rgba8, 4> ramp = {/* the scheme's four colors */};
const PaletteId skin = renderer.uploadPalette(std::span<const Rgba8>(ramp));   // once, at load

// Later, as the player edits the ramp:
ramp[2] = Rgba8{.r = red, .g = green, .b = blue};
renderer.updatePalette(skin, std::span<const Rgba8>(ramp));                   // the same handle, new colors
```

An update is a data change like an upload: the colors travel in the next frame's copy pass, so a
set of palettes changed between two frames costs one submission, and the store does not grow — a
palette edited live, a color at a time, is this call once per change. A frame whose structure has
not otherwise changed still draws the new colors (the whole-frame skip in
[rendering.md](rendering.md#upload-skip) reads the store's state as well as the frame's).

Two calls are refused with `std::invalid_argument`, and nothing is written: an `id` that is not the
first entry of an uploaded palette, and a `colors` whose length differs from that palette's entry
count.

[`examples/palette_update`](../../examples/palette_update/main.cpp) draws a sunset through one
four-color palette beside the same colors as swatches; keys step a color's red, green and blue, and the
picture takes each step on the next frame under the same handle.

## Uploading art: `uploadAtlas` → the sheet

```cpp
AtlasManifest Renderer::uploadAtlas(const std::uint8_t*  indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
AtlasManifest Renderer::uploadAtlas(const std::uint16_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
AtlasManifest Renderer::uploadAtlas(const std::uint32_t* indices, int width, int height, TransparentIndices transparent = TransparentIndices::None);
// carve overloads — declare the sheet's grid alongside the pixels (same sentence as loadAtlas):
AtlasManifest Renderer::uploadAtlas(const std::uint8_t* indices, int width, int height,
                                    AssetDimensions assetSize, ContentKind kind,
                                    ReadOrder order = ReadOrder::LeftRightThenDown, int count = 0,
                                    TransparentIndices transparent = TransparentIndices::None,
                                    int framesPerAnimation = 0);   // also uint16_t / uint32_t
```

Upload an **indexed** atlas once — one palette index per pixel, row-major, tightly packed (not RGBA;
colour comes from a palette at render time). The overloads take 8-, 16-, or 32-bit source indices;
all widen into the renderer's 32-bit index store, so a tile pixel can address an arbitrarily large
palette no matter the source width. The atlas is addressed as an 8×8-tile grid: tile `t` lives at grid
`(t % cols, t / cols)`. Every upload returns the sheet's `AtlasManifest` and declares its **carve**:
the three-argument form carves `Single` (the whole image is one asset), the `assetSize`/`kind` form
declares a grid — see [images-and-transparency.md](images-and-transparency.md#slicing). The manifest
records that carve kind in `sheet.kind`, so a consumer can tell what a sheet holds. Where only the
handle is wanted, write the projection explicitly — `sheet.atlasId` — dropping the slots is always
visible at the call site. The optional `transparent` is
the sheet's structural transparent-index set (default `None` = nothing discarded;
`TransparentIndices::GameBoy` / `GameBoyColor` for the index-0 OBJ hole;
`TransparentIndices::of({...})` builds a custom set and `.contains(i)` tests membership) — see
[images-and-transparency.md](images-and-transparency.md). To load an atlas from a PNG instead of
building the index array by hand, use `loadAtlas` (same page) — handing a decoded image straight to
`uploadAtlas` throws, so PNGs always go through `loadAtlas`.

## Each tile / sprite names its own sheet + palette

Every `TileCell` and every `Sprite` names its **own sheet** (`atlas`, an `AtlasId`) and its **own
palette** (`palette`, a `PaletteId`) — both directly, as handles:

```cpp
struct TileCell {
    AtlasId       atlas{};       // which uploaded sheet this cell draws from
    std::uint16_t tile    = 0;   // cell index within its own sheet (`atlas`), on the 8px grid
    PaletteId     palette{};     // which uploaded palette colours it
    bool          flipX   = false;
    bool          flipY   = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};
```

There is **no per-layer set and no cap.** Because each cell carries its sheet and palette, **one tile
layer freely mixes any number of sheets and any number of palettes** — a font sheet and a menu-border
sheet, several palettes, all in the same layer, with no slot limit. Getting more than one palette on
screen is just data: different cells carry different palette handles, and a palette swap is rewriting
a handle. Sprites work identically — `Sprite::atlas` and `Sprite::palette` name the sprite's own sheet
and palette, so one sprite layer mixes sheets and palettes freely too.

`TileContent` and `SpriteContent` carry no atlas or palette of their own — they hold the cells /
sprites, each self-describing:

```cpp
struct TileContent {
    int                       widthInTiles  = 0;
    int                       heightInTiles = 0;
    std::span<const TileCell> cells;            // row-major, each cell names its sheet + palette
    TileWrap                  wrap = TileWrap::Repeat;
};

struct SpriteContent {
    std::span<const Sprite> sprites;            // each sprite names its sheet + palette
};
```

Palettes are **arbitrary size** — there is no 256-colour cap — and an atlas pixel widens to a full
index, so one palette can hold as many colours as you upload and a tile (or sprite) pixel can address
any of them. A `PaletteId`'s underlying value *is* its flat offset into the palette store, and an
`AtlasId` names a region of the flat atlas store, so the shader reads both directly with no
indirection — you never manage per-sheet textures.

This is the mechanism that lets a small palette render a full-colour map: a single indexed tileset,
drawn through several palettes named per cell, produces a multi-coloured scene.

## The `tiles()` helper — a single-combo cell run

When a run of cells all draw from **one** sheet + palette, `tiles()` fills the repeated handles for
you over a list of slots, so you don't hand-write the same `atlas`/`palette` on every literal:

```cpp
#include "retropp/tilemap.h"   // tiles()

// Three cells, all from `fontAtlas` coloured by `textPal`, drawing slots 7, 8, 9 in order:
std::vector<TileCell> run = tiles(fontAtlas, textPal, {7, 8, 9});
```

It returns one `TileCell` per slot, in order, with no flip — plain mutable data, so set a flip or a
different handle on any cell afterward for anything that varies. It's the single convenience over
hand-writing `TileCell{ .atlas = …, .tile = …, .palette = … }` literals for the common single-combo
run; a layer mixing several sheets just concatenates several runs (or builds the cells from a map image
and a catalog — see **[tilemaps.md](tilemaps.md)**).

## Flip and rotate

`flipX` / `flipY` on a `TileCell` (and on a `Sprite`) mirror the tile's pixels horizontally /
vertically at sample time — the within-tile offset is flipped before the atlas is addressed.

`rotation` turns the tile's pixels in 90° steps (`Rotation::None` / `Rot90` / `Rot180` / `Rot270`,
clockwise), the same kind of sample-time op: it changes which source pixel is read, not the destination
quad. It composes with the flips, so a `(rotation, flipX, flipY)` combination reaches all eight
orientations of square art from one slot:

```cpp
TileCell c{ .atlas = sheet, .tile = 5, .palette = pal, .rotation = Rotation::Rot90 };  // turned 90° CW
```

A flip only mirrors — a horizontal edge tile can't become a vertical one — so rotation is what lets a
single edge tile serve all four sides of a border (and one corner tile reach every corner). No extra art.

For arbitrary, non-90° rotation of a sprite's quad, use the geometric `Sprite::transform` instead;
`rotation` is the grid-aligned texture turn, the discrete sibling of the flips. On a non-square sprite,
`Rot90` / `Rot270` transpose the read (the source extents swap) — square tiles and sprites are unaffected.

## Wrap (finite vs infinite maps)

A tile layer's `TileContent::wrap` chooses what happens when scrolling samples beyond the map's
`widthInTiles × heightInTiles` bounds: `Repeat` (default) tiles the map infinitely, `Clamp` smears
the edge tile, and `Blank` makes the map **finite** — anything past the edge is transparent (the
layers below show through), so a finite overworld map ends cleanly instead of repeating. The full
description is in [draw-state.md](draw-state.md#tilecontent--a-scrolling-tile-map).

## Where to change things

- **Recolour a scene without new art:** rewrite the `palette` handle on the cells / sprites you want
  recoloured — they each name their own.
- **Animate color (water shimmer, palette cycling) or edit a palette live:** change the palette's
  colors in place (`updatePalette` — the same `PaletteId`) or rewrite the `palette` handle on a cell /
  sprite per frame — both are data changes, no shader edit.
- **Whole-frame fades / day-night:** that's a `ColorFill` region + a blend mode
  ([draw-state.md](draw-state.md#whole-frame-colour)) grading the composited frame — distinct from the
  per-pixel palette colouring here.
- **Swap an entire tileset:** upload a new atlas and reference its `AtlasId` from the layer.
