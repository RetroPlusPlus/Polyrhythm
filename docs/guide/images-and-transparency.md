# Images & transparency

Loading art from PNG files, and the opt-in per-source transparency that lets one layer's holes reveal
the layer beneath. This feeds the indexed color model in [tiles-and-color.md](tiles-and-color.md):
a PNG's index plane goes straight into `uploadAtlas`.

```cpp
#include "retropp/image.h"   // ImageColorKind, LoadedImage, loadPng/loadPngFromMemory, ImageId;
                          // ContentKind, ReadOrder, AssetSlot, sliceLayout (slicing, below)
```

## Contents

- [Loading a PNG: `loadPng`](#loading-a-png-loadpng)
  - [How sources route](#how-sources-route)
  - [Truecolor decodes to a color plane](#truecolor-decodes-to-a-color-plane)
  - [A truecolor image as a layer: `loadImage`](#a-truecolor-image-as-a-layer-loadimage)
- [Slicing an atlas into addressable assets](#slicing)
- [Transparency: structural and material](#transparency-structural-and-material)
- [A worked example](#a-worked-example)
- [Where to change things](#where-to-change-things)

## Loading a PNG: `loadPng`

```cpp
LoadedImage loadPng(const std::filesystem::path& path);
LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes);

enum class ImageColorKind : std::uint8_t { Indexed, Rgba };

struct LoadedImage {
    ImageColorKind            kind   = ImageColorKind::Indexed;
    int                       width  = 0, height = 0;
    std::vector<std::uint8_t> indices;   // kind == Indexed: one index per pixel, row-major
    std::vector<Rgba8>        palette;   // kind == Indexed: embedded palette (may be empty)
    std::vector<Rgba16>       pixels;    // kind == Rgba: one 16-bit color per pixel, row-major
};
```

`loadPng` decodes a PNG and routes by its color type. For the faithful indexed path it extracts the
**index plane** — one palette index per pixel — which is exactly what `uploadAtlas` consumes:

```cpp
const LoadedImage img = loadPng("assets/tileset.png");
const AtlasId atlas = renderer.uploadAtlas(img.indices.data(), img.width, img.height).atlasId;
```

> Pass the index pointer (`img.indices.data()`), not the whole `LoadedImage` — `uploadAtlas(const
> LoadedImage&)` deliberately throws `std::logic_error`. A PNG should be sliced via `loadAtlas`
> ([below](#slicing)), which carves it into addressable assets; the raw-pointer overload is for art
> you've already decoded and don't need sliced.

### How sources route

| PNG color type | `kind` | `indices` | `palette` |
|---|---|---|---|
| **Palette (PLTE)** | `Indexed` | the raw PLTE index per pixel | the embedded palette, as `Rgba8` |
| **Grayscale** | `Indexed` | the gray sample value *as* the index (e.g. 2-bit → 0..3) | empty (grayscale carries no color) |
| **Truecolor / truecolor-alpha** | `Rgba` | — (fills `pixels`) | — (see below) |

The index is **read, never reverse-derived from a color**: the decoder preserves the source's own
format and the platform unpacks the (possibly sub-byte) samples itself, so it is exact for any bit depth
(1/2/4/8-bit). A 16-bit-sample PNG throws on this indexed path — that depth carries wider indices and
belongs to the map-import path (`loadMapPng` / `loadMapPngFromMemory`, see
[tilemaps.md](tilemaps.md)). A palette PNG yields its PLTE index plane plus the
embedded palette; a grayscale PNG
yields its sample-as-index plane and an empty palette (you supply color separately via
`uploadPalette` — the indexed model never bakes color into the art). Errors — a missing file, a
corrupt PNG — throw `std::runtime_error`.

> **Faithful default.** Indexed/grayscale is the faithful console source format. Decoding works
> headlessly (pure CPU), so image loading is unit-testable with no window or GPU device.

### Truecolor decodes to a color plane

A truecolor (RGB / RGBA) PNG decodes too: `loadPng` returns `kind == Rgba` and fills `pixels` with one
**16-bit-per-channel** color per pixel (an 8-bit source widens losslessly ×257, a 16-bit source lands
direct), alpha included. This is the color counterpart to the index plane, and two routes consume it.
One builds a **palette** from an image: `slicePaletteImage` walks a truecolor PNG one pixel per palette entry (and
throws `std::runtime_error` on a non-`Rgba` source), and
`Renderer::loadPaletteImage` chains the decode + slice + upload (see
[tiles-and-color.md](tiles-and-color.md#loading-a-palette-from-an-image-loadpaletteimage--paletteid)).
The other shows the image itself: `Renderer::loadImage` decodes a truecolor PNG and uploads it whole as
an image a raster layer names (below).
Atlas art stays **indexed/grayscale** — the faithful console format, where color is an index plus a
palette chosen at render time — so `uploadAtlas` consumes the index plane, not `pixels`.

### A truecolor image as a layer: `loadImage`

```cpp
ImageId Renderer::loadImage(LiteralPath path, std::optional<AssetPolicy> policy = {});
ImageId Renderer::loadImageFromMemory(std::span<const std::uint8_t> bytes);
ImageId Renderer::uploadImage(std::span<const Rgba8>  pixels, int width, int height);
ImageId Renderer::uploadImage(std::span<const Rgba16> pixels, int width, int height);

enum class ImageId : std::uint32_t {};   // a handle to an uploaded image; {} names none
```

A truecolor PNG is uploaded once as an image the renderer owns, and a raster layer shows it by naming
the handle — `RasterContent{.image = id}` — with nothing uploaded per frame
([draw-state.md](draw-state.md#an-image-the-renderer-holds)). `loadImage` takes a literal logical path
and the asset policy every path form takes ([assets-and-embedding.md](assets-and-embedding.md)); its
default is `LoadFromPath`, as `loadAtlas`'s is. `loadImageFromMemory` takes the PNG's bytes;
`uploadImage` takes pixels you hold — 8-bit, or 16-bit for a source that carries more.

```cpp
const ImageId title = renderer.loadImage("game/assets/title.png");                       // shipped beside the binary
const ImageId badge = renderer.loadImage("game/assets/badge.png", AssetPolicy::Embed);   // baked in
const ImageId frame = renderer.loadImageFromMemory(readFile(assetRoot() / chosen));      // a runtime file
```

The image is held at the depth its pixels need — an 8-bit PNG at 8 bits, a 16-bit PNG at 16 — and its
alpha is carried per pixel, so the layer composites by the image's own edges. A truecolor (RGB / RGBA)
PNG is required: an indexed or grayscale PNG is atlas art and throws `std::runtime_error` (load it
with `loadAtlas`); a missing file or a corrupt PNG throws `std::runtime_error` as every loader does.
`uploadImage` throws `std::invalid_argument` on a non-positive dimension or a span shorter than
`width × height`, with nothing uploaded. Handles stay valid until the renderer is destroyed.

`examples/image_layer/` registers one and shows it over a tile layer.

## Slicing an atlas into addressable assets <a id="slicing"></a>

A PNG is often a *grid* of tiles or sprite frames, not one asset. `loadAtlas` (a `Renderer` method)
uploads the image once **and** carves it into addressable sub-asset slots — the ergonomic chain over
`loadPng` → `uploadAtlas` → the pure `sliceLayout`:

```cpp
#include "retropp/renderer.h"   // AtlasManifest, Renderer::loadAtlas; ingestion types come from image.h

struct AssetSlot {            // one carved sub-asset — pure geometry, no draw-state types
    std::uint16_t   tile = 0; // its top-left atlas cell (8px grid) — feed to Sprite::tile / TileCell::tile
    AssetDimensions dimensions{};
};
struct AtlasManifest {        // what loadAtlas returns
    AtlasId                atlasId{};           // the uploaded sheet's handle — the explicit projection
    std::vector<AssetSlot> slots;               // the carved assets, in read order
    int                    framesPerAnimation = 0;  // >0 only for an AnimationSeries load; else ungrouped
    ContentKind            kind = ContentKind::Single;  // what this sheet holds — filter several sheets by it
    std::size_t            tileCount() const;    // slots.size()
    const AssetSlot&       operator[](std::size_t i) const;
    std::size_t            animationCount() const;   // AnimationSeries: how many per-animation runs
    std::span<const AssetSlot> animation(std::size_t g) const;  // the g-th run; throws if g >= animationCount()
};

const AtlasManifest sheet =
    renderer.loadAtlas("assets/tiles.png", AssetDimensions::GameBoy8x8, ContentKind::Tileset);
// sheet[i].tile / sheet[i].dimensions address the i-th carved tile.
```

**Content kind** — what the image holds:

| `ContentKind` | carves into |
|---|---|
| `Single` | exactly one slot covering the whole image |
| `Tileset` | a grid of N independent tiles |
| `SpriteSeries` | a grid of N independent sprite frames |
| `SingleAnimation` | a grid of N frames of ONE animation |
| `AnimationSeries` | a grid of MULTIPLE animations, `framesPerAnimation` frames each |

Every grid kind slices **identically** ("grid of N") — the distinct names let the call site read its
intent. The two animation kinds additionally group the manifest's slots into per-animation runs
(pass `framesPerAnimation` to `loadAtlas`; `manifest.animation(g)` hands back the g-th run — see
[animation.md](animation.md)).

The manifest records its kind in `sheet.kind`. A consumer holding several sheets filters by it —
`if (sheet.kind == ContentKind::AnimationSeries)` — to pick the ones it wants without tracking which
sheet is which by hand.

**Read order** — the traversal across the grid. All eight permutations are named presets, because some
carts laid their frames in non-western orders:

```cpp
struct ReadOrder {
    enum class Fill          { Rows, Columns };          // fill a row first, or a column first
    enum class HorizontalDir { LeftToRight, RightToLeft };
    enum class VerticalDir   { TopToBottom, BottomToTop };
    // ... + the 8 named presets:
};
// ReadOrder::LeftRightThenDown (western default), RightLeftThenDown, LeftRightThenUp, RightLeftThenUp,
// TopBottomThenRight (column-major), BottomTopThenRight, TopBottomThenLeft, BottomTopThenLeft.
```

A raw `ReadOrder{ .fill, .horizontal, .vertical }` builds any combination by hand.

**On asset size + the 8px cell.** The atlas is addressed in **8px cells** — the atomic tile/sprite cell
of the whole 8/16-bit era (GB/GBC, NES, SMS, SNES, Genesis all dice art into 8×8 cells; nothing in the
paradigm is finer). That's the granularity of where an asset *sits* in the atlas, **not** a limit on its
*size*: `AssetDimensions` is flexible — an asset is any whole number of cells (8×8, 8×16, 16×16, 24×16,
64×64, non-square …), the natural sizes those consoles actually use. (Pixel-precise *placement* on
screen is separate — `Sprite::x/y` are arbitrary pixels, exactly as on hardware.) So a size that
straddles the grid (10×10, 12×7) is what's rejected, not "anything but 8×8."

**Partly-used sheets — `count`.** When a sheet has room for more cells than the art actually uses (8
cells of space, 5 real frames), pass a `count` so the manifest holds exactly the real assets instead of
trailing empties: `loadAtlas(path, size, kind, order, /*count=*/5)` carves the first 5 cells in read
order and stops. `count = 0` (the default) carves the whole grid; a `count` past the grid's capacity is
clamped to capacity (and logged); `Single` ignores it.

A **trailing partial cell is dropped** (full cells only, logged); a degenerate request (non-positive
size, asset bigger than the image, asset not a whole number of cells) yields an **empty** manifest — the
slicer never throws (load/decode/GPU failures still throw from `loadPng`/`uploadAtlas`).
`loadAtlasFromMemory` is the in-memory-bytes overload.

**Pixel-built sheets carve at upload.** `uploadAtlas` (the raw-index form) speaks the same sentence as
`loadAtlas` minus the decode: pass the carve alongside the pixels —
`renderer.uploadAtlas(indices, w, h, assetSize, kind, order, count, transparent)` — and it returns the
`AtlasManifest`. The three-argument form (`indices, w, h[, transparent]`) carves **`Single`**: the
whole image is one asset at slot 0. Every form records the sheet's **slice geometry** on the renderer —
what an [`AnimationFrame`](animation.md)'s `tile()`/`size()` resolve a slot index through — so a
pixel-built sheet animates exactly like a loaded one. One carve per sheet, declared at upload; where
only the handle is wanted, write the projection explicitly — `sheet.atlasId`. The pure
`sliceLayout(imageSize, assetSize, kind, order, count)` remains the headless form when only the slot
list is wanted.

## Transparency: structural and material

An atlas pixel can be a HOLE — discarded so the layer beneath shows through — by two independent
mechanisms. They compose: one decides *whether* an index draws at all, the other *how faintly* a drawn
pixel blends. Both the tile and sprite paths honor both.

**Structural transparency — the transparent-index set.** Per sheet, you declare which palette *indices*
are holes. It is a property of the **source** (the atlas), not the layer or the palette. The set is a
`TransparentIndices` value (from `retropp/image.h`), passed at upload time:

```cpp
const AtlasId solid  = renderer.uploadAtlas(indices, w, h).atlasId;                               // None ({}) — default
const AtlasId gbHole = renderer.uploadAtlas(indices, w, h, TransparentIndices::GameBoy).atlasId;  // {0}
const AtlasId custom = renderer.uploadAtlas(indices, w, h, TransparentIndices::of({2, 5})).atlasId;
```

| `TransparentIndices` | Set | Meaning |
|---|---|---|
| `None` | `{}` | no structural hole — every index draws (the default for every sheet) |
| `GameBoy` | `{0}` | the Game Boy OBJ convention: palette index 0 is the hole |
| `GameBoyColor` | `{0}` | identical convention |
| `of({…})` | arbitrary | the listed indices are holes |

The default is `None`: nothing is structurally transparent unless you ask. Game-Boy-style sprite art
opts its index-0 hole in with `TransparentIndices::GameBoy`. `of({…})` builds any set — indices 0–63 are
eligible (an index ≥ 64 is dropped from the set; it stays expressible via palette alpha, below). Where a
fragment's index is in the sheet's set, the shader discards it and whatever layer sits below (by `z`)
shows through.

**Material transparency — palette alpha.** A palette entry with alpha `0` is also a hole: a fragment
whose color samples a fully-transparent entry is discarded, on either path. (Alpha `1`–`254` still
alpha-blends — only an exact `0` discards.) This needs no index list — it travels with the color, so a
palette image (a color PNG; see [tiles-and-color.md](tiles-and-color.md)) whose entries carry alpha
makes its own holes.

The two are orthogonal: the index set holes an index whatever color it maps to (it works for
grayscale/indexed art with no palette alpha); the alpha discard holes a color whatever index it sits
at. Use either or both per sheet.

The runnable showcase of material (palette-alpha) transparency is [`examples/wall_cracks_demo`](../../examples/wall_cracks_demo/main.cpp): a brick
wall whose missing bricks (a palette entry at alpha 0) reveal a scrolling background through the holes,
whose weathered bricks (a partial-alpha entry) let it bleed faintly through, and whose fracture sprites
blend over the brick — every hole and blend coming from the alpha of palette images loaded with
`loadPaletteImage`.

## A worked example

Two layers from one PNG — an opaque background and, above it, the same art with index 0 holed so the
background shows through:

```cpp
const LoadedImage art = loadPng("assets/tileset.png");
const AtlasId opaque = renderer.uploadAtlas(art.indices.data(), art.width, art.height).atlasId;  // None
const AtlasId holed  =
    renderer.uploadAtlas(art.indices.data(), art.width, art.height, TransparentIndices::of({0}));
// lower z = a layer using `opaque`; higher z = a layer using `holed` → its index-0 pixels reveal the
// lower layer through the holes.
```

## Where to change things

- **Use a different art file:** point `loadPng` at another indexed/grayscale PNG; feed `indices` to
  `uploadAtlas`.
- **Make an index see-through:** pass it in the `TransparentIndices` set at upload —
  `TransparentIndices::of({n})`, or `::GameBoy` for the index-0 OBJ hole.
- **Make a color see-through:** give that palette entry alpha 0 — it discards wherever it is sampled,
  no index list needed.
- **Decode a console palette file (`.gbcpal`, etc.):** that's consumer-side — build the `Rgba8`
  palette from your asset format and `uploadPalette` it; the platform's image loader handles the index
  plane, you handle the color table.
- **RGBA / truecolor art:** load it as an image (`loadImage`) and show it on a raster layer
  (`RasterContent{.image = …}`); atlas art stays indexed/grayscale, and a truecolor PNG also decodes to
  a 16-bit color plane for palette building (see "Truecolor decodes to a color plane" above).
