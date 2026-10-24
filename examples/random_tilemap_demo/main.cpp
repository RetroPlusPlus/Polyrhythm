// Random-tilemap demo — proves the tilemap-import ergonomic works with a PROGRAMMATICALLY generated map
// (no PNG). It builds a TileCatalog of distinct tiles, then fills an NxN map by RANDOMLY picking catalog
// ids, and renders the result through the normal assembleTilemap → asTileContent path.
//
// The ergonomic point this demonstrates: the RNG is CLAMPED to the catalog's actual ids. Catalog ids are
// SPARSE 16-bit values (here spread across the range as k·6553), so a random map value is chosen by
// picking a random catalog ENTRY and taking its id — NOT a raw rand()%65536, which would almost always
// land on an id no catalog entry owns and make assembleTilemap throw std::out_of_range. So:
//   1. declare a TileCatalog (tiles built in-memory via uploadAtlas — no PNG needed);
//   2. fill an IndexGrid (width·height uint16) with randomCatalogId() per cell;
//   3. assembleTilemap(grid, catalog) → cells + deduped atlas/palette sets → asTileContent() → a layer.
// That is the whole randomized-map loop; collision, structure, biome rules, etc. would layer on top (a
// real generator constrains WHICH id goes WHERE), but the catalog-clamped-RNG → assemble → render path is
// exactly this.
//
// The catalog mixes 6 solid-color tiles + one diagonal two-tone tile in its four flip orientations (same
// slot, different flipX/flipY) — so the random mosaic also shows the catalog's palette/slot/flip carrying
// through. Static scene; A RE-ROLLS the map on demand (no per-frame randomization → no flicker, photo-
// sensitivity-safe); Select = fullscreen; close to quit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <random>
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
constexpr int kMapW = 20, kMapH = 18;     // 20×18 tiles exactly fill the 160×144 viewport
constexpr int kTileCount = 7;             // atlas slots 0..5 solid colors, slot 6 a diagonal two-tone

// The demo's input vocabulary: re-roll the map and toggle fullscreen.
enum class Action : std::uint8_t { Reroll, Fullscreen };

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Random Tilemap Demo"},
        .window = {.title = "Polyrhythm — random tilemap demo (catalog-clamped RNG)"}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Reroll,     {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::Fullscreen, {SDL_SCANCODE_BACKSPACE, PadButton::Select}},
    };
    platform.actions(map);

    // Build the tile art in memory (no PNG): a kTileCount-wide, 8-tall atlas. Slot k<6 is a solid tile of
    // palette index k; slot 6 is a diagonal split between palette index 6 and 7 (so its four flips read as
    // four distinct tiles). uploadAtlas addresses each 8×8 cell by slot.
    constexpr int kAtlasW = 8 * kTileCount, kAtlasH = 8;
    std::array<std::uint8_t, static_cast<std::size_t>(kAtlasW) * kAtlasH> art{};
    for (int ty = 0; ty < 8; ++ty) {
        for (int slot = 0; slot < kTileCount; ++slot) {
            for (int tx = 0; tx < 8; ++tx) {
                const std::uint8_t idx =
                    slot < 6 ? static_cast<std::uint8_t>(slot)
                             : static_cast<std::uint8_t>(tx + ty < 8 ? 6 : 7);  // diagonal two-tone
                art[static_cast<std::size_t>(ty) * kAtlasW + (slot * 8 + tx)] = idx;
            }
        }
    }
    const AtlasId atlas = renderer.uploadAtlas(art.data(), kAtlasW, kAtlasH).atlasId;

    // One 8-color palette; a tile's art index selects its color within it.
    const std::array<Rgba8, 8> palColours{{{220, 60, 60}, {60, 200, 90}, {70, 120, 230}, {235, 200, 70},
                                           {200, 90, 210}, {80, 210, 210}, {245, 130, 40}, {235, 235, 245}}};
    const PaletteId palette = renderer.uploadPalette(std::span<const Rgba8>(palColours));

    // The catalog: 10 distinct tiles, ids SPARSE across the 16-bit range (k·6553). The four slot-6 entries
    // are the same diagonal tile in its four flip orientations — same slot, different flipX/flipY.
    const auto sparseId = [](int k) { return static_cast<std::uint16_t>(k * 6553); };
    TileCatalog cat;
    cat.entries = {
        {.id = sparseId(0), .sheet = atlas, .slot = 0, .palette = palette},
        {.id = sparseId(1), .sheet = atlas, .slot = 1, .palette = palette},
        {.id = sparseId(2), .sheet = atlas, .slot = 2, .palette = palette},
        {.id = sparseId(3), .sheet = atlas, .slot = 3, .palette = palette},
        {.id = sparseId(4), .sheet = atlas, .slot = 4, .palette = palette},
        {.id = sparseId(5), .sheet = atlas, .slot = 5, .palette = palette},
        {.id = sparseId(6), .sheet = atlas, .slot = 6, .palette = palette},
        {.id = sparseId(7), .sheet = atlas, .slot = 6, .palette = palette, .flipX = true},
        {.id = sparseId(8), .sheet = atlas, .slot = 6, .palette = palette, .flipY = true},
        {.id = sparseId(9), .sheet = atlas, .slot = 6, .palette = palette, .flipX = true, .flipY = true},
    };

    // RNG CLAMPED to the catalog: pick a random ENTRY and take its (sparse) id. A raw rand()%65536 would
    // almost never hit a valid id — this is the "randomize within the tile catalog" ergonomic.
    std::mt19937 rng{0xC0FFEEu};
    const auto randomCatalogId = [&]() -> std::uint16_t {
        std::uniform_int_distribution<std::size_t> pick(0, cat.entries.size() - 1);
        return cat.entries[pick(rng)].id;
    };
    // Fill an NxN IndexGrid with catalog ids, then assemble it.
    const auto rollMap = [&]() -> IndexGrid {
        IndexGrid g;
        g.width  = kMapW;
        g.height = kMapH;
        g.values.resize(static_cast<std::size_t>(kMapW) * kMapH);
        for (std::uint16_t& v : g.values) v = randomCatalogId();
        return g;
    };

    AssembledTilemap assembled;
    try {
        assembled = assembleTilemap(rollMap(), cat);
    } catch (const std::exception& e) {
        std::printf("demo: assembleTilemap failed: %s\n", e.what());
        return 1;
    }

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Reroll)) {
            assembled = assembleTilemap(rollMap(), cat);  // re-roll on demand (static otherwise)
            std::printf("[dev] re-rolled the map (%d×%d, %zu catalog tiles)\n",
                        assembled.widthInTiles, assembled.heightInTiles, cat.entries.size());
        }
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "RandomTilemap"};
        layer.z       = 0;
        layer.size    = PixelSize{kViewW, kViewH};
        layer.content = assembled.asTileContent(TileWrap::Blank);  // finite map fills the viewport
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    std::printf("random-tilemap demo — a %d×%d map randomly filled from a %zu-tile catalog (RNG clamped to "
                "the catalog's sparse 16-bit ids). A = re-roll; Select = fullscreen. Close to quit.\n",
                kMapW, kMapH, cat.entries.size());
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
