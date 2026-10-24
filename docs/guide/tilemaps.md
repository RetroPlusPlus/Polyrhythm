# Tilemaps — building a tile layer from images

**Headers:** `tilemap.h`, `image.h` (`IndexGrid`, `loadMapPng`), `draw_state.h` (`TileCell`, `TileContent`)

This page covers building a tile layer's content from **authored images** instead of hand-written
cell arrays: a *map* image of tile indices plus one or more *atlas* sheets, assembled into the `cells`
a [`TileContent`](tiles-and-color.md) wants. Because every cell names its own sheet and palette, one
layer can draw tiles from **several sheets at once** — this path produces exactly that.

If you only need the low-level draw-state surface (hand-built `cells`, the palette model, flips, wrap
modes), read [tiles-and-color.md](tiles-and-color.md) and [draw-state.md](draw-state.md) first; this
page builds on both. Loading the atlas sheets themselves (PNG → atlas) is in
[images-and-transparency.md](images-and-transparency.md).

---

## Contents

- [The shape of it](#the-shape-of-it)
- [Mixing sheets in one layer](#mixing-sheets-in-one-layer)
- [The map image — `loadMapPng` → `IndexGrid`](#the-map-image--loadmappng--indexgrid)
- [The catalog — `TileCatalog`](#the-catalog--tilecatalog)
- [Assembling — `assembleTilemap` → `AssembledTilemap`](#assembling--assembletilemap--assembledtilemap)
  - [Handing it to a layer](#handing-it-to-a-layer)
- [Worked example — `examples/tilemap_demo.cpp`](#worked-example--examplestilemap_democpp)
- [Where to change things](#where-to-change-things)

## The shape of it

A tile layer is `cells` (a grid of `TileCell`), and each cell names the *sheet* it draws from (the
tile art) and the *palette* that colors it. The hand-written way is to fill the `cells` yourself. The
image-driven way is:

```
 map PNG ──loadMapPng──► IndexGrid ──┐
                                     ├──assembleTilemap──► AssembledTilemap ──asTileContent──► TileContent
 TileCatalog (declared) ─────────────┘
```

- **`IndexGrid`** — the map, decoded to one raw `uint16` index per cell.
- **`TileCatalog`** — a declared table mapping each index to a tile: *which sheet, which cell, which
  palette, flipped how*.
- **`AssembledTilemap`** — the result: the `cells` array, each cell already carrying its sheet and
  palette handle, ready to hand to a layer.

Each step is independent and testable; nothing here touches the GPU until you submit the resulting
`TileContent` in a frame.

---

## Mixing sheets in one layer

Each `TileCell` names its **own** sheet (`atlas`, an `AtlasId`) and palette (`PaletteId`) directly:

```cpp
struct TileCell {
    AtlasId       atlas{};       // which uploaded sheet this cell draws from
    std::uint16_t tile    = 0;   // cell index within its own sheet (`atlas`), on the 8px grid
    PaletteId     palette{};     // which uploaded palette colors it
    bool          flipX   = false;
    bool          flipY   = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};

struct TileContent {
    int                       widthInTiles  = 0;
    int                       heightInTiles = 0;
    std::span<const TileCell> cells;            // row-major; each cell names its sheet + palette
    TileWrap                  wrap = TileWrap::Repeat;
    std::optional<bool>       contentChanged;      // optional: declare cell changes; unset = the platform detects
};
```

**Declaring content changes (`contentChanged`).** By default (unset) the platform detects an unchanged
tilemap itself — it hashes the packed cells each frame and skips the GPU re-upload when nothing changed,
so a static map costs no per-frame DMA. A huge map can opt out of even that per-frame hash by answering
the question itself: set `contentChanged` to `true` on the frames whose cells changed (re-upload) and
`false` otherwise (skip). It is a declaration, not a hint: setting `false` while the cells have in fact
changed renders the stale map.

So **one layer mixes tiles from several sheets** — e.g. a font sheet for text and a border sheet for a
menu frame — simply because adjacent cells name different `atlas` handles. There is no per-layer atlas
or palette set and no slot cap: a layer draws from as many sheets and palettes as its cells reference.
`TileContent` itself carries no atlas or palette; the cells are self-describing.

Under the hood the renderer keeps every uploaded atlas in one *flat atlas store* (the same pattern the
palette store uses — see [tiles-and-color.md](tiles-and-color.md)) and a global atlas-region table
the tile and sprite shaders index by a cell's `atlas` handle. You don't manage any of that — you set
`atlas` and `palette` per cell (or let `assembleTilemap` do it from a catalog, below).

---

## The map image — `loadMapPng` → `IndexGrid`

A *map* is not art; it's a grid of **numbers**. Each pixel's sample value **is** a tile index — for a
tilemap, an index into a `TileCatalog`; for a collision map, a raw id your game interprets.

```cpp
struct IndexGrid {
    int                        width = 0, height = 0;
    std::vector<std::uint16_t> values;   // one raw index per pixel, row-major
    std::uint16_t at(int x, int y) const;
};

IndexGrid loadMapPng(LiteralPath path, std::optional<AssetPolicy> policy = {});  // literal logical path
IndexGrid loadMapPngFromMemory(std::span<const std::uint8_t> bytes);             // a runtime file: read + this
```

`loadMapPng`'s path is a literal logical path the build can bake or copy (a map PNG defaults to **Embed**
— see [assets-and-embedding.md](assets-and-embedding.md)); a map whose path you only know at runtime is
read with `loadMapPngFromMemory(readFile(...))`.

- **16-bit grayscale is the headline format** — a tilemap may reference more than 256 tiles, and a
  16-bit sample carries ids up to 65535. (8-bit and sub-byte grayscale, and palette PNGs, also decode —
  their indices just widen into the `uint16` grid.)
- The sample value is the index, **never** scaled or reverse-derived from a color — the same "the
  stored value is the index" rule the atlas path uses.
- **Truecolor is rejected** (`loadMapPng` throws): a map carries indices, not colors.
- A 16-bit map of small ids looks near-black in an image viewer (id 15 is `15/65535` gray) — that's
  inherent to an index map, not a bug. If you want a map you can eyeball, spread the ids across the
  range (the demo does — see below).

**Collision maps use this exact function.** Decode your collision PNG with `loadMapPng` and read
`IndexGrid::values` directly; the platform assigns the numbers no meaning — collision is your game's logic.

---

## The catalog — `TileCatalog`

A `TileCatalog` is a plain declared table. Each entry says what a map value draws:

```cpp
struct TileCatalogEntry {
    std::uint16_t id      = 0;   // the map value that selects this tile — identity, first field
    AtlasId       sheet{};       // which loaded atlas
    std::uint16_t slot    = 0;   // the 8px cell index within that sheet (an AssetSlot::tile)
    PaletteId     palette{};     // which uploaded palette colors it
    bool          flipX = false, flipY = false;
    Rotation      rotation = Rotation::None;  // 90° texture rotation; composes with the flips
};

struct TileCatalog { std::vector<TileCatalogEntry> entries; };
```

**`id` is the entry's identity, not its position.** A map value selects the entry whose `id` matches it,
so ids are **sparse 16-bit values** — you can scatter them across `0..65535` freely (which is also what
genuinely exercises a 16-bit map). Build it inline, straight from the `AtlasManifest`s `loadAtlas`
returned — `.sheet` takes the explicit projection `manifest.atlasId`, and `manifest[n].tile` addresses
the n-th carved cell for `.slot`:

```cpp
// menu / font are the AtlasManifests from loadAtlas (see images-and-transparency.md).
TileCatalog cat;
cat.entries = {
    {.id = 0,    .sheet = menu.atlasId, .slot = menu[3].tile, .palette = menuPal},                 // interior fill
    {.id = 4369, .sheet = menu.atlasId, .slot = menu[0].tile, .palette = menuPal},                 // top-left corner
    {.id = 8738, .sheet = menu.atlasId, .slot = menu[0].tile, .palette = menuPal, .flipX = true},  // top-right (mirrored)
    {.id = 4370, .sheet = menu.atlasId, .slot = menu[1].tile, .palette = menuPal},                          // top edge
    {.id = 4371, .sheet = menu.atlasId, .slot = menu[1].tile, .palette = menuPal, .rotation = Rotation::Rot90},  // right edge (turned)
    {.id = 39321,.sheet = font.atlasId, .slot = font[1].tile, .palette = textPal},                 // letter 'H'
    // …
};
```

**Flips and rotation reuse a slot instead of storing another tile.** A rectangular menu border needs only
three distinct menu tiles — an outer corner, one edge, and a fill — because reorientation is free at
sample time: `flipX`/`flipY` mirror a tile and `rotation` turns it in 90° steps. A flip alone can't turn a
horizontal edge into a vertical one, so `rotation` is what lets the single edge tile serve all four sides;
the corner reaches all four corners by flips or rotation. Together they give the eight orientations of
square art. Don't author a tile a flip or rotation can produce from one you already have.

---

## Assembling — `assembleTilemap` → `AssembledTilemap`

```cpp
struct AssembledTilemap {
    std::vector<TileCell> cells;          // row-major widthInTiles * heightInTiles
    int                   widthInTiles = 0, heightInTiles = 0;

    TileContent asTileContent(TileWrap wrap = TileWrap::Repeat) const;  // one-call conversion
};

AssembledTilemap assembleTilemap(const IndexGrid& map, const TileCatalog& catalog);
```

`assembleTilemap` looks each map value up by `id` and emits a `cells` array, each cell carrying its
entry's sheet, palette, slot, flip, and rotation directly. So a map mixing a font sheet and a menu sheet comes out
as one layer whose cells name both sheets — the multi-sheet result, computed for you, with no set or cap.

It throws on a map value with no matching `id` (`std::out_of_range`) or a duplicate catalog `id`
(`std::invalid_argument`). An empty grid yields an empty result.

### Handing it to a layer

`asTileContent(wrap)` is the one-call conversion — it fills `cells` and the dimensions into a `TileContent` (the
one display choice it can't know, the wrap mode, is the argument):

```cpp
AssembledTilemap map = assembleTilemap(loadMapPng("level1.png"), catalog);

DrawLayer layer{.key = "World"};   // key is required — DrawLayer has no default constructor
layer.size    = {160, 144};
layer.content = map.asTileContent(TileWrap::Blank);   // finite map: outside the bounds is transparent
```

**It's not fixed — edit it freely.** `AssembledTilemap` holds a plain mutable `cells` vector. After
assembling, mutate a cell (flip one, repoint its `atlas`, swap its `palette`), or push/erase cells, on
the fly. `asTileContent()` hands out a span pointing **into** that vector, so in-place edits show up the
next frame — but if a `push_back` **reallocates** the vector, call `asTileContent()` again so the span
doesn't dangle. The fully-manual route (set `TileContent::cells` yourself, no `AssembledTilemap` at all)
is always available for layers that rebuild their tiles every frame; for a run of cells all drawing from
one sheet + palette, the `tiles(atlas, palette, {slots…})` helper (also in `tilemap.h`) fills the
repeated handles — see
[the `tiles()` helper](tiles-and-color.md#the-tiles-helper--a-single-combo-cell-run).

---

## Worked example — `examples/tilemap_demo.cpp`

The demo draws "HELLO / WORLD" inside a gold menu frame, in **one** tile layer drawing tiles from **two
image sheets**:

1. `loadAtlas` two indexed sheets — a font (`H E L O W R D` glyphs) and a menu border (corner, h-edge,
   v-edge, fill), each an 8×8 `Tileset` — → an `AtlasManifest` (the uploaded atlas + its sliced slots).
   (A PNG always goes through `loadAtlas`, never `loadPng`+`uploadAtlas` — `uploadAtlas` is for raw index
   arrays you author yourself; handing it a `LoadedImage` throws.)
2. `loadMapPng` a 16-bit grayscale map whose ids are spread across `0..65535` (so it both exercises
   16-bit decode and is faintly eyeball-distinguishable).
3. Declare a `TileCatalog`: the menu border tiles (with flips for the other corners/edges) on the menu
   sheet, the letters on the font sheet, each with its palette.
4. `assembleTilemap` → an `AssembledTilemap` whose cells name the `menu` and `font` sheets and the
   `menuPal` / `textPal` palettes directly; `asTileContent(TileWrap::Blank)` into the layer.

The frame's letters come from the font sheet and its border from the menu sheet, mixed per cell, in a
single submission — the headline of this feature.

A non-game consumer of the same path is [`examples/Numberator`](../../examples/Numberator/main.cpp), a calculator: it assembles its window
chrome — the title bar and the sunken display well — from a map PNG and a `TileCatalog`, with the well's
four corners produced from **one** corner tile flipped four ways, and the map's ids spread across the
16-bit range so it reads as distinct gray levels. (Its keys are sprites and its digits are glyph sprites,
layered over the chrome by `z`.)

---

## Where to change things

- **The decode** lives in `src/image.cpp` (`loadMapPng` + the 16-bit sample unpack). Add a map source
  format here if you ever need one beyond grayscale/palette PNG.
- **The assembly** is `src/tilemap.cpp` (`assembleTilemap` + the id lookup). A future *alternate*
  assembler — a different way to turn source data into an `AssembledTilemap`, platform- or game-side —
  would live alongside it and produce the same type; that's deliberately why the type is named for the
  result (`AssembledTilemap`), not for one way of producing it.
- **The sheet storage** (the flat atlas store + the global atlas-region table + the tile/sprite shaders
  that index it by a cell's `atlas` handle) is in `src/renderer.cpp` and `shaders/src/tile.frag.hlsl` /
  `sprite.frag.hlsl`. You don't touch this to *use* multiple sheets; it's where the mechanism lives if
  you're forking the renderer.

> **Naming note.** `AssembledTilemap` is intentionally distinct from the platform's *internal* "tilemap"
> vocabulary — `sampleTilemap()` (the cell-grid sampling math), `TilemapTex` (the renderer's GPU cell
> texture), and `TileContent::cells` — so the public assembled bundle and the internal cell grid never blur.
