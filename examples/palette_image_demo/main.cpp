// Palette-image demo — the visual companion to the headless palette slicer suite
// (tests/palette_image_slice_test.cpp), and the sibling of the atlas-load demo. It loads 16-bit-RGBA
// color PNGs ONE PIXEL PER PALETTE ENTRY via Renderer::loadPaletteImage and shows the things palette
// images exist for:
//
//   1. 16-bit color + material transparency. Both sources are 16-bit-per-channel with a real alpha
//      channel. The 4x4 grid's alpha falls per row (opaque → fully transparent); the ramp is one hue
//      with alpha ramped 0 → full. Drawn over a checker background, the alpha reads directly: a
//      translucent entry dims the checker through it, a fully-transparent entry shows it untouched.
//   2. Both asset policies. The grid is loaded Embed (its bytes are BAKED into this binary); the ramp
//      is loaded LoadFromPath (COPIED beside the binary, read from disk at runtime). Both render —
//      "embedded works AND on-disk works" — printed at startup.
//   3. Every read order. The grid is sliced in all 8 ReadOrders (one PaletteId each). Its 16 entries
//      draw as swatches in STORE order (entry 0 top-left, left-to-right then down), so
//      LeftRightThenDown reproduces the source layout and any other order visibly permutes it — step
//      ← / → to walk the orders.
//   4. Structural vs material transparency on swatch 0. Press A to toggle the swatch sheet's
//      transparent-index set between None ({}) and GameBoy ({0}). With None, swatch 0 (palette index 0)
//      draws its palette entry — color and alpha — like every other swatch. With GameBoy, index 0 is a
//      structural HOLE and swatch 0 shows the checker behind it regardless of the palette. This is the
//      difference between material transparency (the entry's own alpha) and structural transparency (the
//      sheet's index set), side by side on the same swatch.
//
// Each swatch is a block of a single palette index drawn through the loaded palette, so swatch i shows
// entry i (color + alpha) of the current order's palette. There is no on-screen text (the printf-label
// convention every demo uses). One of the runnable example hosts: it keeps the live
// SdlPlatform/Renderer + loadPaletteImage path compiling and linking on every CI platform even though
// CI never opens the window; the slicer math (all 8 orders, 16-bit + alpha) is proven headlessly in
// palette_image_slice_test.cpp.

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <vector>

#include "retropp/asset_policy.h"
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

// The demo's input vocabulary: step through the read orders and toggle swatch 0's structural hole.
enum class Action : std::uint8_t { NextOrder, PrevOrder, ToggleHole };

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

// A swatch atlas: 16 horizontally-stacked 16x16 blocks, block i filled entirely with index value i, so
// a 16x16 sprite at block i drawn through a palette shows that palette's entry i. 256x16 px = a 32x2
// grid of 8px cells; block i's top-left 8px cell index is i*2.
constexpr int kBlocks = 16;
constexpr int kBlockPx = 16;

AtlasId uploadSwatchAtlas(Renderer& r, TransparentIndices transparent) {
    constexpr int w = kBlocks * kBlockPx;  // 256
    constexpr int h = kBlockPx;            // 16
    std::vector<std::uint8_t> idx(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            idx[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(x / kBlockPx);
        }
    }
    return r.uploadAtlas(idx.data(), w, h, transparent).atlasId;
}

// Block i's sprite tile = its top-left 8px cell index (i*2 across a 32-cell-wide atlas).
std::uint16_t blockTile(int i) { return static_cast<std::uint16_t>(i * (kBlockPx / kAtlasCellPx)); }

}  // namespace

int main() {

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Palette Image Demo"},
        .window = {.title = "Polyrhythm — palette-image demo (policies + read order)"}};
    EngineConfig::setActive(config);  // make it the active config — the bare ctors below inherit it

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::NextOrder,  {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, PadButton::DpadRight}},
        {Action::PrevOrder,  {SDL_SCANCODE_LEFT, SDL_SCANCODE_A, PadButton::DpadLeft}},
        {Action::ToggleHole, {SDL_SCANCODE_X, PadButton::FaceSouth}},
    };
    platform.actions(map);

    // The same swatch art uploaded twice: once with no structural hole (None), once with the Game Boy
    // {0} hole. Pressing A swaps which the swatches draw from, so swatch 0 toggles between drawing its
    // palette entry and being a structural hole. The background always uses the None sheet (the
    // transparent-index set is per sheet and read by the tile path too — a {0} background would punch
    // holes in the checker).
    const AtlasId atlasNone = uploadSwatchAtlas(renderer, TransparentIndices::None);
    const AtlasId atlasGB   = uploadSwatchAtlas(renderer, TransparentIndices::GameBoy);

    // A checker background so the palettes' ALPHA reads as transparency: a translucent swatch reveals the
    // pattern behind it, and a fully-transparent entry (grid row 3, ramp entry 0) shows it untouched. Two
    // grays drawn from the swatch atlas's index-0 / index-1 blocks (tiles 0 and 2). Built once — static.
    const std::array<Rgba8, 2> bgColors{{{40, 40, 48}, {88, 88, 98}}};
    const PaletteId bgPal = renderer.uploadPalette(std::span<const Rgba8>(bgColors));
    constexpr int kBgCols = 20, kBgRows = 18;  // 160x144 / 8
    std::vector<TileCell> bgCells(static_cast<std::size_t>(kBgCols) * kBgRows);
    for (int ty = 0; ty < kBgRows; ++ty) {
        for (int tx = 0; tx < kBgCols; ++tx) {
            bgCells[static_cast<std::size_t>(ty) * kBgCols + tx] =
                TileCell{.atlas = atlasNone, .tile = blockTile(((tx + ty) % 2) ? 1 : 0), .palette = bgPal};
        }
    }

    // Load the SAME grid once per read order (Embed default → baked into this binary). One literal path
    // in the source, so the build bakes palette_grid.png once; the loop slices the baked bytes 8 ways.
    std::array<PaletteId, kOrders.size()> gridPals{};
    PaletteId rampPal{};
    try {
        for (std::size_t i = 0; i < kOrders.size(); ++i) {
            gridPals[i] = renderer.loadPaletteImage(
                "examples/palette_image_demo/assets/palette_grid.png", kOrders[i].order);
        }
        // The ramp is LoadFromPath — copied beside the binary, read from disk at runtime. Proves the
        // on-disk policy alongside the embedded grid.
        rampPal = renderer.loadPaletteImage("examples/palette_image_demo/assets/palette_ramp.png",
                                            ReadOrder::LeftRightThenDown, 0, AssetPolicy::LoadFromPath);
    } catch (const std::exception& e) {
        std::printf("palette-image demo: could not load a palette: %s\n", e.what());
        return 1;
    }
    std::printf("palette-image demo — loaded palette_grid.png (Embed / baked into the binary) and "
                "palette_ramp.png (LoadFromPath / read from disk). Both produced palette entries.\n");

    int  orderIdx = 0;
    bool gbHole   = false;  // false = swatch sheet is None ({}); true = GameBoy ({0}) holes swatch 0
    auto announce = [&] {
        std::printf("[read order: %s | swatch 0: %s]  grid entries drawn in store order (entry 0 top-left)\n",
                    kOrders[static_cast<std::size_t>(orderIdx)].name,
                    gbHole ? "structural hole ({0})" : "drawn (palette colour + alpha)");
    };
    announce();

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::NextOrder))  { orderIdx = (orderIdx + 1) % 8; announce(); }
        if (in.justPressed(Action::PrevOrder))  { orderIdx = (orderIdx + 7) % 8; announce(); }
        if (in.justPressed(Action::ToggleHole)) { gbHole = !gbHole; announce(); }
    });

    // Persistent backing for the spans the draw state references during renderFrame().
    FrameDrawState      frame;
    std::vector<Sprite> gridSwatches;
    std::vector<Sprite> rampSwatches;

    loop.renderLoop([&]() {
        frame.layers.clear();

        // The swatch sheet the grid + ramp draw from: None ({}) or GameBoy ({0}), per the A toggle.
        const AtlasId swatchAtlas = gbHole ? atlasGB : atlasNone;

        // z=0 — the static checker background, so the swatch palettes' alpha reads as transparency.
        DrawLayer bgLayer{.key = "bg"};
        bgLayer.z       = 0;
        bgLayer.size    = PixelSize{160, 144};
        bgLayer.content = TileContent{.widthInTiles = kBgCols, .heightInTiles = kBgRows,
                                      .cells = std::span<const TileCell>(bgCells), .wrap = TileWrap::Blank};
        frame.layers.push_back(std::move(bgLayer));

        // The 4x4 grid palette in STORE order: entry k at on-screen (col k%4, row k/4). With
        // LeftRightThenDown the on-screen grid matches the source; another order permutes it.
        const PaletteId pal = gridPals[static_cast<std::size_t>(orderIdx)];
        gridSwatches.clear();
        constexpr int gridPitch = 20;                  // 16px block + 4px gap
        constexpr int gridStartX = (160 - (4 * gridPitch - 4)) / 2;  // centered 4-wide
        constexpr int gridStartY = 16;
        static const std::vector<std::string> gKeys =
            [] { std::vector<std::string> v; for (int k = 0; k < 64; ++k) v.push_back("g" + std::to_string(k)); return v; }();
        static const std::vector<std::string> rKeys =
            [] { std::vector<std::string> v; for (int k = 0; k < 16; ++k) v.push_back("r" + std::to_string(k)); return v; }();
        for (int k = 0; k < kBlocks; ++k) {
            gridSwatches.push_back(Sprite{.key = gKeys[static_cast<std::size_t>(k)],
                                          .x = gridStartX + (k % 4) * gridPitch,
                                          .y = gridStartY + (k / 4) * gridPitch,
                                          .size = AssetDimensions{kBlockPx, kBlockPx},
                                          .atlas = swatchAtlas, .tile = blockTile(k), .palette = pal});
        }
        DrawLayer gridLayer{.key = "grid"};
        gridLayer.z       = 10;
        gridLayer.size    = PixelSize{160, 144};
        gridLayer.content = SpriteContent{.sprites = std::span<const Sprite>(gridSwatches)};
        frame.layers.push_back(std::move(gridLayer));

        // The 8-step ramp (LoadFromPath) as a contiguous bar below the grid — proves the disk-loaded
        // palette renders. Fixed order; 8 abutting 16px swatches.
        rampSwatches.clear();
        constexpr int rampStartX = (160 - 8 * kBlockPx) / 2;
        constexpr int rampY = 112;
        for (int j = 0; j < 8; ++j) {
            rampSwatches.push_back(Sprite{.key = rKeys[static_cast<std::size_t>(j)],
                                          .x = rampStartX + j * kBlockPx, .y = rampY,
                                          .size = AssetDimensions{kBlockPx, kBlockPx},
                                          .atlas = swatchAtlas, .tile = blockTile(j), .palette = rampPal});
        }
        DrawLayer rampLayer{.key = "ramp"};
        rampLayer.z       = 20;
        rampLayer.size    = PixelSize{160, 144};
        rampLayer.content = SpriteContent{.sprites = std::span<const Sprite>(rampSwatches)};
        frame.layers.push_back(std::move(rampLayer));

        renderer.renderFrame(frame);
    });

    std::printf("  top: the 4x4 hue grid (Embed) in store order; bottom: the brightness ramp "
                "(LoadFromPath). Left/Right = read order (8). A = toggle swatch 0 between drawn ({}) and "
                "structural hole ({0}). Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
