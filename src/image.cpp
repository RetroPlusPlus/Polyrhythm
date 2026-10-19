#include "retropp/image.h"

#include <stdexcept>
#include <string>

#include <SDL3/SDL.h>

#include "lodepng.h"
#include "retropp/asset_policy.h"    // resolveAssetPolicy
#include "retropp/asset_registry.h"  // detail::findEmbeddedAsset

namespace retropp {

namespace {

// Read one `bits`-wide sample (bits ∈ {1,2,4,8}) from a PNG scanline, MSB-first within the byte —
// the PNG sub-byte packing order (the left-most pixel occupies the most-significant bits, each
// scanline starts on a byte boundary). `bitPos` is the sample's bit offset from the row start.
[[nodiscard]] std::uint8_t readSample(const unsigned char* row, std::size_t bitPos, unsigned bits) {
    const std::size_t byte = bitPos / 8;
    if (bits == 8) return row[byte];
    const unsigned shift = 8u - bits - static_cast<unsigned>(bitPos % 8);
    const unsigned mask  = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((row[byte] >> shift) & mask);
}

// Unpack a raw (unconverted) PNG buffer into one index byte per pixel. `channels` is the per-pixel
// sample count (1 for palette/grayscale, 2 for grayscale+alpha — the grey sample is the first channel
// and the only one we keep); `bitdepth` is the per-sample bit width. Faithful: the sample value IS the
// index, never scaled or reverse-derived from a colour.
//
// The buffer is lodepng's raw output, which packs sub-byte samples tightly across the whole image: a
// row begins where the row before it ended, not at the next byte. So a pixel's position is its flat
// bit offset from the buffer's start. At 8 bits and above a row is whole bytes and the two agree.
[[nodiscard]] std::vector<std::uint8_t> unpackIndices(const std::vector<unsigned char>& raw,
                                                      unsigned width, unsigned height,
                                                      unsigned channels, unsigned bitdepth) {
    const std::size_t bitsPerPixel = static_cast<std::size_t>(bitdepth) * channels;
    std::vector<std::uint8_t> indices(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            // The grey/palette sample is the pixel's first channel (alpha, if any, follows).
            const std::size_t pixel  = static_cast<std::size_t>(y) * width + x;
            const std::size_t bitPos = pixel * bitsPerPixel;
            indices[pixel] = readSample(raw.data(), bitPos, bitdepth);
        }
    }
    return indices;
}

// Read one `bits`-wide sample (bits ∈ {1,2,4,8,16}) from a PNG scanline as a uint16 — the map path's
// widened readSample. For 16 bits PNG stores the sample big-endian (high byte first); sub-byte and
// 8-bit samples match readSample exactly. `bitPos` is the sample's bit offset from the row start.
[[nodiscard]] std::uint16_t readSample16(const unsigned char* row, std::size_t bitPos, unsigned bits) {
    const std::size_t byte = bitPos / 8;
    if (bits == 16) return static_cast<std::uint16_t>((row[byte] << 8) | row[byte + 1]);  // PNG big-endian
    if (bits == 8) return row[byte];
    const unsigned shift = 8u - bits - static_cast<unsigned>(bitPos % 8);
    const unsigned mask  = (1u << bits) - 1u;
    return static_cast<std::uint16_t>((row[byte] >> shift) & mask);
}

// Unpack a raw (unconverted) PNG buffer into one uint16 index per pixel — the map path's widened
// unpackIndices, reading the same tightly packed buffer by the same flat bit offset. `channels` is the
// per-pixel sample count (1 for palette/grayscale, 2 for grayscale+alpha — the grey sample is the first
// channel, the only one kept); `bitdepth` the per-sample bit width. Faithful: the sample value IS the
// index, never scaled or reverse-derived.
[[nodiscard]] std::vector<std::uint16_t> unpackIndices16(const std::vector<unsigned char>& raw,
                                                         unsigned width, unsigned height,
                                                         unsigned channels, unsigned bitdepth) {
    const std::size_t bitsPerPixel = static_cast<std::size_t>(bitdepth) * channels;
    std::vector<std::uint16_t> values(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const std::size_t pixel  = static_cast<std::size_t>(y) * width + x;
            const std::size_t bitPos = pixel * bitsPerPixel;
            values[pixel] = readSample16(raw.data(), bitPos, bitdepth);
        }
    }
    return values;
}

}  // namespace

IndexGrid loadMapPngFromMemory(std::span<const std::uint8_t> bytes) {
    const unsigned char* data = bytes.data();
    const std::size_t    size = bytes.size();

    // Inspect IHDR first so a truecolour source is rejected before any pixel decode, and so the
    // colour type / bit depth route the unpack. lodepng::State IS-A LodePNGState.
    lodepng::State state;
    unsigned width = 0, height = 0;
    if (const unsigned err = lodepng_inspect(&width, &height, &state, data, size)) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }

    const LodePNGColorType colortype = state.info_png.color.colortype;
    const unsigned         bitdepth  = state.info_png.color.bitdepth;

    // A map carries INDICES, not colour — truecolour is meaningless here and rejected.
    if (colortype == LCT_RGB || colortype == LCT_RGBA) {
        throw std::runtime_error("loadMapPng: a map PNG must be grayscale or palette, not truecolour");
    }

    // Decode WITHOUT colour conversion: palette indices and grey sample values come through unscaled;
    // we unpack the (possibly sub-byte, possibly 16-bit) samples to one uint16 index per pixel.
    state.decoder.color_convert = 0;
    std::vector<unsigned char> raw;
    if (const unsigned err = lodepng::decode(raw, width, height, state, data, size)) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }

    IndexGrid grid;
    grid.width  = static_cast<int>(width);
    grid.height = static_cast<int>(height);
    const unsigned channels = (colortype == LCT_GREY_ALPHA) ? 2u : 1u;
    grid.values = unpackIndices16(raw, width, height, channels, bitdepth);
    return grid;
}

IndexGrid loadMapPng(LiteralPath path, std::optional<AssetPolicy> policy) {
    // Resolve embed-vs-load: per-call > loadMapPng's per-type default (Embed). An Embed asset decodes
    // from the bytes the build baked into the binary, keyed by its logical path; if none were baked (the
    // target was not run through the asset scan) we fall through to the disk read.
    if (resolveAssetPolicy(policy, AssetPolicy::Embed) == AssetPolicy::Embed) {
        if (const std::span<const std::uint8_t> bytes = detail::findEmbeddedAsset(path.view());
            !bytes.empty()) {
            return loadMapPngFromMemory(bytes);
        }
        detail::warnEmbedNotBaked("asset", path.view());
    }
    // LoadFromPath (or an un-baked Embed): resolve the logical path against the runtime asset root.
    const std::filesystem::path full = assetRoot() / path.c_str();
    std::vector<unsigned char> file;
    if (const unsigned err = lodepng::load_file(file, full.string())) {
        throw std::runtime_error(std::string{"loadMapPng: "} + lodepng_error_text(err));
    }
    return loadMapPngFromMemory(std::span<const std::uint8_t>(file.data(), file.size()));
}

LoadedImage loadPngFromMemory(std::span<const std::uint8_t> bytes) {
    const unsigned char* data = bytes.data();
    const std::size_t    size = bytes.size();

    // Inspect the IHDR first so a truecolour source is rejected before any pixel decode, and so the
    // colour type / bit depth route the unpack. lodepng::State IS-A LodePNGState.
    lodepng::State state;
    unsigned width = 0, height = 0;
    if (const unsigned err = lodepng_inspect(&width, &height, &state, data, size)) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }

    const LodePNGColorType colortype = state.info_png.color.colortype;
    const unsigned         bitdepth  = state.info_png.color.bitdepth;

    // Truecolour / truecolour-alpha → the RGBA path: one colour per pixel in 16-bit channels. lodepng
    // converts to canonical LCT_RGBA (filling a missing alpha with the opaque max); an 8-bit source
    // then widens ×257 (lossless — see widen8), a 16-bit source assembles each big-endian sample direct.
    if (colortype == LCT_RGB || colortype == LCT_RGBA) {
        LoadedImage image;
        image.kind   = ImageColorKind::Rgba;
        image.width  = static_cast<int>(width);
        image.height = static_cast<int>(height);
        const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        image.pixels.resize(count);

        std::vector<unsigned char> rgba;
        unsigned w2 = 0, h2 = 0;
        if (bitdepth == 16) {
            // 16-bit RGBA: lodepng emits 16-bit samples big-endian (high byte first), 8 bytes per pixel.
            if (const unsigned err = lodepng::decode(rgba, w2, h2, data, size, LCT_RGBA, 16)) {
                throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
            }
            for (std::size_t i = 0; i < count; ++i) {
                const unsigned char* s = rgba.data() + i * 8;
                image.pixels[i] = Rgba16{static_cast<std::uint16_t>((s[0] << 8) | s[1]),
                                         static_cast<std::uint16_t>((s[2] << 8) | s[3]),
                                         static_cast<std::uint16_t>((s[4] << 8) | s[5]),
                                         static_cast<std::uint16_t>((s[6] << 8) | s[7])};
            }
        } else {
            // Any sub-16-bit source: decode to 8-bit RGBA, then widen each channel ×257 into 16 bits.
            if (const unsigned err = lodepng::decode(rgba, w2, h2, data, size, LCT_RGBA, 8)) {
                throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
            }
            for (std::size_t i = 0; i < count; ++i) {
                const unsigned char* s = rgba.data() + i * 4;
                image.pixels[i] = widen(Rgba8{s[0], s[1], s[2], s[3]});
            }
        }
        return image;
    }
    // The indexed path is 8-bit indices (the R8 atlas). 16-bit grayscale would need >8-bit indices.
    if (bitdepth == 16) {
        throw std::runtime_error("loadPng: 16-bit samples are not supported on the indexed path");
    }

    // Decode WITHOUT colour conversion: the output stays in the PNG's own format, so palette indices
    // and grey sample values come through unscaled. We unpack the (possibly sub-byte) samples to one
    // index per pixel ourselves — exact for any bit depth, never reverse-derived from a colour.
    state.decoder.color_convert = 0;
    std::vector<unsigned char> raw;
    if (const unsigned err = lodepng::decode(raw, width, height, state, data, size)) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }

    LoadedImage image;
    image.kind   = ImageColorKind::Indexed;
    image.width  = static_cast<int>(width);
    image.height = static_cast<int>(height);

    const unsigned channels = (colortype == LCT_GREY_ALPHA) ? 2u : 1u;
    image.indices = unpackIndices(raw, width, height, channels, bitdepth);

    // A PLTE PNG carries its palette; grayscale carries none (the consumer supplies colour).
    if (colortype == LCT_PALETTE) {
        const unsigned char* p   = state.info_png.color.palette;
        const std::size_t    n   = state.info_png.color.palettesize;
        image.palette.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            image.palette.push_back(Rgba8{p[4 * i + 0], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]});
        }
    }

    return image;
}

LoadedImage loadPng(const std::filesystem::path& path) {
    std::vector<unsigned char> file;
    if (const unsigned err = lodepng::load_file(file, path.string())) {
        throw std::runtime_error(std::string{"loadPng: "} + lodepng_error_text(err));
    }
    return loadPngFromMemory(std::span<const std::uint8_t>(file.data(), file.size()));
}

namespace {

// The cell at position `index` of the read-order walk over a `cols` × `rows` grid — the single
// traversal truth readOrderCells and sliceSlot both resolve through, as pure arithmetic: `fill` picks
// which axis is the inner one (index splits into inner = index % innerLen, outer = index / innerLen)
// and each axis's direction runs its sequence forward or reversed. Precondition: cols > 0, rows > 0,
// index ∈ [0, cols·rows) — the callers guard.
GridCell readOrderCellAt(int cols, int rows, ReadOrder order, int index) {
    const int innerLen = order.fill == ReadOrder::Fill::Rows ? cols : rows;
    const int inner    = index % innerLen;
    const int outer    = index / innerLen;
    const int colIdx   = order.fill == ReadOrder::Fill::Rows ? inner : outer;
    const int rowIdx   = order.fill == ReadOrder::Fill::Rows ? outer : inner;
    const int col = order.horizontal == ReadOrder::HorizontalDir::LeftToRight ? colIdx : cols - 1 - colIdx;
    const int row = order.vertical == ReadOrder::VerticalDir::TopToBottom ? rowIdx : rows - 1 - rowIdx;
    return GridCell{col, row};
}

}  // namespace

std::vector<GridCell> readOrderCells(int cols, int rows, ReadOrder order, int count) {
    // Non-positive grid → nothing to walk (also guards the loop below).
    if (cols <= 0 || rows <= 0) {
        return {};
    }

    // How many cells to emit: 0 = the whole grid; a positive count caps to the first `count` in read
    // order. A count past capacity is clamped to capacity and logged — never invents cells beyond the grid.
    const int capacity = cols * rows;
    int limit = capacity;
    if (count > 0) {
        if (count > capacity) {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                        "retropp: readOrderCells asked for %d cells but the %dx%d grid holds only %d; "
                        "using all %d",
                        count, cols, rows, capacity, capacity);
        } else {
            limit = count;
        }
    }

    std::vector<GridCell> cells;
    cells.reserve(static_cast<std::size_t>(limit));
    for (int i = 0; i < limit; ++i) {
        cells.push_back(readOrderCellAt(cols, rows, order, i));
    }
    return cells;
}

std::vector<AssetSlot> sliceLayout(PixelSize imageSize, AssetDimensions assetSize,
                                   ContentKind kind, ReadOrder order, int count) {
    // Degenerate image → nothing to carve (also guards the divisions below).
    if (imageSize.width <= 0 || imageSize.height <= 0) {
        return {};
    }

    // Single: the whole image is one asset, regardless of assetSize/order/count — slot 0 spans the image.
    if (kind == ContentKind::Single) {
        return {AssetSlot{0, AssetDimensions{imageSize.width, imageSize.height}}};
    }

    // Grid (Tileset / SpriteSeries — identical carving): the asset must be a POSITIVE multiple of the
    // 8px cell grid and fit within the image on both axes, else the grid is undefined → empty.
    if (assetSize.width <= 0 || assetSize.height <= 0 ||
        assetSize.width % kAtlasCellPx != 0 || assetSize.height % kAtlasCellPx != 0 ||
        assetSize.width > imageSize.width || assetSize.height > imageSize.height) {
        return {};
    }

    const int gridCols = imageSize.width  / assetSize.width;   // floor: trailing partial column dropped
    const int gridRows = imageSize.height / assetSize.height;  // floor: trailing partial row dropped
    const int cellsAcross = imageSize.width / kAtlasCellPx;    // atlas cell stride (matches the shader)

    // Full cells only — a trailing remainder is dropped and logged, never silently claimed as covered.
    const int remainderX = imageSize.width  - gridCols * assetSize.width;
    const int remainderY = imageSize.height - gridRows * assetSize.height;
    if (remainderX != 0 || remainderY != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                    "retropp: sliceLayout dropped a %dx%d-px trailing remainder of a %dx%d image not "
                    "covered by whole %dx%d cells (%d cols x %d rows carved)",
                    remainderX, remainderY, imageSize.width, imageSize.height,
                    assetSize.width, assetSize.height, gridCols, gridRows);
    }

    // Walk the grid in read order (shared with slicePaletteImage), mapping each cell to its top-left 8px
    // atlas cell index: (pyTop/8) * cellsAcross + (pxTop/8). `count` caps the carve inside readOrderCells.
    const std::vector<GridCell> cells = readOrderCells(gridCols, gridRows, order, count);
    std::vector<AssetSlot> slots;
    slots.reserve(cells.size());
    for (const GridCell cell : cells) {
        const int pxTop = cell.col * assetSize.width;
        const int pyTop = cell.row * assetSize.height;
        const int tile  = (pyTop / kAtlasCellPx) * cellsAcross + (pxTop / kAtlasCellPx);
        slots.push_back(AssetSlot{static_cast<std::uint16_t>(tile), assetSize});
    }
    return slots;
}

std::optional<AssetSlot> sliceSlot(PixelSize imageSize, AssetDimensions assetSize,
                                   ContentKind kind, ReadOrder order, int index) {
    // Same degenerate guards as sliceLayout — a carve sliceLayout would refuse has no slots to resolve.
    if (imageSize.width <= 0 || imageSize.height <= 0 || index < 0) {
        return std::nullopt;
    }

    // Single: the whole image is one asset — slot 0 spans the image; there is no slot 1.
    if (kind == ContentKind::Single) {
        if (index != 0) return std::nullopt;
        return AssetSlot{0, AssetDimensions{imageSize.width, imageSize.height}};
    }

    if (assetSize.width <= 0 || assetSize.height <= 0 ||
        assetSize.width % kAtlasCellPx != 0 || assetSize.height % kAtlasCellPx != 0 ||
        assetSize.width > imageSize.width || assetSize.height > imageSize.height) {
        return std::nullopt;
    }

    const int gridCols = imageSize.width  / assetSize.width;
    const int gridRows = imageSize.height / assetSize.height;
    if (index >= gridCols * gridRows) {
        return std::nullopt;
    }

    // The same cell → top-left-atlas-cell mapping sliceLayout applies, for the one cell at `index`.
    const int cellsAcross = imageSize.width / kAtlasCellPx;
    const GridCell cell   = readOrderCellAt(gridCols, gridRows, order, index);
    const int pxTop = cell.col * assetSize.width;
    const int pyTop = cell.row * assetSize.height;
    const int tile  = (pyTop / kAtlasCellPx) * cellsAcross + (pxTop / kAtlasCellPx);
    return AssetSlot{static_cast<std::uint16_t>(tile), assetSize};
}

std::vector<Rgba16> slicePaletteImage(const LoadedImage& img, ReadOrder order, int count) {
    // A palette image carries COLOURS, not indices — an indexed source is a misuse (the inverse of the
    // map path's truecolour reject).
    if (img.kind != ImageColorKind::Rgba) {
        throw std::runtime_error(
            "slicePaletteImage: a palette image must be truecolour (RGBA), not indexed");
    }

    // Walk the pixels one-per-cell in read order (the SAME traversal sliceLayout uses); each cell IS a
    // pixel, so entry k is the k-th pixel in the walk. `count` caps the take inside readOrderCells.
    const std::vector<GridCell> cells = readOrderCells(img.width, img.height, order, count);
    std::vector<Rgba16> entries;
    entries.reserve(cells.size());
    for (const GridCell cell : cells) {
        const std::size_t i = static_cast<std::size_t>(cell.row) * static_cast<std::size_t>(img.width) +
                              static_cast<std::size_t>(cell.col);
        entries.push_back(img.pixels[i]);
    }
    return entries;
}

}  // namespace retropp
