// SNES co-execution demo — a cartridge running its own world, and this program reading and writing the
// places inside it while it runs.
//
// The picture on the left is the demo cartridge (examples/snes/cartridge) on its own thread, at the
// console's own speed. The arrows drive its sprite. The panel on the right is not the cartridge's picture:
// it is the machine's own memory, read every tick through places this program declares —
//
//   the 256 palette words      snes::Palette, the whole of CGRAM, drawn as a 16x16 grid of swatches
//   tiles 0 to 15 of video RAM  sixteen 32-byte entries at snes::videoRam(0), decoded from 4 bpp
//   the sprite's X and Y       two bytes of work RAM at $7E:0010, where the cartridge keeps them
//   the step                   one byte of the cartridge IMAGE at $00:8300, which its frame handler
//                              reads to decide how far a held direction moves the sprite
//
// Every key writes one of those places, and the cartridge carries on with what it finds:
//
//   1 2    write the sprite's color, palette word 129, to blue or yellow
//   3 4    move the sprite 32 pixels left or right, by writing its X in work RAM
//   5 6    patch the step byte in the image, 1 to 8, while it runs; hold an arrow to see it
//   SPACE  park the machine where it stands, and resume from there
//
// Modes:
//   (no args)   the window
//   --verify    headless: declares the four places, round-trips a palette word and the sprite's X, and
//               checks a patched step moves the sprite that far a frame; exits nonzero on any miss
//               (CI runs this on every platform)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/palette.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/snes.h"           // snes::Palette, snes::videoRam — the machine's memories by name
#include "retropp/vm.h"             // Vm — hosting, running, reading and writing
#include "retropp/windowed_host.h"

#include "examples/snes/cartridge/cartridge.h"

using namespace retropp;

namespace {

// ── The window ──────────────────────────────────────────────────────────────────────────────────

constexpr int kGlyphPx = 16, kGlyphStride = 128 / 8;  // font.png is 128 wide, in 8px tiles
constexpr int kCols = 64, kRows = 32;
constexpr int kViewW = kCols * kGlyphPx, kViewH = kRows * kGlyphPx;  // 1024 x 512

constexpr int kSlotW = 512, kSlotH = 448;           // the console's largest picture, and the slot it lands in
constexpr int kSlotTop = (kViewH - kSlotH) / 2;     // centered down the left half
constexpr int kPanelX = kViewW - kSlotW;            // the right half, where the panel is
constexpr int kPanelW = kViewW - kPanelX;

constexpr int kHeadCol  = 33;  // a heading
constexpr int kNameCol  = 34;  // a row's label, indented under its heading
constexpr int kValueCol = 42;  // a row's value
constexpr int kKeyCol   = 52;  // the keys that change a heading's rows

constexpr int kSwatchPx = 8;                        // one palette word
constexpr int kSwatchX = 32, kSwatchY = 5 * kGlyphPx;
constexpr int kTileScale = 2;                       // one tile pixel, drawn 2x2
constexpr int kStripX = 32, kStripY = 15 * kGlyphPx;

// ── The cartridge's places ──────────────────────────────────────────────────────────────────────

constexpr std::uint32_t kSpriteXY  = 0x7E0010;  // sprite 0's X, then its Y, in work RAM
constexpr std::uint32_t kStepByte  = 0x008300;  // the step, in the cartridge image
constexpr std::size_t   kColorWord = 129;       // object palette 0, color 1: the sprite's color
constexpr std::uint32_t kTileCount = 16;

// The places this program names in the machine. Never instantiated — the struct exists so the places
// have names, and naming one wrong is a compile error rather than a bad address.
struct Places {
    MemoryRegion palette;  // CGRAM, whole, by the console's own constant
    MemoryRegion tiles;    // the first sixteen 4 bpp tiles of video RAM, one entry each
    MemoryRegion sprite;   // the sprite's X and Y
    MemoryRegion step;     // one byte inside the cartridge image
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(regions(
        region(&Places::palette, snes::Palette, "palette"),
        region(&Places::tiles, MemoryRegion{.at = snes::videoRam(0), .size = 32, .count = kTileCount},
               "tiles"),
        region(&Places::sprite, MemoryRegion{.at = kSpriteXY, .size = 2}, "sprite"),
        region(&Places::step, MemoryRegion{.at = kStepByte, .size = 1}, "step")));
}

// A palette word as the console stores it — five bits each of red, green and blue, red lowest.
[[nodiscard]] std::uint16_t wordAt(std::span<const std::uint8_t> palette, std::size_t word) {
    return static_cast<std::uint16_t>(palette[word * 2] | (palette[word * 2 + 1] << 8));
}

void putWord(std::vector<std::uint8_t>& palette, std::size_t word, std::uint16_t value) {
    palette[word * 2]     = static_cast<std::uint8_t>(value & 0xFF);
    palette[word * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────
// Headless, one tick at a time, so every read answers a known step.

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_coexecution --verify: the places inside a running SNES cartridge read and write\n\n");

    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge());
    const auto places = declarePlaces(machine);
    machine.run(Vm::Advance::OnTick);
    const auto ticks = [&machine](int n) {
        for (int i = 0; i < n; ++i) machine.advanceTick();
    };
    ticks(3);  // the cartridge's reset code has filled video memory and set the sprite's color

    std::vector<std::uint8_t> palette = machine.read(places, &Places::palette);
    check(wordAt(palette, kColorWord) == 0x7FFF, "palette word 129 reads the white the cartridge set");

    putWord(palette, kColorWord, 0x001F);
    machine.write(places, &Places::palette, palette);
    ticks(1);
    check(wordAt(machine.read(places, &Places::palette), kColorWord) == 0x001F,
          "palette word 129 reads back the red this program wrote");

    const std::vector<std::uint8_t> tile = machine.read(places, &Places::tiles, 3);
    check(tile[0] == 0xFF && tile[2] == 0x00, "tile 3 reads the cartridge's stripes");

    std::vector<std::uint8_t> sprite = machine.read(places, &Places::sprite);
    sprite[0]                        = 200;
    machine.write(places, &Places::sprite, sprite);
    ticks(1);
    check(machine.read(places, &Places::sprite).at(0) == 200, "the sprite's X reads back 200");

    machine.write(places, &Places::step, std::vector<std::uint8_t>{4});
    machine.buttons(snes::Ports{.one = snes::Buttons{.right = true}, .two = std::nullopt});
    ticks(2);  // the pad is read at the frame's vertical blank
    const std::uint8_t before = machine.read(places, &Places::sprite).at(0);
    ticks(1);
    const std::uint8_t after = machine.read(places, &Places::sprite).at(0);
    std::printf("  the step patched to 4: X went %u -> %u in one tick with Right held\n",
                static_cast<unsigned>(before), static_cast<unsigned>(after));
    check(static_cast<std::uint8_t>(after - before) == 4, "the sprite moves by the patched step");
    machine.stop();

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

// ── The panel ───────────────────────────────────────────────────────────────────────────────────

enum class Action : ActionId { ColorOne = 20, ColorTwo, Left32, Right32, StepDown, StepUp, Park };

// The font sheet carries digits, then letters, then a blank. Anything else lands on the blank.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    return 36;
}

// A palette word as 8-bit red, green and blue: each five-bit channel widened by repeating its top bits.
void paintWord(std::uint8_t* px, std::uint16_t word) {
    const auto widen = [](unsigned c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
    px[0] = widen(word & 0x1F);
    px[1] = widen((word >> 5) & 0x1F);
    px[2] = widen((word >> 10) & 0x1F);
    px[3] = 255;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesCoexecution"},
        .window   = {.title = "Polyrhythm — SNES co-execution"},
        .viewport = ViewportResolution{kViewW, kViewH},
        .timing   = TimingProfile::Snes,
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ColorOne, {SDL_SCANCODE_1}},
        {Action::ColorTwo, {SDL_SCANCODE_2}},
        {Action::Left32, {SDL_SCANCODE_3}},
        {Action::Right32, {SDL_SCANCODE_4}},
        {Action::StepDown, {SDL_SCANCODE_5}},
        {Action::StepUp, {SDL_SCANCODE_6}},
        {Action::Park, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
    };
    map.add(presets::directional(snes::Button::Up, snes::Button::Down, snes::Button::Left,
                                 snes::Button::Right));
    platform.actions(map);

    // ── The panel's font and its three palettes ──────────────────────────────────────────────────
    const AtlasManifest font = renderer.loadAtlas(
        "examples/snes/coexecution/assets/art/font.png", AssetDimensions{kGlyphPx, kGlyphPx},
        ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64, TransparentIndices::of({0}), 0,
        AssetPolicy::Embed);
    const PaletteId palText = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/font.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palLive = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/font_pick.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palDim = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/mono.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);

    // A glyph is 16px and the grid is 8px tiles, so each character stamps a 2x2 block.
    constexpr int         kMonW = kCols * 2, kMonH = kRows * 2;
    std::vector<TileCell> text(static_cast<std::size_t>(kMonW) * kMonH,
                               TileCell{.atlas = font.atlasId, .tile = 0, .palette = palDim});
    const auto clearText = [&] {
        for (TileCell& c : text) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palDim;
        }
    };
    const auto put = [&](int col, int row, std::string_view s, PaletteId pal) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kCols || row < 0 || row >= kRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(s[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = text[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile      = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette   = pal;
                }
        }
    };

    // ── The machine ──────────────────────────────────────────────────────────────────────────────
    // Host the image and name the places before it runs; from run() on, the declared places are what
    // this program reads and writes, each read answered by the machine's latest completed step.
    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge());
    machine.video(true);
    const auto places = declarePlaces(machine);
    machine.run(Vm::Advance::Continuously);
    bool running = true;

    // What the panel shows, sampled once a tick.
    std::vector<std::uint8_t>              palette(snes::Palette.size, 0);
    std::array<std::vector<std::uint8_t>, kTileCount> tiles;
    std::uint8_t spriteX = 0, spriteY = 0, step = 1;
    std::string  status = "THE PANEL IS ITS OWN MEMORY";

    loop.simTick([&](const InputState& in) {
        machine.buttons(snes::Ports{.one = snes::held(in), .two = std::nullopt});

        // A write to palette word 129, through the whole palette the machine last published.
        const auto writeColor = [&](std::uint16_t color, const char* said) {
            std::vector<std::uint8_t> whole = machine.read(places, &Places::palette);
            putWord(whole, kColorWord, color);
            machine.write(places, &Places::palette, whole);
            status = said;
        };
        if (in.justPressed(Action::ColorOne)) writeColor(0x7C00, "PALETTE WORD 129 WRITTEN BLUE");
        if (in.justPressed(Action::ColorTwo)) writeColor(0x03FF, "PALETTE WORD 129 WRITTEN YELLOW");

        // The sprite's X is a byte of work RAM the cartridge moves every frame; this program moves it too.
        const int nudge = (in.justPressed(Action::Right32) ? 32 : 0) - (in.justPressed(Action::Left32) ? 32 : 0);
        if (nudge != 0) {
            std::vector<std::uint8_t> xy = machine.read(places, &Places::sprite);
            xy[0] = static_cast<std::uint8_t>(xy[0] + nudge);
            machine.write(places, &Places::sprite, xy);
            status = "ITS X WRITTEN IN WORK RAM";
        }

        // The step is a byte of the image itself: the frame handler reads it every frame.
        const int asked = step + (in.justPressed(Action::StepUp) ? 1 : 0) - (in.justPressed(Action::StepDown) ? 1 : 0);
        if (asked != step && asked >= 1 && asked <= 8) {
            machine.write(places, &Places::step, std::vector<std::uint8_t>{static_cast<std::uint8_t>(asked)});
            status = "THE STEP PATCHED IN THE IMAGE";
        }

        if (in.justPressed(Action::Park)) {
            if (running) {
                machine.stop();
                status = "PARKED WITH EVERY BYTE KEPT";
            } else {
                machine.run(Vm::Advance::Continuously);
                status = "RESUMED FROM WHERE IT STOOD";
            }
            running = !running;
        }

        palette = machine.read(places, &Places::palette);
        for (std::uint32_t t = 0; t < kTileCount; ++t) {
            tiles[t] = machine.read(places, &Places::tiles, t);
        }
        const std::vector<std::uint8_t> xy = machine.read(places, &Places::sprite);
        spriteX = xy[0];
        spriteY = xy[1];
        step    = machine.read(places, &Places::step).at(0);
    });

    // The panel's pictures — the swatches and the decoded tiles — are this program's own raster, painted
    // on the CPU from the bytes read above.
    std::vector<std::uint8_t> panelPixels(static_cast<std::size_t>(kPanelW) * kViewH * 4);
    std::uint64_t             panelGeneration = 0;
    const auto paintPanel = [&] {
        for (std::size_t i = 0; i < panelPixels.size(); i += 4) {
            panelPixels[i] = 16;  panelPixels[i + 1] = 16;  panelPixels[i + 2] = 24;  panelPixels[i + 3] = 255;
        }
        const auto pixel = [&](int x, int y) { return &panelPixels[(static_cast<std::size_t>(y) * kPanelW + x) * 4]; };

        // The 256 palette words, sixteen to a row.
        for (std::size_t w = 0; w < 256; ++w) {
            const int x0 = kSwatchX + static_cast<int>(w % 16) * kSwatchPx;
            const int y0 = kSwatchY + static_cast<int>(w / 16) * kSwatchPx;
            for (int y = 0; y < kSwatchPx - 1; ++y)
                for (int x = 0; x < kSwatchPx - 1; ++x) paintWord(pixel(x0 + x, y0 + y), wordAt(palette, w));
        }

        // Sixteen 4 bpp tiles in a row. A tile's row y is planes 0 and 1 at bytes 2y and 2y+1, planes 2 and
        // 3 at bytes 16+2y and 17+2y, the leftmost pixel in each byte's top bit; its color is object palette
        // 0's, where the sprite reads it — so 1 and 2 recolor these too. Index 0 is left as the panel.
        for (std::uint32_t t = 0; t < kTileCount; ++t) {
            const std::vector<std::uint8_t>& b = tiles[t];
            if (b.size() != 32) continue;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int bit   = 7 - x;
                    const int index = ((b[2 * y] >> bit) & 1) | (((b[2 * y + 1] >> bit) & 1) << 1) |
                                      (((b[16 + 2 * y] >> bit) & 1) << 2) | (((b[17 + 2 * y] >> bit) & 1) << 3);
                    if (index == 0) continue;
                    for (int sy = 0; sy < kTileScale; ++sy)
                        for (int sx = 0; sx < kTileScale; ++sx)
                            paintWord(pixel(kStripX + (static_cast<int>(t) * 8 + x) * kTileScale + sx,
                                            kStripY + y * kTileScale + sy),
                                      wordAt(palette, 128 + static_cast<std::size_t>(index)));
                }
        }
        ++panelGeneration;
    };

    FrameDrawState frame;
    loop.renderLoop([&]() {
        clearText();
        const auto heading = [&](int row, std::string_view name, std::string_view keys) {
            put(kHeadCol, row, name, palText);
            put(kKeyCol, row, keys, palLive);
        };
        const auto row = [&](int at, std::string_view name, std::string_view value) {
            put(kNameCol, at, name, palDim);
            put(kValueCol, at, value, palText);
        };

        put(kHeadCol, 1, "SNES COEXECUTION", palText);
        put(kHeadCol, 2, "ITS MEMORY READ AS IT RUNS", palDim);
        heading(4, "PALETTE", "1 2 WORD 129");
        heading(14, "VIDEO RAM TILES 0 TO 15", "");
        heading(18, "THE SPRITE", "3 4 X 32");
        row(19, "X", std::to_string(spriteX));
        row(20, "Y", std::to_string(spriteY));
        heading(22, "STEP IN THE IMAGE", "5 6");
        row(23, "STEP", std::to_string(step) + " A FRAME");
        heading(25, "THE MACHINE", "SPACE");
        row(26, "IT IS", running ? "RUNNING" : "PARKED");
        put(kNameCol, 28, "ARROWS MOVE THE SPRITE", palDim);
        put(kHeadCol, 30, status, palLive);

        paintPanel();

        frame.layers.clear();

        // The cartridge's picture, fitted to a slot the size of the console's largest picture.
        RasterContent picture = machine.video();
        picture.fit           = PixelSize{kSlotW, kSlotH};
        DrawLayer screen{.key = "cartridge"};
        screen.z       = 0;
        screen.size    = PixelSize{kViewW, kViewH};
        screen.scroll  = LayerScroll{0, -kSlotTop};
        screen.content = picture;
        frame.layers.push_back(screen);

        DrawLayer panel{.key = "panel"};
        panel.z       = 1;
        panel.size    = PixelSize{kViewW, kViewH};
        panel.scroll  = LayerScroll{-kPanelX, 0};
        panel.content = RasterContent{.pixels     = panelPixels,
                                      .width      = kPanelW,
                                      .height     = kViewH,
                                      .format     = RasterPixelFormat::Rgba8888,
                                      .generation = panelGeneration};
        frame.layers.push_back(panel);

        DrawLayer words{.key = "text"};
        words.z       = 2;
        words.size    = PixelSize{kViewW, kViewH};
        words.content = TileContent{.widthInTiles  = kMonW,
                                    .heightInTiles = kMonH,
                                    .cells         = std::span<const TileCell>(text),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(words);

        renderer.renderFrame(frame);
    });

    std::printf(
        "SNES co-execution — the demo cartridge on the left, its own memory on the right, read every tick\n"
        "through declared places. Arrows move the sprite. 1 and 2 write its palette word, 3 and 4 write its\n"
        "X in work RAM, 5 and 6 patch the step byte in the image while it runs, SPACE parks and resumes.\n\n");

    WindowedHost host{loop, platform};
    host.run();

    machine.stop();
    return 0;
}
