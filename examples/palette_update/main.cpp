// Palette update — one palette's colors changed in place while the picture drawn through it stays put.
//
// A sunset is drawn from an indexed sheet through ONE four-color palette: sky, hills, ground, sun. Beside
// it, the same four colors as swatches, drawn through the same palette. Pick a color and step its red,
// green or blue: each step is renderer.updatePalette(handle, ramp) — the palette keeps its PaletteId, no
// cell or sprite is touched, and the picture and its swatch take the new color on the next frame. The
// handle shown under the picture is the one every cell names, and it never changes.
//
// Keys:
//   UP / DOWN   pick a color
//   Q / A       its red up / down
//   W / S       its green up / down
//   E / D       its blue up / down
// A held key steps once, then repeats, the way a settings screen's auto-repeat does.
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: draws the still picture until its compose is
//               skipped, then steps colors with the same function the keys call and requires, after each
//               step, that the next frame composes and that every color reads back at a point of the
//               picture and at its swatch exactly as stepped; then requires that the next palette uploaded
//               lands where it would have with no steps at all. Exits nonzero on any miss (CI runs this
//               on every platform); on Windows on ARM, whose CI runner has no GPU device, exits 77 — a
//               counted skip.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/asset_policy.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
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

constexpr int kViewW = 640;
constexpr int kViewH = 360;
constexpr int kCell  = 8;  // the sheet's cell, and the tile layer's
constexpr int kMapW  = kViewW / kCell;
constexpr int kMapH  = kViewH / kCell;

// ── The picture ───────────────────────────────────────────────────────────────────────────────────
// A 192×192 sunset in four indices, stacked above one row of four solid tiles — one per index — that the
// swatches and the backdrop draw.
constexpr int kArtW       = 192;
constexpr int kArtH       = 192;
constexpr int kSheetW     = kArtW;
constexpr int kSheetH     = kArtH + kCell;
constexpr int kSheetCells = kSheetW / kCell;  // cells per sheet row
constexpr int kColors     = 4;

enum Index : std::uint8_t { Sky = 0, Hills = 1, Ground = 2, Sun = 3 };

[[nodiscard]] std::uint8_t artIndex(int x, int y) {
    if (y >= 160) return Ground;
    const double ridge = 128.0 + 14.0 * std::sin(static_cast<double>(x) / 22.0);
    if (y >= ridge) return Hills;
    const int dx = x - 132, dy = y - 60;
    if (dx * dx + dy * dy <= 30 * 30) return Sun;
    return Sky;
}

[[nodiscard]] std::uint16_t solidTile(int index) {
    return static_cast<std::uint16_t>((kArtH / kCell) * kSheetCells + index);
}

// Where things sit on screen, in pixels; each is cell-aligned so the tile layer places it exactly.
constexpr int kPicX     = 32;
constexpr int kPicY     = 64;
constexpr int kSwatchX  = 272;
constexpr int kSwatchW  = 48;
constexpr int kSwatchH  = 32;
constexpr int kLabelX   = 336;
constexpr int kChannelX = 272;
constexpr int kBarX     = 508;
constexpr int kBarW     = 116;
[[nodiscard]] constexpr int swatchY(int color) { return 88 + color * 40; }
[[nodiscard]] constexpr int channelY(int channel) { return 256 + channel * 24; }

// The palette the picture opens with; every channel is a multiple of the step, so each is reachable.
constexpr std::array<Rgba8, kColors> kStartRamp{{
    Rgba8{.r = 40, .g = 60, .b = 120},   // sky
    Rgba8{.r = 40, .g = 110, .b = 60},   // hills
    Rgba8{.r = 120, .g = 80, .b = 40},   // ground
    Rgba8{.r = 240, .g = 200, .b = 80},  // sun
}};
constexpr std::array<std::string_view, kColors> kColorNames{"SKY", "HILLS", "GROUND", "SUN"};

// The sheet, the backdrop's palette and the picture's palette. The picture's palette is uploaded last, so
// the next palette uploaded begins where its four entries end.
struct Picture {
    AtlasId               sheet{};
    PaletteId             backdrop{};
    PaletteId             ramp{};
    std::vector<TileCell> cells;
};

[[nodiscard]] Picture uploadPicture(Renderer& r, const std::array<Rgba8, kColors>& ramp) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(kSheetW) * static_cast<std::size_t>(kSheetH));
    for (int y = 0; y < kSheetH; ++y)
        for (int x = 0; x < kSheetW; ++x)
            px[static_cast<std::size_t>(y) * kSheetW + static_cast<std::size_t>(x)] =
                y < kArtH ? artIndex(x, y)
                          : static_cast<std::uint8_t>(x / kCell < kColors ? x / kCell : 0);

    Picture pic;
    pic.sheet                             = r.uploadAtlas(px.data(), kSheetW, kSheetH).atlasId;
    const std::array<Rgba8, 1> backdrop{{Rgba8{.r = 18, .g = 20, .b = 28}}};
    pic.backdrop                          = r.uploadPalette(std::span<const Rgba8>(backdrop));
    pic.ramp                              = r.uploadPalette(std::span<const Rgba8>(ramp));

    // Every cell is the backdrop's solid tile until the picture and the swatches claim it; those name the
    // picture's palette.
    pic.cells.assign(static_cast<std::size_t>(kMapW) * kMapH,
                     TileCell{.atlas = pic.sheet, .tile = solidTile(0), .palette = pic.backdrop});
    const auto cell = [&](int tx, int ty) -> TileCell& {
        return pic.cells[static_cast<std::size_t>(ty) * kMapW + static_cast<std::size_t>(tx)];
    };
    for (int cy = 0; cy < kArtH / kCell; ++cy)
        for (int cx = 0; cx < kArtW / kCell; ++cx)
            cell(kPicX / kCell + cx, kPicY / kCell + cy) =
                TileCell{.atlas   = pic.sheet,
                         .tile    = static_cast<std::uint16_t>(cy * kSheetCells + cx),
                         .palette = pic.ramp};
    for (int k = 0; k < kColors; ++k)
        for (int cy = 0; cy < kSwatchH / kCell; ++cy)
            for (int cx = 0; cx < kSwatchW / kCell; ++cx)
                cell(kSwatchX / kCell + cx, swatchY(k) / kCell + cy) =
                    TileCell{.atlas = pic.sheet, .tile = solidTile(k), .palette = pic.ramp};
    return pic;
}

[[nodiscard]] DrawLayer pictureLayer(const Picture& pic) {
    DrawLayer layer{.key = "picture"};
    layer.z       = 0;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.content = TileContent{.widthInTiles  = kMapW,
                                .heightInTiles = kMapH,
                                .cells         = std::span<const TileCell>(pic.cells),
                                .wrap          = TileWrap::Blank};
    return layer;
}

// ── A step ────────────────────────────────────────────────────────────────────────────────────────
enum class Channel : std::uint8_t { Red, Green, Blue };
constexpr std::array<std::string_view, 3> kChannelNames{"RED", "GREEN", "BLUE"};
constexpr int                             kStep = 5;

[[nodiscard]] std::uint8_t& channelOf(Rgba8& c, Channel ch) {
    return ch == Channel::Red ? c.r : ch == Channel::Green ? c.g : c.b;
}

// One color's channel moves by `delta`, held to 0–255, and the whole ramp goes to the renderer under the
// palette's own handle.
void step(Renderer& r, const Picture& pic, std::array<Rgba8, kColors>& ramp, int color, Channel ch,
          int delta) {
    std::uint8_t& v = channelOf(ramp[static_cast<std::size_t>(color)], ch);
    v               = static_cast<std::uint8_t>(std::clamp(static_cast<int>(v) + delta, 0, 255));
    r.updatePalette(pic.ramp, std::span<const Rgba8>(ramp));
}

// ── Text ──────────────────────────────────────────────────────────────────────────────────────────
constexpr int kGlyphPx = 16;

struct Fonts {
    AtlasManifest font;
    PaletteId     text;
    PaletteId     active;
    PaletteId     dim;
};

[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    switch (ch) {  // punctuation cells in the authored font sheet
        case '(': return 44;
        case ')': return 45;
        case '.': return 38;
        case '-': return 39;
        case '_': return 37;
        default:  return 36;  // space + punctuation the font has no glyph for
    }
}

struct Panel {
    int         selected = 0;
    int         edits    = 0;
    std::string status   = "PICK A COLOR AND STEP IT";
};

[[nodiscard]] std::vector<Sprite> textSprites(const Panel& p, const std::array<Rgba8, kColors>& ramp,
                                              PaletteId handle, const Fonts& fonts) {
    std::vector<Sprite> out;
    // `keyPrefix` gives each glyph slot a stable key across frames.
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
    const auto center = [&](int y, std::string_view text, PaletteId pal, std::string_view keyPrefix) {
        write(kViewW / 2 - static_cast<int>(text.size()) * kGlyphPx / 2, y, text, pal, keyPrefix);
    };

    center(8, "PALETTE UPDATE", fonts.active, "title");
    center(28, "ITS COLORS CHANGE IN PLACE", fonts.dim, "sub");

    write(kPicX, 264, "HANDLE " + std::to_string(static_cast<std::size_t>(handle)), fonts.text, "handle");
    write(kPicX, 284, "EDITS " + std::to_string(p.edits), fonts.dim, "edits");

    write(kSwatchX, 64, "UP DOWN PICK", fonts.text, "pick");
    for (int k = 0; k < kColors; ++k) {
        const PaletteId pal = k == p.selected ? fonts.active : fonts.text;
        write(kLabelX, swatchY(k) + (kSwatchH - kGlyphPx) / 2, kColorNames[static_cast<std::size_t>(k)], pal,
              "name" + std::to_string(k) + "_");
    }

    // The picked color's three channels, each beside the two keys that step it.
    constexpr std::array<std::string_view, 3> keys{"Q A", "W S", "E D"};
    Rgba8 picked = ramp[static_cast<std::size_t>(p.selected)];
    for (int c = 0; c < 3; ++c) {
        const auto  ch    = static_cast<Channel>(c);
        std::string value = std::to_string(channelOf(picked, ch));
        value.insert(0, 3 - value.size(), '0');
        std::string label{kChannelNames[static_cast<std::size_t>(c)]};
        label.resize(6, ' ');
        write(kChannelX, channelY(c), std::string(keys[static_cast<std::size_t>(c)]) + "  " + label + value,
              fonts.text, "ch" + std::to_string(c) + "_");
    }

    center(336, p.status, fonts.active, "status");
    return out;
}

// The marker beside the picked swatch, and a bar per channel of the picked color.
[[nodiscard]] std::vector<Region> panelRegions(const Panel& p, const std::array<Rgba8, kColors>& ramp) {
    const auto rect = [](std::string key, float x, float y, float w, float h, Rgba8 color) {
        return Region{.key     = std::move(key),
                      .shape   = ShapePoints::rectangle(Point{.x = x, .y = y}, w, h),
                      .effects = {ScreenSpaceEffect{.kind = ScreenSpaceEffectKind::ColorFill, .fill = color}}};
    };
    std::vector<Region> out;
    out.push_back(rect("marker", kSwatchX - 14, static_cast<float>(swatchY(p.selected) + 8), 8, 16,
                       Rgba8{.r = 250, .g = 200, .b = 80}));
    Rgba8 picked = ramp[static_cast<std::size_t>(p.selected)];
    constexpr std::array<Rgba8, 3> fills{{Rgba8{.r = 230, .g = 70, .b = 70}, Rgba8{.r = 70, .g = 200, .b = 90},
                                          Rgba8{.r = 80, .g = 130, .b = 240}}};
    for (int c = 0; c < 3; ++c) {
        const auto  y    = static_cast<float>(channelY(c) + 4);
        const float fill = static_cast<float>(channelOf(picked, static_cast<Channel>(c))) / 255.0f * kBarW;
        out.push_back(rect("track" + std::to_string(c), kBarX, y, kBarW, 8, Rgba8{.r = 40, .g = 46, .b = 64}));
        out.push_back(rect("bar" + std::to_string(c), kBarX, y, fill, 8, fills[static_cast<std::size_t>(c)]));
    }
    return out;
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────
struct Edit {
    int     color = 0;
    Channel channel{};
    int     delta = 0;
};

int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("palette update verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("palette update verify: no GPU device (%s)%s\n", SDL_GetError(),
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

    {
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);
        std::array<Rgba8, kColors> ramp = kStartRamp;
        const Picture              pic  = uploadPicture(r, ramp);
        FrameDrawState             frame;
        frame.layers = {pictureLayer(pic)};

        // A point of the picture inside a patch of each color, so the pixel read there is that color's alone.
        const auto solidAround = [](int x, int y) {
            const std::uint8_t k = artIndex(x, y);
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (artIndex(x + dx, y + dy) != k) return false;
            return true;
        };
        std::array<Vec2i, kColors> inPicture{};
        std::array<bool, kColors>  found{};
        for (int y = 2; y < kArtH - 2; ++y)
            for (int x = 2; x < kArtW - 2; ++x) {
                const std::size_t k = artIndex(x, y);
                if (!found[k] && solidAround(x, y)) {
                    inPicture[k] = Vec2i{.x = kPicX + x, .y = kPicY + y};
                    found[k]     = true;
                }
            }
        for (int k = 0; k < kColors; ++k) check(found[static_cast<std::size_t>(k)], "the picture draws color " + std::to_string(k));

        // Every color and channel, both directions, and a step held at each end of the range.
        constexpr std::array<Edit, 8> script{{
            Edit{.color = Sun, .channel = Channel::Red, .delta = kStep},
            Edit{.color = Sky, .channel = Channel::Blue, .delta = -8 * kStep},
            Edit{.color = Hills, .channel = Channel::Green, .delta = 12 * kStep},
            Edit{.color = Ground, .channel = Channel::Red, .delta = -24 * kStep},
            Edit{.color = Ground, .channel = Channel::Red, .delta = -kStep},  // already 0: stays 0
            Edit{.color = Sun, .channel = Channel::Blue, .delta = 40 * kStep},  // clamps at 255
            Edit{.color = Sky, .channel = Channel::Green, .delta = kStep},
            Edit{.color = Hills, .channel = Channel::Red, .delta = 3 * kStep},
        }};

        for (std::size_t e = 0; e < script.size(); ++e) {
            const Edit&       edit = script[e];
            const std::string at   = "step " + std::to_string(e + 1) + ": ";

            const Renderer::RenderStats s0 = r.renderStats();
            r.renderFrame(frame);
            r.renderFrame(frame);
            const Renderer::RenderStats s1 = r.renderStats();
            check(s1.composeSkips == s0.composeSkips + 1, at + "the still picture's second frame skips its compose");

            step(r, pic, ramp, edit.color, edit.channel, edit.delta);
            r.renderFrame(frame);
            const Renderer::RenderStats s2 = r.renderStats();
            check(s2.composePasses == s1.composePasses + 1 && s2.composeSkips == s1.composeSkips,
                  at + "the frame after the step composes, its structure unchanged");

            const std::vector<Rgba8> px = r.captureViewport(frame);
            for (int k = 0; k < kColors; ++k) {
                const Rgba8 want    = ramp[static_cast<std::size_t>(k)];
                const auto  readAt  = [&](Vec2i p) { return px[static_cast<std::size_t>(p.y) * kViewW + static_cast<std::size_t>(p.x)]; };
                const Rgba8 picture = readAt(inPicture[static_cast<std::size_t>(k)]);
                const Rgba8 swatch  = readAt(Vec2i{.x = kSwatchX + kSwatchW / 2, .y = swatchY(k) + kSwatchH / 2});
                const auto  same    = [&](Rgba8 got) { return got.r == want.r && got.g == want.g && got.b == want.b; };
                const auto  said    = [&](Rgba8 got) {
                    return std::to_string(got.r) + "," + std::to_string(got.g) + "," + std::to_string(got.b) +
                           " (want " + std::to_string(want.r) + "," + std::to_string(want.g) + "," +
                           std::to_string(want.b) + ")";
                };
                check(same(picture), at + std::string(kColorNames[static_cast<std::size_t>(k)]) + " in the picture reads " + said(picture));
                check(same(swatch), at + std::string(kColorNames[static_cast<std::size_t>(k)]) + " swatch reads " + said(swatch));
            }
        }

        // The ramp was the last palette uploaded, so the next one begins where the ramp's four entries end —
        // however many steps came between.
        const std::array<Rgba8, 1> probe{{Rgba8{}}};
        const PaletteId            next = r.uploadPalette(std::span<const Rgba8>(probe));
        check(static_cast<std::size_t>(next) == static_cast<std::size_t>(pic.ramp) + kColors,
              "the store holds the entries it held before the steps (the next palette lands at " +
                  std::to_string(static_cast<std::size_t>(pic.ramp) + kColors) + ", it landed at " +
                  std::to_string(static_cast<std::size_t>(next)) + ")");

        std::printf("palette update verify: %zu steps, each drawn the next frame under handle %zu; %s\n",
                    script.size(), static_cast<std::size_t>(pic.ramp), failures == 0 ? "PASS" : "FAIL");
    }

    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

// ── The window ────────────────────────────────────────────────────────────────────────────────────
enum class Action : std::uint8_t { PickUp, PickDown, RedUp, RedDown, GreenUp, GreenDown, BlueUp, BlueDown };

constexpr int kRepeatDelay = 18;  // ticks a key is held before it repeats
constexpr int kRepeatEvery = 3;   // ticks between repeats

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "PaletteUpdate"},
        .window   = {.title = "Polyrhythm — palette update"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::PickUp, {SDL_SCANCODE_UP}},   {Action::PickDown, {SDL_SCANCODE_DOWN}},
                  {Action::RedUp, {SDL_SCANCODE_Q}},     {Action::RedDown, {SDL_SCANCODE_A}},
                  {Action::GreenUp, {SDL_SCANCODE_W}},   {Action::GreenDown, {SDL_SCANCODE_S}},
                  {Action::BlueUp, {SDL_SCANCODE_E}},    {Action::BlueDown, {SDL_SCANCODE_D}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font   = renderer.loadAtlas("examples/palette_update/assets/art/font.png",
                                      AssetDimensions{.width = 16, .height = 16},
                                      ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                                      TransparentIndices::None, 0, AssetPolicy::Embed);
    fonts.text   = renderer.loadPaletteImage("examples/palette_update/assets/palettes/font.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.active = renderer.loadPaletteImage("examples/palette_update/assets/palettes/font_pick.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim    = renderer.loadPaletteImage("examples/palette_update/assets/palettes/mono.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    std::array<Rgba8, kColors> ramp = kStartRamp;
    const Picture              pic  = uploadPicture(renderer, ramp);
    Panel                      panel;

    // A channel key steps on its press, then every few ticks while it stays held.
    std::array<int, 6> heldFor{};
    const auto         stepping = [&](const InputState& in, Action a, std::size_t slot) {
        if (in.justPressed(a)) {
            heldFor[slot] = 1;
            return true;
        }
        if (!in.isHeld(a)) {
            heldFor[slot] = 0;
            return false;
        }
        const int t = heldFor[slot]++;
        return t >= kRepeatDelay && (t - kRepeatDelay) % kRepeatEvery == 0;
    };

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::PickUp)) panel.selected = (panel.selected + kColors - 1) % kColors;
        if (in.justPressed(Action::PickDown)) panel.selected = (panel.selected + 1) % kColors;

        constexpr std::array<Action, 6> keys{Action::RedUp,   Action::RedDown, Action::GreenUp,
                                             Action::GreenDown, Action::BlueUp, Action::BlueDown};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (!stepping(in, keys[i], i)) continue;
            const auto ch = static_cast<Channel>(i / 2);
            step(renderer, pic, ramp, panel.selected, ch, i % 2 == 0 ? kStep : -kStep);
            ++panel.edits;
            Rgba8 picked = ramp[static_cast<std::size_t>(panel.selected)];
            panel.status = std::string(kColorNames[static_cast<std::size_t>(panel.selected)]) + " " +
                           std::string(kChannelNames[i / 2]) + " NOW " + std::to_string(channelOf(picked, ch)) +
                           " - SAME HANDLE";
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        frame.layers.push_back(pictureLayer(pic));

        const std::vector<Sprite> sprites = textSprites(panel, ramp, pic.ramp, fonts);
        DrawLayer                 text{.key = "text"};
        text.z       = 10;
        text.size    = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        text.regions = panelRegions(panel, ramp);
        frame.layers.push_back(text);

        renderer.renderFrame(frame);
    });

    std::printf(
        "Palette update: a sunset drawn through one four-color palette, and the same four colors as swatches.\n"
        "UP/DOWN pick a color; Q/A, W/S, E/D step its red, green and blue (hold to repeat). Each step is\n"
        "updatePalette(handle, ramp): the picture takes the new color on the next frame and the handle under it\n"
        "never changes. Close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
