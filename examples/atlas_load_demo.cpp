// Atlas-load demo — the EXHAUSTIVE visual companion to the headless slicer suite
// (tests/atlas_slice_test.cpp). It loadAtlases committed, numbered, license-clean indexed PNGs and
// walks the whole ingestion matrix: every image arrangement × both content kinds × all 8 read orders.
//
// Each source cell is an 8×8 tile showing its own NATURAL-reading-order index as a tiny digit (the
// digit == the atlas tile index under LeftRightThenDown). The demo draws, per selection:
//   • TOP  — the SOURCE image as one whole-image sprite, so you see the cells in their natural grid
//            positions (0,1,2,… left-to-right, top-to-bottom).
//   • BELOW — the CARVED slots laid in SLOT ORDER (slot 0 leftmost). The digit sequence below reads
//            the active read order directly: e.g. RightLeftThenDown over the 3×2 grid lays 2 1 0 5 4 3.
// The carved sequence is placed either as a TileContent row (Tileset kind) or as Sprites
// (SpriteSeries kind) — the SAME slots feed both paths, proving the two kinds carve identically.
//
// Walk the matrix:
//   ← / →      step the read order  (all 8 ReadOrder presets; ignored by the Single arrangement)
//   ↑ / ↓      step the arrangement (Single / 1-D h / 1-D v / square / 3×2 / 2×3)
//   X (pad A)  toggle the content kind / placement path (Tileset-as-tiles ↔ SpriteSeries-as-sprites)
// The active (arrangement × kind × order) + the carved tile sequence print to the terminal on each
// change (there is no on-screen text renderer — the same printf-label convention every demo uses).
//
// This is one of the runnable example hosts: it keeps the live SdlPlatform/Renderer + loadAtlas path
// compiling and linking on every CI platform even though CI never opens the window; the slicer math
// itself is proven headlessly (all 8 orders + every arrangement) in atlas_slice_test.cpp.

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
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
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

// The demo's vocabulary: steppers over the (arrangement × kind × order × count) selection.
enum class Action : std::uint8_t {
    NextOrder, PrevOrder, NextArrangement, PrevArrangement, ToggleKind, CycleCount,
};

// One arrangement: a committed source image + how the demo slices it. `single` forces
// ContentKind::Single (1 slot = the whole image); the others follow the kind toggle.
struct Arrangement {
    const char* name;
    const char* file;
    PixelSize   size;
    bool        single;
};

constexpr std::array<Arrangement, 6> kArrangements{{
    {"Single (whole image)", "atlas_grid_3x2.png", {24, 16}, true},
    {"1-D horizontal strip", "atlas_strip_h.png",  {16, 8},  false},
    {"1-D vertical strip",   "atlas_strip_v.png",  {8, 16},  false},
    {"square grid 2x2",      "atlas_grid_2x2.png", {16, 16}, false},
    {"non-square grid 3x2",  "atlas_grid_3x2.png", {24, 16}, false},
    {"non-square grid 2x3",  "atlas_grid_2x3.png", {16, 24}, false},
}};

struct NamedOrder { const char* name; ReadOrder order; };

constexpr std::array<NamedOrder, 8> kOrders{{
    {"LeftRightThenDown",  ReadOrder::LeftRightThenDown},
    {"RightLeftThenDown",  ReadOrder::RightLeftThenDown},
    {"LeftRightThenUp",    ReadOrder::LeftRightThenUp},
    {"RightLeftThenUp",    ReadOrder::RightLeftThenUp},
    {"TopBottomThenRight", ReadOrder::TopBottomThenRight},
    {"BottomTopThenRight", ReadOrder::BottomTopThenRight},
    {"TopBottomThenLeft",  ReadOrder::TopBottomThenLeft},
    {"BottomTopThenLeft",  ReadOrder::BottomTopThenLeft},
}};

// Locate a committed asset next to the executable (CMake copies examples/assets there post-build).
std::string assetPath(const char* name) {
    const char* base = SDL_GetBasePath();  // SDL-owned, do not free
    return (base ? std::string{base} : std::string{}) + "assets/" + name;
}

// This demo loads from a RUNTIME table of filenames (a.file is a variable, never a literal), so it reads
// each file's bytes itself and uploads via loadAtlasFromMemory — loadAtlas's literal-path form is for
// build-managed assets; a runtime-determined path goes through the byte path (see assets-and-embedding.md).
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Atlas Load Demo"},
        .window = {.title = "Polyrhythm — atlas-load demo (slice + read order)"}};

    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // X / pad A toggles the kind, Z / pad B steps the count cap; the directional preset (arrows +
    // WASD + d-pad) steps the arrangement (up/down) and the read order (left/right).
    ActionMap map{
        {Action::ToggleKind, {SDL_SCANCODE_X, PadButton::FaceSouth}},
        {Action::CycleCount, {SDL_SCANCODE_Z, PadButton::FaceEast}},
    };
    map.add(presets::directional(Action::NextArrangement, Action::PrevArrangement,
                                 Action::PrevOrder, Action::NextOrder));
    platform.actions(map);

    // Upload each unique source image ONCE (no eviction in the renderer), keyed by filename, and keep
    // its AtlasId. Re-slicing for a different order/kind is a pure sliceLayout call against the same
    // atlas — no re-upload — so the demo switches selections without leaking GPU textures.
    std::map<std::string, AtlasId> atlasByFile;
    try {
        for (const Arrangement& a : kArrangements) {
            if (atlasByFile.find(a.file) == atlasByFile.end()) {
                // Any kind/order works just to upload + obtain the handle; the slots are recomputed live.
                const AtlasManifest m = renderer.loadAtlasFromMemory(
                    readFile(assetPath(a.file)), AssetDimensions::GameBoy8x8, ContentKind::Tileset);
                atlasByFile.emplace(a.file, m.atlasId);
            }
        }
    } catch (const std::exception& e) {
        std::printf("atlas-load demo: could not load an asset: %s\n", e.what());
        return 1;
    }

    // One palette matching the generator (examples/assets/gen_atlas_demo_assets.py): index 0 = dark
    // corner-notch marker, 1 = white digit, 2..7 = one distinct hue per ordinal 0..5.
    const std::array<Rgba8, 8> colors{{
        {30, 30, 36},     {240, 240, 245},
        {200, 70, 60},    {220, 140, 60}, {210, 200, 70},
        {90, 180, 90},    {70, 130, 210}, {160, 100, 200},
    }};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(colors));

    // Selection state.
    int  arrIdx   = 0;
    int  orderIdx = 0;
    int  countCap = 0;     // 0 = carve the whole grid; 1..6 caps to the first N cells (count param)
    bool sprites  = true;  // placement/kind: true = SpriteSeries-as-sprites, false = Tileset-as-tiles

    // Recompute the active carved slots and announce the selection. Single forces ContentKind::Single
    // (1 whole-image slot, sprite-placed regardless of the toggle); grids follow the kind toggle.
    // `countCap` (0 = all) caps how many cells are carved — a partly-used sheet keeps only its real
    // frames instead of trailing empties.
    std::vector<AssetSlot> slots;
    auto refresh = [&] {
        const Arrangement& a = kArrangements[static_cast<std::size_t>(arrIdx)];
        const ContentKind kind = a.single ? ContentKind::Single
                                           : (sprites ? ContentKind::SpriteSeries : ContentKind::Tileset);
        slots = sliceLayout(a.size, AssetDimensions::GameBoy8x8, kind,
                            kOrders[static_cast<std::size_t>(orderIdx)].order, countCap);
        const char* kindName = a.single ? "Single"
                                         : (sprites ? "SpriteSeries (sprites)" : "Tileset (tiles)");
        const std::string countLabel = a.single ? "n/a" : (countCap == 0 ? "all" : std::to_string(countCap));
        std::printf("\n[%s]  kind=%s  order=%s  count=%s  → %zu slot(s): ",
                    a.name, kindName, kOrders[static_cast<std::size_t>(orderIdx)].name,
                    countLabel.c_str(), slots.size());
        for (const AssetSlot& s : slots) std::printf("%u ", s.tile);
        std::printf("\n");
    };
    refresh();

    constexpr auto kLabels = std::to_array<std::pair<Action, const char*>>({
        {Action::NextArrangement, "NextArrangement"}, {Action::PrevArrangement, "PrevArrangement"},
        {Action::PrevOrder, "PrevOrder"}, {Action::NextOrder, "NextOrder"},
        {Action::ToggleKind, "ToggleKind"}, {Action::CycleCount, "CycleCount"},
    });

    loop.simTick([&](const InputState& in) {
        for (const auto& [action, name] : kLabels) {
            if (in.justPressed(action)) std::printf("press %s\n", name);
        }
        bool changed = false;
        if (in.justPressed(Action::NextOrder)) { orderIdx = (orderIdx + 1) % 8; changed = true; }
        if (in.justPressed(Action::PrevOrder)) { orderIdx = (orderIdx + 7) % 8; changed = true; }
        // Stepping arrangement resets the count cap to "all" — a value carried from a bigger grid would
        // just clamp on a smaller one (and look stuck), so each arrangement starts fresh.
        if (in.justPressed(Action::NextArrangement)) { arrIdx = (arrIdx + 1) % static_cast<int>(kArrangements.size()); countCap = 0; changed = true; }
        if (in.justPressed(Action::PrevArrangement)) { arrIdx = (arrIdx + static_cast<int>(kArrangements.size()) - 1) % static_cast<int>(kArrangements.size()); countCap = 0; changed = true; }
        if (in.justPressed(Action::ToggleKind))      { sprites = !sprites; changed = true; }
        // CycleCount steps the count cap 0(all) → 1 → … → capacity → 0, capped to THIS arrangement's real
        // cell count so every press visibly changes the carved row. Single has one slot, so count is n/a there.
        if (in.justPressed(Action::CycleCount)) {
            const Arrangement& a = kArrangements[static_cast<std::size_t>(arrIdx)];
            if (!a.single) {
                const int capacity = (a.size.width / kAtlasCellPx) * (a.size.height / kAtlasCellPx);
                countCap = (countCap + 1) % (capacity + 1);
                changed = true;
            }
        }
        if (changed) refresh();
    });

    // The game owns the draw state; rebuilt each frame from the current selection (static — no motion).
    // Persistent backing for the spans the draw state references during renderFrame().
    FrameDrawState         frame;
    std::vector<Sprite>    refSprites;     // the source image shown whole (z=10)
    std::vector<Sprite>    carvedSprites;  // the carved slots as sprites (SpriteSeries path, z=20)
    std::vector<TileCell>  carvedCells;    // the carved slots as a tile row (Tileset path, z=20)

    loop.renderLoop([&]() {
        const Arrangement& a = kArrangements[static_cast<std::size_t>(arrIdx)];
        const AtlasId atlas = atlasByFile.at(a.file);
        frame.layers.clear();

        // z=10 — the source image, whole, near the top-center (one sprite reading the full image rect).
        refSprites.clear();
        refSprites.push_back(Sprite{.key = "source", .x = (160 - a.size.width) / 2, .y = 12,
                                    .size = AssetDimensions{a.size.width, a.size.height}, .atlas = atlas,
                                    .tile = 0, .palette = pal});
        // Stable, unique per-slot keys (required + unique frame-wide; these grid sprites don't interpolate),
        // built once so the string_views stay valid.
        static const std::vector<std::string> slotKeys = [] {
            std::vector<std::string> v; v.reserve(256);
            for (int k = 0; k < 256; ++k) v.push_back("slot" + std::to_string(k));
            return v;
        }();
        DrawLayer ref{.key = "source"};
        ref.z       = 10;
        ref.size    = PixelSize{160, 144};
        ref.content = SpriteContent{.sprites = std::span<const Sprite>(refSprites)};
        frame.layers.push_back(std::move(ref));

        // z=20 — the carved slots in slot order. Single is always sprite-placed (its one slot is the
        // whole image, larger than a tile cell); grids honor the toggle.
        const int n = static_cast<int>(slots.size());
        if (a.single || sprites) {
            carvedSprites.clear();
            if (a.single) {
                // The one whole-image slot, centered below the source (identical to it — slot 0 = image).
                carvedSprites.push_back(Sprite{.key = slotKeys[0], .x = (160 - slots[0].dimensions.width) / 2, .y = 84,
                                               .size = slots[0].dimensions, .atlas = atlas,
                                               .tile = slots[0].tile, .palette = pal});
            } else {
                constexpr int pitch = 12;  // 8px cell + 4px gap so the sequence reads clearly
                const int startX = (160 - (n * pitch - 4)) / 2;
                for (int i = 0; i < n; ++i) {
                    carvedSprites.push_back(Sprite{.key = slotKeys[static_cast<std::size_t>(i)],
                                                   .x = startX + i * pitch, .y = 84,
                                                   .size = slots[static_cast<std::size_t>(i)].dimensions,
                                                   .atlas = atlas,
                                                   .tile = slots[static_cast<std::size_t>(i)].tile, .palette = pal});
                }
            }
            DrawLayer carved{.key = "carved"};
            carved.z       = 20;
            carved.size    = PixelSize{160, 144};
            carved.content = SpriteContent{.sprites = std::span<const Sprite>(carvedSprites)};
            frame.layers.push_back(std::move(carved));
        } else {
            // Tileset path: a contiguous N×1 tile row, finite (TileWrap::Blank), positioned by scroll
            // so it sits centered below the source and the rest of the layer is transparent.
            carvedCells.clear();
            for (int i = 0; i < n; ++i) {
                carvedCells.push_back(TileCell{.atlas = atlas,
                                               .tile = slots[static_cast<std::size_t>(i)].tile, .palette = pal});
            }
            const int startX = (160 - n * 8) / 2;
            DrawLayer carved{.key = "carved"};
            carved.z       = 20;
            carved.size    = PixelSize{160, 144};
            carved.scroll  = LayerScroll{-startX, -84};  // world (0,0) lands at screen (startX, 84)
            carved.content = TileContent{.widthInTiles = n, .heightInTiles = 1,
                                         .cells = std::span<const TileCell>(carvedCells),
                                         .wrap = TileWrap::Blank};
            frame.layers.push_back(std::move(carved));
        }

        renderer.renderFrame(frame);
    });

    std::printf("atlas-load demo — top row: the source grid (numbered cells); bottom row: the carved "
                "slots in slot order, whose digits read the active read order.\n");
    std::printf("  Left/Right = read order (8), Up/Down = arrangement (6), X (pad A) = Tileset/SpriteSeries, "
                "Z (pad B) = count cap (all/1..N for the grid; n/a on Single). Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
