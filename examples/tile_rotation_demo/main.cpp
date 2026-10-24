// Tile-rotation demo — proves that ONE corner tile and ONE edge tile, reused through the four 90°
// rotations, assemble a full bordered box. Rotation is a texture-orientation field on TileCell (and
// Sprite), the discrete sibling of flipX/flipY: it changes which source pixel is read, composing with
// the flips for all eight orientations of square art. The geometric Transform path (arbitrary-angle
// quad rotation) is separate.
//
// What it shows:
//   - A 20×18 box frame whose four corners are ONE corner tile at rotations None/Rot90/Rot180/Rot270,
//     and whose four sides are ONE edge tile at the matching rotations — built through the catalog
//     (TileCatalogEntry::rotation rides onto each emitted cell). One source slot per shape, not four.
//   - A square glyph sprite that rotates with A, so the sprite rotation path is visible.
//   - A 16×16 view of the SOURCE block (drawn once) so the green's origin is visible: an up-arrow in the
//     left 8 columns, a green neighbor in the right 8. Then two non-square (8×16) sprites that read only
//     the arrow column of that block: the LEFT stays unrotated (a clean up-arrow, direction at a glance);
//     the RIGHT rotates with A. Rot180 is dimension-safe → a clean upside-down arrow. But Rot90/Rot270
//     TRANSPOSE a non-square cell's read (the extents swap): the arrow shears and the GREEN neighbor —
//     the one you can see in the source view — bleeds in. A kept fantasy-console quirk, not a bug; use the
//     geometric transform for true quad rotation of non-square art.
//
// A cycles the sprite rotation; Select = fullscreen; close to quit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/tilemap.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kViewW = 160, kViewH = 144;
constexpr int kMapW = 20, kMapH = 18;          // 20×18 tiles fill the 160×144 viewport
constexpr int kAtlasCols = 6, kAtlasRows = 2;  // 6×2 cells: corner/edge/fill/glyph in row 0; arrow spans col 4
constexpr int kAtlasW = kAtlasCols * 8, kAtlasH = kAtlasRows * 8;

constexpr std::uint8_t kBg = 0, kBorder = 1, kFill = 2, kGlyph = 3, kOrange = 4, kGreen = 5, kHole = 7;

// The demo's input vocabulary: cycle the sprite rotation and toggle fullscreen.
enum class Action : std::uint8_t { NextOrientation, Fullscreen };

// Set one atlas pixel at cell (cellCol, cellRow), local (x, y).
void px(std::array<std::uint8_t, kAtlasW * kAtlasH>& a, int cellCol, int cellRow,
        int x, int y, std::uint8_t idx) {
    a[static_cast<std::size_t>(cellRow * 8 + y) * kAtlasW + (cellCol * 8 + x)] = idx;
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Tile Rotation Demo"},
        .window = {.title = "Polyrhythm — tile rotation (one tile, every orientation)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::NextOrientation, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Fullscreen,      {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // Build the atlas in memory. Slot 0 is a top-left corner (top row + left column = border): rotated
    // clockwise it reads as the other three corners. Slot 1 is a top edge bar (top row): rotated it reads
    // as the right/bottom/left edges. Slot 2 is a solid interior. Slot 3 is an asymmetric "F" glyph for a
    // square sprite. The arrow occupies cells (4,0)+(4,1) so an 8×16 sprite at slot 4 reads the whole arrow.
    std::array<std::uint8_t, kAtlasW * kAtlasH> art{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            px(art, 0, 0, x, y, (y == 0 || x == 0) ? kBorder : kBg);   // slot 0: top-left corner
            px(art, 1, 0, x, y, (y == 0) ? kBorder : kBg);             // slot 1: top edge bar
            px(art, 2, 0, x, y, kFill);                                // slot 2: solid interior
            px(art, 3, 0, x, y, kHole);                                // slot 3: glyph background (transparent)
        }
    }
    // Slot 3: an "F" (top bar + left stem + mid bar) — asymmetric so rotation is unmistakable.
    for (int x = 0; x < 6; ++x) px(art, 3, 0, x, 0, kGlyph);          // top bar
    for (int x = 0; x < 4; ++x) px(art, 3, 0, x, 3, kGlyph);          // mid bar
    for (int y = 0; y < 8; ++y) px(art, 3, 0, 0, y, kGlyph);          // left stem

    // Non-square showcase: a directional up-arrow in the LEFT column (cells (4,0)+(4,1), over a transparent
    // background) so its orientation reads at a glance, and a solid GREEN neighbor in the RIGHT column
    // (cells (5,0)+(5,1)). An 8×16 sprite (tile 4) reads the left column = the whole arrow. Rot180 is
    // dimension-safe → a clean upside-down arrow. Rot90/Rot270 transpose the read into a 16×8 strip that
    // reaches into the green neighbor — the arrow shears and green bleeds in: the neighbor-cell read made
    // visible while the arrow keeps the demo directional.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            px(art, 4, 0, x, y, kHole);    // arrow cells: transparent background
            px(art, 4, 1, x, y, kHole);
            px(art, 5, 0, x, y, kGreen);   // neighbor column: solid green
            px(art, 5, 1, x, y, kGreen);
        }
    // The up-arrow across the left column (8 wide × 16 tall): a head widening over the first 4 rows, then a
    // 2px shaft.
    const auto arrowPx = [&](int x, int y, std::uint8_t idx) { px(art, 4, y < 8 ? 0 : 1, x, y % 8, idx); };
    for (int y = 0; y < 16; ++y) {
        const int head = y < 4 ? y : 3;
        for (int x = 3 - head; x <= 4 + head; ++x) arrowPx(x, y, kOrange);  // arrowhead
        if (y >= 4) { arrowPx(3, y, kOrange); arrowPx(4, y, kOrange); }     // shaft
    }

    // Index 7 (kHole) is the sprite-art transparent background; the box tiles never use it.
    const AtlasId atlas = renderer.uploadAtlas(art.data(), kAtlasW, kAtlasH, TransparentIndices::of({kHole})).atlasId;

    const std::array<Rgba8, 8> palColours{{
        {28, 36, 48},    // 0 bg
        {240, 240, 250}, // 1 border
        {48, 92, 158},   // 2 fill
        {245, 210, 70},  // 3 glyph
        {240, 120, 40},  // 4 orange (arrow)
        {70, 200, 110},  // 5 green  (the neighbor column the transpose pulls in)
        {0, 0, 0},       // 6 unused
        {0, 0, 0},       // 7 transparent (never drawn)
    }};
    const PaletteId palette = renderer.uploadPalette(std::span<const Rgba8>(palColours));

    // The catalog: ONE corner slot + ONE edge slot, each declared in its four rotations, plus the fill.
    enum Id : std::uint16_t { CornerN, CornerE, CornerS, CornerW, EdgeN, EdgeE, EdgeS, EdgeW, Interior };
    TileCatalog cat;
    cat.entries = {
        {.id = CornerN, .sheet = atlas, .slot = 0, .palette = palette, .rotation = Rotation::None},   // top-left
        {.id = CornerE, .sheet = atlas, .slot = 0, .palette = palette, .rotation = Rotation::Rot90},  // top-right
        {.id = CornerS, .sheet = atlas, .slot = 0, .palette = palette, .rotation = Rotation::Rot180}, // bottom-right
        {.id = CornerW, .sheet = atlas, .slot = 0, .palette = palette, .rotation = Rotation::Rot270}, // bottom-left
        {.id = EdgeN,   .sheet = atlas, .slot = 1, .palette = palette, .rotation = Rotation::None},   // top
        {.id = EdgeE,   .sheet = atlas, .slot = 1, .palette = palette, .rotation = Rotation::Rot90},  // right
        {.id = EdgeS,   .sheet = atlas, .slot = 1, .palette = palette, .rotation = Rotation::Rot180}, // bottom
        {.id = EdgeW,   .sheet = atlas, .slot = 1, .palette = palette, .rotation = Rotation::Rot270}, // left
        {.id = Interior,.sheet = atlas, .slot = 2, .palette = palette},
    };

    // Lay out the box frame: corners, edges, interior.
    IndexGrid grid;
    grid.width  = kMapW;
    grid.height = kMapH;
    grid.values.resize(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y) {
        for (int x = 0; x < kMapW; ++x) {
            std::uint16_t id = Interior;
            const bool left = x == 0, right = x == kMapW - 1, top = y == 0, bottom = y == kMapH - 1;
            if (top && left)        id = CornerN;
            else if (top && right)  id = CornerE;
            else if (bottom && right) id = CornerS;
            else if (bottom && left)  id = CornerW;
            else if (top)    id = EdgeN;
            else if (right)  id = EdgeE;
            else if (bottom) id = EdgeS;
            else if (left)   id = EdgeW;
            grid.values[static_cast<std::size_t>(y) * kMapW + x] = id;
        }
    }

    AssembledTilemap frame_tiles;
    try {
        frame_tiles = assembleTilemap(grid, cat);
    } catch (const std::exception& e) {
        std::printf("demo: assembleTilemap failed: %s\n", e.what());
        return 1;
    }

    constexpr std::array<Rotation, 4> kCycle{Rotation::None, Rotation::Rot90, Rotation::Rot180, Rotation::Rot270};
    constexpr std::array<const char*, 4> kNames{"None", "Rot90", "Rot180", "Rot270"};
    int spriteRot = 0;

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::NextOrientation)) {
            spriteRot = (spriteRot + 1) % 4;
            std::printf("[dev] sprite rotation = %s\n", kNames[spriteRot]);
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    std::array<Sprite, 4> sprites{{{.key = "glyph"}, {.key = "block16"}, {.key = "narrowL"}, {.key = "narrowR"}}};
    loop.renderLoop([&]() {
        frame.layers.clear();

        DrawLayer tiles{.key = "Frame"};
        tiles.z       = 0;
        tiles.size    = PixelSize{kViewW, kViewH};
        tiles.content = frame_tiles.asTileContent(TileWrap::Blank);
        frame.layers.push_back(std::move(tiles));

        // Square glyph (8×8) — rotation reorients the F cleanly.
        sprites[0] = Sprite{.key = "glyph", .x = 40,  .y = 64, .size = AssetDimensions::GameBoy8x8,
                            .atlas = atlas, .tile = 3, .palette = palette, .rotation = kCycle[spriteRot]};
        // The full 16×16 SOURCE block, drawn once so the green's origin is visible: the up-arrow fills the
        // left 8 columns, a green neighbor the right 8. This is the sheet the narrow sprites read from.
        sprites[1] = Sprite{.key = "block16", .x = 72,  .y = 60, .size = AssetDimensions::Snes16x16,
                            .atlas = atlas, .tile = 4, .palette = palette, .rotation = Rotation::None};
        // Two 8×16 sprites that read only the arrow (left) column of that block: the LEFT stays at None (a
        // clean up-arrow); the RIGHT rotates. Rot180 stays a clean upside-down arrow; Rot90/Rot270 transpose
        // the read and pull the green neighbor — the one visible in the source at left — in.
        sprites[2] = Sprite{.key = "narrowL", .x = 104, .y = 60, .size = AssetDimensions::GameBoy8x16,
                            .atlas = atlas, .tile = 4, .palette = palette, .rotation = Rotation::None};
        sprites[3] = Sprite{.key = "narrowR", .x = 120, .y = 60, .size = AssetDimensions::GameBoy8x16,
                            .atlas = atlas, .tile = 4, .palette = palette, .rotation = kCycle[spriteRot]};
        DrawLayer sl{.key = "Sprites"};
        sl.z       = 1;
        sl.size    = PixelSize{kViewW, kViewH};
        sl.content = SpriteContent{sprites};
        frame.layers.push_back(std::move(sl));

        renderer.renderFrame(frame);
    });

    std::printf("tile-rotation demo — a box frame built from ONE corner tile + ONE edge tile in their four "
                "90° rotations. A = rotate the F glyph and the RIGHT non-square arrow. The wide sprite is the "
                "16×16 source (arrow + its green neighbour); the two narrow sprites read only the arrow "
                "column. Rot180 = a clean upside-down arrow; at 90°/270° the right arrow shears and the green "
                "neighbour bleeds in (the non-square transpose). Select = fullscreen. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
