// Atlas basics demo — the SIMPLE, transparent companion to atlas_load_demo. Where that one
// walks the whole ingestion matrix (every arrangement × kind × read order) and needs lookup tables to
// do it, THIS demo shows the one canonical flow a developer actually writes, top to bottom, with
// nothing in the way:
//
//   1. loadAtlas(path, assetSize, kind)  → an AtlasManifest: the uploaded atlas + the carved slots.
//   2. manifest[i].tile / manifest[i].dimensions  → address the i-th tile/frame. No hand-computed
//      grid indices, no tables.
//   3. make a Sprite from each slot and place it.
//
// It loads ONE 6-tile sheet (examples/assets/atlas_grid_3x2.png — six 8×8 cells numbered 0..5), slices
// it left-to-right, and lays the six carved tiles out in a row beneath the whole source image. Static
// scene, no input beyond the close button — read the source, not the controls.
//
// Like the other example hosts it keeps the live SdlPlatform/Renderer + loadAtlas path compiling and
// linking on every CI platform; it is dev-run only (CI has no display). The slicer math is proven
// headlessly in tests/atlas_slice_test.cpp.

#include <array>
#include <cstdio>
#include <deque>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/image.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

using namespace retropp;

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Atlas Basics Demo"},
        .window = {.title = "Polyrhythm — atlas basics"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // ── The whole point: load a sprite sheet and get back addressable pieces ──────────────────────
    // One call decodes the PNG, uploads it as ONE atlas, and slices it into 8×8 tiles read
    // left-to-right (the default order). `sheet` is an AtlasManifest: `sheet.atlas` is the GPU handle,
    // `sheet.tileCount()` is how many tiles were carved, and `sheet[i]` is the i-th tile's slot.
    AtlasManifest sheet;
    try {
        sheet = renderer.loadAtlas("assets/atlas_grid_3x2.png",
                                   AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                                   ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::GameBoy);
    } catch (const std::exception& e) {
        std::printf("atlas basics: could not load the sheet: %s\n", e.what());
        return 1;
    }
    std::printf("loaded a sheet of %zu tiles. tile indices, in read order: ", sheet.tileCount());
    for (std::size_t i = 0; i < sheet.tileCount(); ++i) std::printf("%u ", sheet[i].tile);
    std::printf("\n");

    // A palette matching the sheet's authored colors (index 0 marker, 1 white, 2..7 per-cell hues).
    const std::array<Rgba8, 8> colors{{
        {30, 30, 36},   {240, 240, 245},
        {200, 70, 60},  {220, 140, 60}, {210, 200, 70},
        {90, 180, 90},  {70, 130, 210}, {160, 100, 200},
    }};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colors));

    // Lay a manifest's slots out in a centered row of sprites at height `y`. This is the canonical use
    // of a manifest: walk the slots, and for each make a Sprite taking its `tile` and `size` straight
    // from the slot, naming the sheet's atlas + palette directly. (In a real game you'd instead, say,
    // animate by setting one sprite's `tile` to sheet[frame].tile on a timer.)
    auto rowOfSlots = [&](const std::vector<AssetSlot>& slots, int y, std::string_view prefix) {
        static std::deque<std::string> keyPool;  // stable storage for the sprites' required unique keys
        std::vector<Sprite> row;
        const int n      = static_cast<int>(slots.size());
        const int pitch  = 14;                            // 8px tile + 6px gap
        const int startX = (160 - (n * pitch - 6)) / 2;   // center the row
        for (int i = 0; i < n; ++i) {
            Sprite s{.key = keyPool.emplace_back(std::string(prefix) + std::to_string(i))};
            s.x       = startX + i * pitch;
            s.y       = y;
            s.size    = slots[static_cast<std::size_t>(i)].dimensions;  // the slot's dimensions
            s.tile    = slots[static_cast<std::size_t>(i)].tile;        // the slot's atlas cell
            s.atlas   = sheet.atlasId;                                    // the sheet it draws from
            s.palette = pal;                                            // the palette coloring it
            row.push_back(s);
        }
        return row;
    };

    // ── Two carved rows ──────────────────────────────────────────────────────────────────────────
    // (1) every tile in the sheet (all six).
    const std::vector<Sprite> allRow = rowOfSlots(sheet.slots, 60, "all");

    // (2) the SAME sheet, but suppose only its first 4 cells are real frames. Re-carve the atlas we
    // already uploaded — no second load — passing count = 4, so the manifest stops at 4 instead of
    // carving all six. `sliceLayout` is the pure slicer loadAtlas calls under the hood; pairing its
    // slots with the existing sheet.atlas re-slices without re-uploading.
    const std::vector<AssetSlot> firstFour =
        sliceLayout(PixelSize{24, 16}, AssetDimensions::GameBoy8x8, ContentKind::Tileset,
                    ReadOrder::LeftRightThenDown, /*count=*/4);
    const std::vector<Sprite> countRow = rowOfSlots(firstFour, 96, "count");
    std::printf("count = 4 keeps only the first 4 of %zu cells\n", sheet.tileCount());

    // For reference, also show the whole source image (one sprite reading the full sheet) up top, so
    // you can see the six numbered cells in their original grid next to the carved rows.
    const std::array<Sprite, 1> sourceImage{Sprite{.key = "srcImg", .x = (160 - 24) / 2, .y = 16,
                                                   .size = AssetDimensions{24, 16}, .atlas = sheet.atlasId,
                                                   .tile = 0, .palette = pal}};

    // Static scene — only the close button is handled.
    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer source{.key = "source"};
        source.z       = 10;
        source.size    = PixelSize{160, 144};
        source.content = SpriteContent{.sprites = std::span<const Sprite>(sourceImage)};
        frame.layers.push_back(std::move(source));

        DrawLayer all{.key = "all-tiles"};
        all.z       = 20;
        all.size    = PixelSize{160, 144};
        all.content = SpriteContent{.sprites = std::span<const Sprite>(allRow)};
        frame.layers.push_back(std::move(all));

        DrawLayer capped{.key = "count-4"};
        capped.z       = 30;
        capped.size    = PixelSize{160, 144};
        capped.content = SpriteContent{.sprites = std::span<const Sprite>(countRow)};
        frame.layers.push_back(std::move(capped));

        renderer.renderFrame(frame);
    });

    std::printf("atlas basics — top: the source sheet; middle: all its tiles carved via loadAtlas; "
                "bottom: the same sheet re-sliced with count = 4 (only the first four). Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
