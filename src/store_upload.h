#pragma once

#include <cstddef>

// Where appended store content lands, and how much room a store texture is given.
//
// The renderer's palette and atlas stores each hold everything a game has uploaded so far, so an upload
// appends to the end, a palette's colors change over its own entries, and each writes only its own run.
// These decide the destination rectangle for that write, and how large a store texture is allocated when
// it has to be recreated. Engine-internal, and pure: a wrong rectangle writes correct bytes to the wrong
// place without failing, so the arithmetic is decided in one place and exercised on its own.

namespace retropp::detail {

// A destination rectangle in a store texture, in texels.
struct StoreRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    friend bool operator==(const StoreRect&, const StoreRect&) noexcept = default;
};

// Where a run of palette colors written at a flat offset lands. Colors pack flat and rows lie end to end,
// so a run that reaches the store's right edge continues on the next row — one run, two destinations.
struct PaletteAppend {
    StoreRect head{};
    StoreRect wrapped{};  // w == 0 when the run fits on one row
    [[nodiscard]] constexpr bool wraps() const noexcept { return wrapped.w > 0; }
};

// The palette store texture's row width, in colours. The store is a FLAT array of palette colours wrapped
// into a 2-D texture this many wide; a palette's flat offset + a colour index address the texel at
// (flat % W, flat / W). Palettes pack contiguously (no per-palette padding) and may straddle rows; only
// the final row is padded out to W. The store holds as many rows as the palettes need, so palette
// capacity is W × maxTextureHeight — arbitrary for any real use (no per-palette colour cap). 16384 keeps
// the height minimal for typical palettes, and a row of 16-bit RGBA is 131072 B, which satisfies backend
// upload-pitch alignment.
inline constexpr int kPaletteStoreWidth = 16384;

// The rows `colors` flat colours occupy in a `width`-wide store. At least one, so the texture a shader
// binds has a row even before anything is uploaded.
[[nodiscard]] constexpr int paletteStoreRows(std::size_t colors, int width) noexcept {
    const std::size_t w    = static_cast<std::size_t>(width);
    const std::size_t rows = (colors + w - 1) / w;
    return rows < 1u ? 1 : static_cast<int>(rows);
}

// Whether a run of `count` colors written at flat offset `first` lands within two rows, which is what
// paletteAppend describes. A run long enough to cover a whole row is written by recreating the store.
[[nodiscard]] constexpr bool paletteAppendFitsTwoRows(std::size_t first, std::size_t count,
                                                      int width) noexcept {
    const std::size_t w = static_cast<std::size_t>(width);
    return count <= (w - first % w) + w;
}

// Where `count` colors written at flat offset `first` land in a `width`-wide store: the run starts at
// column `first % width` of row `first / width` and continues onto the next row at column 0 when it
// reaches the edge. Empty for an empty run.
[[nodiscard]] constexpr PaletteAppend paletteAppend(std::size_t first, std::size_t count,
                                                    int width) noexcept {
    PaletteAppend where{};
    if (count == 0) return where;
    const std::size_t w         = static_cast<std::size_t>(width);
    const std::size_t x         = first % w;
    const std::size_t y         = first / w;
    const std::size_t onThisRow = count < w - x ? count : w - x;
    where.head                  = StoreRect{.x = static_cast<int>(x),
                                            .y = static_cast<int>(y),
                                            .w = static_cast<int>(onThisRow),
                                            .h = 1};
    if (onThisRow < count)
        where.wrapped = StoreRect{.x = 0,
                                  .y = static_cast<int>(y + 1),
                                  .w = static_cast<int>(count - onThisRow),
                                  .h = 1};
    return where;
}

// The rows an atlas of `width` × `height` occupies once appended at row `top`: an atlas is written
// left-aligned over its own rows, and every texel a shader resolves for that sheet is inside them.
[[nodiscard]] constexpr StoreRect atlasRect(int top, int width, int height) noexcept {
    return StoreRect{.x = 0, .y = top, .w = width, .h = height};
}

// The one texel atlas `index` occupies in the region table, which is a single row of texels.
[[nodiscard]] constexpr StoreRect atlasRegionTexel(int index) noexcept {
    return StoreRect{.x = index, .y = 0, .w = 1, .h = 1};
}

// Whether an atlas of `width`, stacked to bring the store's used height to `totalHeight`, fits the
// texture already allocated. A wider atlas changes the stride of every row, so it is written by
// recreating the store rather than by appending.
[[nodiscard]] constexpr bool atlasFitsStore(int width, int totalHeight, int capacityWidth,
                                            int capacityHeight) noexcept {
    return width <= capacityWidth && totalHeight <= capacityHeight;
}

// The room to allocate for a store that has to hold `needed`: twice that, so a run of appends recreates
// the texture a handful of times rather than once per upload.
[[nodiscard]] constexpr int grownCapacity(int needed) noexcept {
    return needed < 1 ? 1 : needed * 2;
}

}  // namespace retropp::detail
