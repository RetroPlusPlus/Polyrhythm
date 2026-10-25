// Arbitrary-palette demo — live-GPU proof that the color pipeline has NO 256-color
// palette cap and NO 8-bit-per-pixel atlas-index cap.
//
// Everything is built in code (no asset file):
//   • a 1024-color palette (a smooth HSV rainbow) via uploadPalette — far past the former 256 limit;
//   • an indexed atlas 1024 px wide × 8 px tall where pixel column x holds palette INDEX x (0..1023),
//     uploaded through the 16-bit uploadAtlas overload — indices past 255 only fit because the atlas
//     is now R32_UINT;
//   • a 128-tile-wide tilemap (map column c → atlas cell c), so each 8-px tile shows an 8-color band
//     and the full row sweeps all 1024 colors.
// The band drifts slowly sideways (a slow, same-direction scroll — no flashing). If the pipeline were
// still capped you would see banding or garbage past color 256; instead the whole smooth rainbow shows.
//
// The arbitrary-palette + flat-offset math is proven headlessly in tests (paletteStoreTexel /
// paletteSetOffsets); this is the on-a-real-device proof CI can't run (no display).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {
// Smooth HSV→RGB sweep at s = v = 1, hue in [0,1) — a clean rainbow with no hard edges.
Rgba8 hue(float h) {
    const float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    const float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    const float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    auto ch = [](float v) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
    };
    return Rgba8{ch(r), ch(g), ch(b), 255};
}
}  // namespace

int main() {

    constexpr int kColors = 1024;            // > 256 — the whole point
    constexpr int kAtlasW = kColors;         // one column per color
    constexpr int kAtlasH = 8;               // one tile tall
    constexpr int kTiles  = kAtlasW / 8;     // 128 tiles wide
    constexpr int kMapH   = 28;              // rows of the band (each row is the same sweep)
    constexpr ViewportResolution kView = ViewportResolution::Genesis;  // 320×224 — a wide canvas

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Arbitrary Palette Demo"},
        .window   = {.title = "Polyrhythm — arbitrary palette (1024 colours)"},
        .viewport = kView};
    EngineConfig::setActive(config);  // bare ctors below inherit it
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // 1024-color palette — a full HSV rainbow. uploadPalette has no cap now.
    std::vector<Rgba8> rainbow(static_cast<std::size_t>(kColors));
    for (int i = 0; i < kColors; ++i) {
        rainbow[static_cast<std::size_t>(i)] = hue(static_cast<float>(i) / static_cast<float>(kColors));
    }
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(rainbow));

    // Indexed atlas: column x → palette index x (0..1023). The 16-bit uploadAtlas overload; values
    // past 255 require the R32_UINT atlas.
    std::vector<std::uint16_t> atlasIdx(static_cast<std::size_t>(kAtlasW) * kAtlasH);
    for (int y = 0; y < kAtlasH; ++y) {
        for (int x = 0; x < kAtlasW; ++x) {
            atlasIdx[static_cast<std::size_t>(y) * kAtlasW + x] = static_cast<std::uint16_t>(x);
        }
    }
    const AtlasId atlas = renderer.uploadAtlas(atlasIdx.data(), kAtlasW, kAtlasH).atlasId;

    // Tilemap: map column c → atlas cell c, so column c draws the 8-color band [c*8 .. c*8+7].
    std::vector<TileCell> cells(static_cast<std::size_t>(kTiles) * kMapH);
    for (int row = 0; row < kMapH; ++row) {
        for (int col = 0; col < kTiles; ++col) {
            cells[static_cast<std::size_t>(row) * kTiles + col] =
                TileCell{.atlas = atlas, .tile = static_cast<std::uint16_t>(col), .palette = pal};
        }
    }

    std::printf("arbitrary palette: uploaded %d colours; atlas indices run 0..%d (needs R32). "
                "PaletteId(flat offset) = %u. The full rainbow should drift smoothly, no banding.\n",
                kColors, kColors - 1, static_cast<std::uint32_t>(pal));

    int tick = 0;
    loop.simTick([&](const InputState&) { ++tick; });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer band{.key = "rainbow"};
        band.z       = 10;
        band.size    = PixelSize{kView.width, kView.height};
        band.scroll  = LayerScroll{tick / 4, 0};  // slow, same-direction drift (no flashing)
        band.content = TileContent{.widthInTiles  = kTiles,
                                   .heightInTiles = kMapH,
                                   .cells         = std::span<const TileCell>(cells),
                                   .wrap          = TileWrap::Repeat};
        frame.layers.push_back(std::move(band));
        renderer.renderFrame(frame);
    });

    std::printf("A 1024-colour rainbow built from ONE palette, drifting slowly. Close to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
