// Atlas release — scenes that upload their sheets and release them when they end, and a store that holds
// its size while they do.
//
// Each of four scenes is four 64×64 sheets drawn in code, a pattern of its own per scene, colored through a
// palette of its own, and shown as a 2×2 of tile layers. Going to the next scene releases the last scene's
// sheets with renderer.releaseAtlas(...) and uploads its own: the new sheets take the room and the handles
// the released ones held, so the store's size, shown in the status line from renderStats(), stays where it
// was. Holding keeps the last scene's sheets instead; they stay in the store beside the new ones, and the
// store grows to hold them.
//
// Keys (the status line at the bottom shows each one and what it is set to):
//   SPACE    the next scene
//   H        hold: keep the last scene's sheets when the scene changes, or release them
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: walks the four scenes releasing each one's sheets
//               and requires the store's size after the first scene to hold through the rest; walks them
//               again holding and requires the store to grow; requires a released sheet to draw nothing
//               and a handle minted again to draw its new sheet texel for texel; uploads and releases one
//               16×16 sheet ten thousand times and requires the store's size to hold; and requires an
//               8×16400 sheet, taller than the store can be, to throw std::runtime_error. Exits nonzero on
//               any miss (CI runs this on every platform); on Windows on ARM, whose CI runner has no GPU
//               device, exits 77 — a counted skip.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

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
#include "retropp/viewport.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
inline constexpr bool kDeviceOptional = true;
#else
inline constexpr bool kDeviceOptional = false;
#endif

constexpr int kViewW  = 640;
constexpr int kViewH  = 360;
constexpr int kCell   = 8;    // the sheets' cell, and the tile layers'
constexpr int kSheet  = 64;   // each sheet is kSheet × kSheet
constexpr int kShown  = 128;  // each quadrant shows its sheet twice across and twice down
constexpr int kGap    = 16;
constexpr int kScenes = 4;
constexpr int kSheets = 4;    // per scene
constexpr int kColors = 4;    // per scene's palette

// Where quadrant `q` (0–3, left to right then down) sits on screen; cell-aligned, centered across.
constexpr int kQuadX = (kViewW - (2 * kShown + kGap)) / 2;
constexpr int kQuadY = 16;
[[nodiscard]] constexpr int quadX(int q) { return kQuadX + (q % 2) * (kShown + kGap); }
[[nodiscard]] constexpr int quadY(int q) { return kQuadY + (q / 2) * (kShown + kGap); }

// ── The sheets ────────────────────────────────────────────────────────────────────────────────────
// The palette index at (x, y) of sheet `sheet` in scene `scene`: checks, bands, rings and diamonds, each
// sheet of a scene at its own scale, so every sheet differs from every other.
[[nodiscard]] std::uint8_t sheetIndex(int scene, int sheet, int x, int y) {
    const int step = 4 + 2 * sheet;
    switch (scene) {
        case 0: return static_cast<std::uint8_t>((x / step + y / step) % kColors);
        case 1: return static_cast<std::uint8_t>(((y + x * (sheet + 1) / 4) / step) % kColors);
        case 2: {
            const double r = std::sqrt(static_cast<double>((x - 32) * (x - 32) + (y - 32) * (y - 32)));
            return static_cast<std::uint8_t>(static_cast<int>(r / step) % kColors);
        }
        default: return static_cast<std::uint8_t>(((std::abs(x - 32) + std::abs(y - 32)) / step) % kColors);
    }
}

[[nodiscard]] std::vector<std::uint8_t> sheetPixels(int scene, int sheet) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kSheet) * kSheet);
    for (int y = 0; y < kSheet; ++y)
        for (int x = 0; x < kSheet; ++x)
            px[static_cast<std::size_t>(y) * kSheet + static_cast<std::size_t>(x)] = sheetIndex(scene, sheet, x, y);
    return px;
}

// Each scene's four colors: a dark ground and three tones of the scene's own hue.
constexpr std::array<std::array<Rgba8, kColors>, kScenes> kScenePalettes{{
    {{Rgba8{.r = 20, .g = 24, .b = 40}, Rgba8{.r = 60, .g = 110, .b = 200}, Rgba8{.r = 120, .g = 170, .b = 240},
      Rgba8{.r = 220, .g = 235, .b = 255}}},
    {{Rgba8{.r = 30, .g = 20, .b = 20}, Rgba8{.r = 190, .g = 70, .b = 50}, Rgba8{.r = 240, .g = 140, .b = 70},
      Rgba8{.r = 255, .g = 220, .b = 140}}},
    {{Rgba8{.r = 16, .g = 32, .b = 20}, Rgba8{.r = 50, .g = 150, .b = 80}, Rgba8{.r = 130, .g = 210, .b = 110},
      Rgba8{.r = 230, .g = 250, .b = 190}}},
    {{Rgba8{.r = 30, .g = 18, .b = 36}, Rgba8{.r = 140, .g = 60, .b = 170}, Rgba8{.r = 210, .g = 120, .b = 220},
      Rgba8{.r = 250, .g = 210, .b = 250}}},
}};

// The scene on screen and its handles; `kept` are the sheets earlier scenes left in the store under hold.
struct Scenes {
    std::array<PaletteId, kScenes> palettes{};
    int                            scene = 0;
    std::array<AtlasId, kSheets>   shown{};
    std::vector<AtlasId>           kept;
    bool                           hold = false;
};

void uploadScene(Renderer& r, Scenes& s) {
    for (int k = 0; k < kSheets; ++k) {
        const std::vector<std::uint8_t> px = sheetPixels(s.scene, k);
        s.shown[static_cast<std::size_t>(k)] = r.uploadAtlas(px.data(), kSheet, kSheet).atlasId;
    }
}

// The next scene: the last one's sheets are released (or kept, under hold), then the new one's uploaded.
void nextScene(Renderer& r, Scenes& s) {
    for (const AtlasId id : s.shown) {
        if (s.hold)
            s.kept.push_back(id);
        else
            r.releaseAtlas(id);
    }
    s.scene = (s.scene + 1) % kScenes;
    uploadScene(r, s);
}

// Quadrant `q`'s layer: sheet `atlas` across a kShown × kShown map, placed by the layer's scroll.
[[nodiscard]] DrawLayer quadLayer(int q, AtlasId atlas, PaletteId palette, std::vector<TileCell>& cells) {
    constexpr int kMap = kShown / kCell;
    constexpr int kRow = kSheet / kCell;  // cells per sheet row
    cells.clear();
    for (int cy = 0; cy < kMap; ++cy)
        for (int cx = 0; cx < kMap; ++cx)
            cells.push_back(TileCell{.atlas   = atlas,
                                     .tile    = static_cast<std::uint16_t>((cy % kRow) * kRow + cx % kRow),
                                     .palette = palette});
    DrawLayer layer{.key = "quad" + std::to_string(q)};
    layer.z       = 10 + q;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.scroll  = LayerScroll{-quadX(q), -quadY(q)};
    layer.content = TileContent{.widthInTiles  = kMap,
                                .heightInTiles = kMap,
                                .cells         = std::span<const TileCell>(cells),
                                .wrap          = TileWrap::Blank};
    return layer;
}

// ── The backdrop ──────────────────────────────────────────────────────────────────────────────────
constexpr Rgba8 kBackdrop{.r = 18, .g = 20, .b = 28};

struct Backdrop {
    AtlasId               sheet{};
    PaletteId             palette{};
    std::vector<TileCell> cells;
};

[[nodiscard]] Backdrop uploadBackdrop(Renderer& r) {
    const std::array<std::uint8_t, kCell * kCell> blank{};
    const std::array<Rgba8, 1>                    color{{kBackdrop}};
    Backdrop                                      b;
    b.sheet   = r.uploadAtlas(blank.data(), kCell, kCell).atlasId;
    b.palette = r.uploadPalette(std::span<const Rgba8>(color));
    b.cells.assign(static_cast<std::size_t>(kViewW / kCell) * (kViewH / kCell),
                   TileCell{.atlas = b.sheet, .tile = 0, .palette = b.palette});
    return b;
}

[[nodiscard]] DrawLayer backdropLayer(const Backdrop& b) {
    DrawLayer layer{.key = "backdrop"};
    layer.z       = 0;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.content = TileContent{.widthInTiles  = kViewW / kCell,
                                .heightInTiles = kViewH / kCell,
                                .cells         = std::span<const TileCell>(b.cells)};
    return layer;
}

// ── Text ──────────────────────────────────────────────────────────────────────────────────────────
constexpr int kGlyphPx = 16;

struct Fonts {
    AtlasManifest font;
    PaletteId     text{};
    PaletteId     dim{};
};

[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    switch (ch) {  // punctuation cells in the authored font sheet
        case '.': return 38;
        case '-': return 39;
        default:  return 36;  // space
    }
}

// The status line: each key beside what it is set to, then the live sheets and the store's size.
[[nodiscard]] std::vector<Sprite> statusSprites(const Scenes& s, const Renderer::RenderStats& stats,
                                                const Fonts& fonts) {
    std::vector<Sprite> out;
    const auto write = [&](int x, int y, std::string_view text, PaletteId pal, std::string_view keyPrefix) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (glyphCell(text[i]) == 36) continue;
            out.push_back(Sprite{.key     = std::string(keyPrefix) + std::to_string(i),
                                 .x       = x + static_cast<int>(i) * kGlyphPx,
                                 .y       = y,
                                 .size    = AssetDimensions{.width = kGlyphPx, .height = kGlyphPx},
                                 .atlas   = fonts.font.atlasId,
                                 .tile    = static_cast<std::uint16_t>(fonts.font[glyphCell(text[i])].tile),
                                 .palette = pal});
        }
    };
    constexpr int kLine1 = kViewH - 2 * kGlyphPx - 12;
    constexpr int kLine2 = kViewH - kGlyphPx - 8;

    write(16, kLine1, "SPACE", fonts.text, "knext");
    write(16 + 6 * kGlyphPx, kLine1, "SCENE " + std::to_string(s.scene + 1), fonts.dim, "vnext");
    write(16 + 16 * kGlyphPx, kLine1, "H", fonts.text, "khold");
    write(16 + 18 * kGlyphPx, kLine1, s.hold ? "HOLD ON" : "HOLD OFF", fonts.dim, "vhold");

    const std::size_t live = s.shown.size() + s.kept.size();
    write(16, kLine2, "SHEETS " + std::to_string(live), fonts.dim, "vsheets");
    write(16 + 13 * kGlyphPx, kLine2,
          "STORE " + std::to_string(stats.atlasStoreWidth) + " X " + std::to_string(stats.atlasStoreHeight),
          fonts.dim, "vstore");
    return out;
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────
int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("atlas release verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("atlas release verify: no GPU device (%s)%s\n", SDL_GetError(),
                    kDeviceOptional ? " — skipped on this platform" : "");
        SDL_Quit();
        return kDeviceOptional ? 77 : 1;
    }

    int        failures = 0;
    const auto check    = [&](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    };
    const auto storeSize = [](const Renderer& r) {
        return std::to_string(r.renderStats().atlasStoreWidth) + "x" + std::to_string(r.renderStats().atlasStoreHeight);
    };
    std::string released, held;

    {
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);
        const Backdrop backdrop = uploadBackdrop(r);
        Scenes         s;
        for (int i = 0; i < kScenes; ++i)
            s.palettes[static_cast<std::size_t>(i)] =
                r.uploadPalette(std::span<const Rgba8>(kScenePalettes[static_cast<std::size_t>(i)]));

        std::vector<TileCell> cells;
        const auto            capture = [&](AtlasId atlas, PaletteId palette) {
            FrameDrawState frame;
            frame.layers = {backdropLayer(backdrop), quadLayer(0, atlas, palette, cells)};
            return r.captureViewport(frame);
        };
        const auto readAt = [](const std::vector<Rgba8>& px, int x, int y) {
            return px[static_cast<std::size_t>(y) * kViewW + static_cast<std::size_t>(x)];
        };
        // Every pixel of quadrant 0 is the backdrop: the layer drew nothing.
        const auto drewNothing = [&](const std::vector<Rgba8>& px) {
            int shown = 0;
            for (int y = 0; y < kShown; ++y)
                for (int x = 0; x < kShown; ++x) {
                    const Rgba8 c = readAt(px, quadX(0) + x, quadY(0) + y);
                    if (c.r != kBackdrop.r || c.g != kBackdrop.g || c.b != kBackdrop.b) ++shown;
                }
            return shown;
        };
        // Every pixel of quadrant 0 is the color sheet `sheet` of `scene` names there; the misses counted.
        const auto drewSheet = [&](const std::vector<Rgba8>& px, int scene, int sheet) {
            int misses = 0;
            for (int y = 0; y < kShown; ++y)
                for (int x = 0; x < kShown; ++x) {
                    const Rgba8 want = kScenePalettes[static_cast<std::size_t>(scene)]
                                                     [sheetIndex(scene, sheet, x % kSheet, y % kSheet)];
                    const Rgba8 got = readAt(px, quadX(0) + x, quadY(0) + y);
                    if (got.r != want.r || got.g != want.g || got.b != want.b) ++misses;
                }
            return misses;
        };

        // 1. Four scenes, each releasing the last one's sheets: the store holds the size the first left it.
        uploadScene(r, s);
        const int firstW = r.renderStats().atlasStoreWidth;
        const int firstH = r.renderStats().atlasStoreHeight;
        released         = storeSize(r);
        for (int i = 1; i < kScenes; ++i) {
            const AtlasId leaving = s.shown[0];
            // 2. A released sheet draws nothing: release the scene's sheets, then draw the first one's handle.
            for (const AtlasId id : s.shown) r.releaseAtlas(id);
            const int stray = drewNothing(capture(leaving, s.palettes[static_cast<std::size_t>(s.scene)]));
            check(stray == 0, "scene " + std::to_string(i) + ": a released sheet draws nothing (" +
                                  std::to_string(stray) + " pixels drawn)");

            s.scene = i;
            uploadScene(r, s);
            check(r.renderStats().atlasStoreWidth == firstW && r.renderStats().atlasStoreHeight == firstH,
                  "scene " + std::to_string(i + 1) + ": the store holds its size (" + released + ", now " +
                      storeSize(r) + ")");

            // 3. The handle the released sheet held is minted again, and draws its new sheet.
            for (int k = 0; k < kSheets; ++k) {
                if (s.shown[static_cast<std::size_t>(k)] != leaving) continue;
                const int misses = drewSheet(capture(leaving, s.palettes[static_cast<std::size_t>(i)]), i, k);
                check(misses == 0, "scene " + std::to_string(i + 1) + ": the handle minted again draws its new sheet (" +
                                       std::to_string(misses) + " pixels off)");
            }
            check(std::find(s.shown.begin(), s.shown.end(), leaving) != s.shown.end(),
                  "scene " + std::to_string(i + 1) + ": a released handle is minted again");
        }

        // 4. One 16×16 sheet uploaded and released ten thousand times: the store holds its size.
        const std::array<std::uint8_t, 16 * 16> small{};
        for (int i = 0; i < 10000; ++i) r.releaseAtlas(r.uploadAtlas(small.data(), 16, 16).atlasId);
        check(r.renderStats().atlasStoreWidth == firstW && r.renderStats().atlasStoreHeight == firstH,
              "ten thousand uploads and releases leave the store its size (" + released + ", now " + storeSize(r) +
                  ")");

        // 5. A sheet taller than the store can be is refused.
        const std::vector<std::uint8_t> tall(static_cast<std::size_t>(8) * 16400u);
        bool                            refused = false;
        try {
            (void)r.uploadAtlas(tall.data(), 8, 16400);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        check(refused, "an 8x16400 sheet throws std::runtime_error");
    }

    {
        // 6. The same four scenes, holding every one: the store grows to hold them.
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);
        (void)uploadBackdrop(r);
        Scenes s;
        s.hold = true;
        uploadScene(r, s);
        const int firstW = r.renderStats().atlasStoreWidth;
        const int firstH = r.renderStats().atlasStoreHeight;
        const std::string first = storeSize(r);
        for (int i = 1; i < kScenes; ++i) nextScene(r, s);
        held = storeSize(r);
        check(static_cast<long long>(r.renderStats().atlasStoreWidth) * r.renderStats().atlasStoreHeight >
                  static_cast<long long>(firstW) * firstH,
              "holding every scene grows the store (" + first + ", now " + held + ")");
    }

    std::printf("atlas release verify: the store at %s through four scenes released, %s with all four held; %s\n",
                released.c_str(), held.c_str(), failures == 0 ? "PASS" : "FAIL");
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

// ── The window ────────────────────────────────────────────────────────────────────────────────────
enum class Action : std::uint8_t { Next, Hold };

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "AtlasRelease"},
        .window   = {.title = "Polyrhythm — atlas release"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Next, {SDL_SCANCODE_SPACE}}, {Action::Hold, {SDL_SCANCODE_H}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font = renderer.loadAtlas("examples/atlas_release/assets/art/font.png",
                                    AssetDimensions{.width = 16, .height = 16}, ContentKind::Tileset,
                                    ReadOrder::LeftRightThenDown, 64, TransparentIndices::None, 0,
                                    AssetPolicy::Embed);
    fonts.text = renderer.loadPaletteImage("examples/atlas_release/assets/palettes/font.png",
                                           ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim  = renderer.loadPaletteImage("examples/atlas_release/assets/palettes/mono.png",
                                           ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    const Backdrop backdrop = uploadBackdrop(renderer);
    Scenes         scenes;
    for (int i = 0; i < kScenes; ++i)
        scenes.palettes[static_cast<std::size_t>(i)] =
            renderer.uploadPalette(std::span<const Rgba8>(kScenePalettes[static_cast<std::size_t>(i)]));
    uploadScene(renderer, scenes);

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Hold)) scenes.hold = !scenes.hold;
        if (in.justPressed(Action::Next)) nextScene(renderer, scenes);
    });

    std::array<std::vector<TileCell>, kSheets> cells;
    FrameDrawState                             frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        frame.layers.push_back(backdropLayer(backdrop));
        const PaletteId palette = scenes.palettes[static_cast<std::size_t>(scenes.scene)];
        for (int q = 0; q < kSheets; ++q)
            frame.layers.push_back(quadLayer(q, scenes.shown[static_cast<std::size_t>(q)], palette,
                                             cells[static_cast<std::size_t>(q)]));

        const std::vector<Sprite> sprites = statusSprites(scenes, renderer.renderStats(), fonts);
        DrawLayer                 text{.key = "text"};
        text.z       = 20;
        text.size    = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(text);

        renderer.renderFrame(frame);
    });

    std::printf(
        "Atlas release: four scenes of four sheets each. SPACE goes to the next scene, releasing the last\n"
        "scene's sheets and uploading its own, and the store holds its size; H holds, keeping the last scene's\n"
        "sheets instead, and the store grows. Close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
