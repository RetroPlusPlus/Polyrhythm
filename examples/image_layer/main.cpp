// Image layer — a truecolor PNG registered once and shown as a raster layer.
//
// A planet painted in thousands of colors with a soft, transparent edge is loaded once with
// renderer.loadImage(...) and handed to a raster layer as RasterContent{.image = planet}. The renderer
// uploads nothing for it on any frame: it binds the image's own texture as it is. It sits over a
// star field drawn from an indexed sheet, and the star field shows through the planet's edge and around
// it, because the image carries its alpha per pixel.
//
// The image is loaded with the default policy, LoadFromPath, so the build copies planet.png beside the
// binary (into assets/ under the example's output directory) and the window reads it from there.
//
// Keys (the status line at the bottom shows each one and what it is set to):
//   ARROWS   move the planet (the layer's scroll)
//   F        fit: the image's own size, or twice its size
//   R        spin: the layer's transform turns the planet about its center, a degree a tick
//   A        alpha: the layer's alpha steps 100, 75, 50, 25
//   E        ripple: a Ripple in the layer's effects, on or off
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a GPU device with no window: loads the image under both policies and
//               requires the two to draw identically; reads the decoded PNG as the expected picture and
//               requires every opaque texel exactly, the backdrop wherever the image is transparent, and
//               the alpha-over of the backdrop within one step at the soft edge; requires a fit of twice
//               the size to repeat each texel over a 2×2 block; requires that no frame uploads a raster
//               and that a still frame skips its compose; requires a recompose when the layer names the
//               other image and a skip when it names the same one again; and requires that an id nobody
//               minted draws nothing. Exits nonzero on any miss (CI runs this on every platform); on
//               Windows on ARM, whose CI runner has no GPU device, exits 77 — a counted skip.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "retropp/asset_policy.h"
#include "retropp/asset_registry.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/image.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/palette.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/transform.h"
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
constexpr int kCell   = 8;
constexpr int kMapW   = kViewW / kCell;
constexpr int kMapH   = kViewH / kCell;
constexpr int kPlanet = 96;            // planet.png's size
constexpr int kFitted = kPlanet * 2;   // the size F fits it to

// ── The planet's layer ────────────────────────────────────────────────────────────────────────────
// Where the planet is and how it is shown. `cx`, `cy` are its center in viewport pixels.
struct View {
    int   cx     = kViewW / 2;
    int   cy     = kViewH / 2 - 16;
    bool  fitted = false;
    bool  spin   = false;
    float angle  = 0.0f;
    int   alpha  = 100;   // percent
    bool  ripple = false;
    int   ticks  = 0;
};

[[nodiscard]] int shownSize(const View& v) { return v.fitted ? kFitted : kPlanet; }

// A viewport-sized raster layer naming the image, placed by its scroll so the image's top-left lands at
// the center less half the shown size.
[[nodiscard]] DrawLayer planetLayer(ImageId image, const View& v) {
    const int half = shownSize(v) / 2;
    DrawLayer layer{.key = "planet"};
    layer.z       = 10;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.scroll  = LayerScroll{-(v.cx - half), -(v.cy - half)};
    layer.alpha   = static_cast<float>(v.alpha) / 100.0f;
    layer.content = RasterContent{.fit   = v.fitted ? PixelSize{kFitted, kFitted} : PixelSize{},
                                  .image = image};
    if (v.spin) {
        layer.transform = Transform::rotation(v.angle, static_cast<float>(v.cx), static_cast<float>(v.cy));
    }
    if (v.ripple) {
        layer.effects = {ScreenSpaceEffect{.kind      = ScreenSpaceEffectKind::Ripple,
                                           .amplitude = 3.0f,
                                           .frequency = 12.0f,
                                           .phase     = static_cast<float>(v.ticks) * 0.02f,
                                           .center    = Point{static_cast<float>(v.cx), static_cast<float>(v.cy)},
                                           .decay     = 3.0f}};
    }
    return layer;
}

// ── The star field ────────────────────────────────────────────────────────────────────────────────
// A tile map over stars.png's four tiles — empty sky, one, two and three stars — scattered by a hash of
// the cell, repeating as the layer scrolls.
struct StarField {
    AtlasManifest         sheet;
    PaletteId             palette{};
    std::vector<TileCell> cells;
};

[[nodiscard]] std::uint16_t starTile(int x, int y) {
    const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
    const unsigned r = (h ^ (h >> 13)) % 23u;
    return static_cast<std::uint16_t>(r < 3 ? r + 1 : 0);
}

[[nodiscard]] DrawLayer starLayer(const StarField& stars, int ticks) {
    DrawLayer layer{.key = "stars"};
    layer.z       = 0;
    layer.size    = PixelSize{kViewW, kViewH};
    layer.scroll  = LayerScroll{ticks / 2, 0};   // one pixel every two ticks
    layer.content = TileContent{.widthInTiles  = kMapW,
                                .heightInTiles = kMapH,
                                .cells         = std::span<const TileCell>(stars.cells),
                                .wrap          = TileWrap::Repeat};
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

// The status line: each key beside what it is set to.
[[nodiscard]] std::vector<Sprite> statusSprites(const View& v, const Fonts& fonts) {
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

    write(16, kLine1, "ARROWS", fonts.text, "kmove");
    write(16 + 7 * kGlyphPx, kLine1, "MOVE", fonts.dim, "vmove");
    write(16 + 13 * kGlyphPx, kLine1, "F", fonts.text, "kfit");
    write(16 + 15 * kGlyphPx, kLine1, v.fitted ? "FIT 192" : "FIT 96", fonts.dim, "vfit");
    write(16 + 24 * kGlyphPx, kLine1, "R", fonts.text, "kspin");
    write(16 + 26 * kGlyphPx, kLine1, v.spin ? "SPIN ON" : "SPIN OFF", fonts.dim, "vspin");

    write(16, kLine2, "A", fonts.text, "kalpha");
    write(16 + 2 * kGlyphPx, kLine2, "ALPHA " + std::to_string(v.alpha), fonts.dim, "valpha");
    write(16 + 13 * kGlyphPx, kLine2, "E", fonts.text, "kripple");
    write(16 + 15 * kGlyphPx, kLine2, v.ripple ? "RIPPLE ON" : "RIPPLE OFF", fonts.dim, "vripple");
    return out;
}

// ── --verify ──────────────────────────────────────────────────────────────────────────────────────
constexpr Rgba8 kBackdrop{.r = 18, .g = 20, .b = 34};

[[nodiscard]] std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int runVerify() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("image layer verify: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
        return kDeviceOptional ? 77 : 1;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        /*debug_mode=*/false, /*name=*/nullptr);
    if (!device) {
        std::printf("image layer verify: no GPU device (%s)%s\n", SDL_GetError(),
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
    int opaqueTexels = 0, clearTexels = 0, edgeTexels = 0;

    {
        Renderer r{device, /*window=*/nullptr, ViewportResolution{kViewW, kViewH}};
        r.automaticInterpolation(false);

        // A solid backdrop: one 8×8 tile of index 0 under a palette whose entry 0 is the backdrop color.
        const std::array<std::uint8_t, kCell * kCell> blank{};
        const AtlasId                                 sheet = r.uploadAtlas(blank.data(), kCell, kCell).atlasId;
        const std::array<Rgba8, 1>                    pal{{kBackdrop}};
        const PaletteId                               palette = r.uploadPalette(std::span<const Rgba8>(pal));
        const std::vector<TileCell> cells(static_cast<std::size_t>(kMapW) * kMapH,
                                          TileCell{.atlas = sheet, .tile = 0, .palette = palette});
        DrawLayer backdrop{.key = "backdrop"};
        backdrop.size    = PixelSize{kViewW, kViewH};
        backdrop.content = TileContent{.widthInTiles  = kMapW,
                                       .heightInTiles = kMapH,
                                       .cells         = std::span<const TileCell>(cells)};

        const ImageId fromDisk  = r.loadImage("examples/image_layer/assets/planet.png");
        const ImageId fromBytes = r.loadImage("examples/image_layer/assets/planet.png", AssetPolicy::Embed);
        check(fromDisk != fromBytes, "the two loads mint two images");

        View view;
        const auto frameFor = [&](ImageId image, const View& v) {
            FrameDrawState frame;
            frame.layers = {backdrop, planetLayer(image, v)};
            return frame;
        };
        const auto readAt = [](const std::vector<Rgba8>& px, int x, int y) {
            return px[static_cast<std::size_t>(y) * kViewW + static_cast<std::size_t>(x)];
        };
        const auto said = [](Rgba8 c) {
            return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
        };

        // 1. Both policies deliver the same bytes, so both images draw the same frame.
        const std::vector<Rgba8> diskFrame  = r.captureViewport(frameFor(fromDisk, view));
        const std::vector<Rgba8> bytesFrame = r.captureViewport(frameFor(fromBytes, view));
        check(diskFrame == bytesFrame, "the image loaded from disk and the one baked in draw the same frame");

        // 2. The decoded PNG is the expected picture: opaque texels exactly, transparent ones the backdrop,
        //    the soft edge the alpha-over of the texel on the backdrop within one step.
        const LoadedImage png = loadPngFromMemory(readFile(assetPath("examples/image_layer/assets/planet.png")));
        check(png.width == kPlanet && png.height == kPlanet, "planet.png is 96x96");
        const int ox = view.cx - kPlanet / 2, oy = view.cy - kPlanet / 2;
        for (int y = 0; y < png.height; ++y) {
            for (int x = 0; x < png.width; ++x) {
                const Rgba16 t16 = png.pixels[static_cast<std::size_t>(y) * png.width + static_cast<std::size_t>(x)];
                const Rgba8  t{.r = static_cast<std::uint8_t>(t16.r >> 8), .g = static_cast<std::uint8_t>(t16.g >> 8),
                               .b = static_cast<std::uint8_t>(t16.b >> 8), .a = static_cast<std::uint8_t>(t16.a >> 8)};
                const Rgba8 got = readAt(diskFrame, ox + x, oy + y);
                const std::string at = " at texel " + std::to_string(x) + "," + std::to_string(y) + ": ";
                if (t.a == 255) {
                    ++opaqueTexels;
                    check(got.r == t.r && got.g == t.g && got.b == t.b, "opaque" + at + said(got) + " (want " + said(t) + ")");
                } else if (t.a == 0) {
                    ++clearTexels;
                    check(got.r == kBackdrop.r && got.g == kBackdrop.g && got.b == kBackdrop.b,
                          "transparent" + at + said(got) + " (want the backdrop)");
                } else {
                    ++edgeTexels;
                    const auto over = [&](int c, int dst) {
                        return static_cast<int>(std::lround((c * t.a + dst * (255 - t.a)) / 255.0));
                    };
                    const Rgba8 want{.r = static_cast<std::uint8_t>(over(t.r, kBackdrop.r)),
                                     .g = static_cast<std::uint8_t>(over(t.g, kBackdrop.g)),
                                     .b = static_cast<std::uint8_t>(over(t.b, kBackdrop.b))};
                    check(std::abs(got.r - want.r) <= 1 && std::abs(got.g - want.g) <= 1 &&
                              std::abs(got.b - want.b) <= 1,
                          "soft edge" + at + said(got) + " (want " + said(want) + " within one)");
                }
            }
        }
        check(opaqueTexels > 0 && clearTexels > 0 && edgeTexels > 0,
              "the planet has opaque, transparent and soft-edge texels");

        // 3. Fitted to twice its size, each opaque texel covers a 2×2 block.
        View fitted   = view;
        fitted.fitted = true;
        const std::vector<Rgba8> fitFrame = r.captureViewport(frameFor(fromDisk, fitted));
        const int fx = fitted.cx - kFitted / 2, fy = fitted.cy - kFitted / 2;
        int       fitMisses = 0;
        for (int y = 0; y < png.height; ++y) {
            for (int x = 0; x < png.width; ++x) {
                const Rgba16 t16 = png.pixels[static_cast<std::size_t>(y) * png.width + static_cast<std::size_t>(x)];
                if ((t16.a >> 8) != 255) continue;
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx) {
                        const Rgba8 got = readAt(fitFrame, fx + 2 * x + sx, fy + 2 * y + sy);
                        if (got.r != (t16.r >> 8) || got.g != (t16.g >> 8) || got.b != (t16.b >> 8)) ++fitMisses;
                    }
            }
        }
        check(fitMisses == 0, "the fit repeats each opaque texel over a 2x2 block (" + std::to_string(fitMisses) +
                                  " pixels off)");

        // 4. A still frame: no raster is uploaded on any frame, and every frame after the first skips.
        const Renderer::RenderStats before = r.renderStats();
        const FrameDrawState        still  = frameFor(fromDisk, view);
        for (int i = 0; i < 5; ++i) r.renderFrame(still);
        const Renderer::RenderStats after = r.renderStats();
        check(after.rasterUploads == 0 && after.rasterSkips == 0, "no frame uploads a raster");
        check(after.composeSkips - before.composeSkips == 4,
              "four of five still frames skip their compose (" + std::to_string(after.composeSkips - before.composeSkips) +
                  " skipped)");

        // 5. Naming the other image recomposes, naming it back recomposes, naming the same again skips.
        const auto composedOn = [&](ImageId image) {
            const Renderer::RenderStats s0 = r.renderStats();
            r.renderFrame(frameFor(image, view));
            return r.renderStats().composePasses == s0.composePasses + 1;
        };
        check(composedOn(fromBytes), "the frame naming the other image composes");
        check(composedOn(fromDisk), "the frame naming the first image again composes");
        check(!composedOn(fromDisk), "the frame naming the same image skips");

        // 6. An id nobody minted draws nothing: the backdrop everywhere.
        const std::vector<Rgba8> none = r.captureViewport(frameFor(ImageId{999}, view));
        int                      shown = 0;
        for (const Rgba8 c : none) {
            if (c.r != kBackdrop.r || c.g != kBackdrop.g || c.b != kBackdrop.b) ++shown;
        }
        check(shown == 0, "an id nobody minted draws nothing (" + std::to_string(shown) + " pixels drawn)");
    }

    std::printf("image layer verify: %d opaque, %d transparent and %d soft-edge texels; %s\n", opaqueTexels,
                clearTexels, edgeTexels, failures == 0 ? "PASS" : "FAIL");
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}

// ── The window ────────────────────────────────────────────────────────────────────────────────────
enum class Action : std::uint8_t { Left, Right, Up, Down, Fit, Spin, Alpha, Ripple };

constexpr int kMoveStep = 2;   // pixels a tick while an arrow is held

}  // namespace

int main(int argc, char** argv) {
    // Set first in both modes: it points the asset root at the directory the binary runs from, where the
    // build copied planet.png.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "ImageLayer"},
        .window   = {.title = "Polyrhythm — image layer"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Left, {SDL_SCANCODE_LEFT}}, {Action::Right, {SDL_SCANCODE_RIGHT}},
                  {Action::Up, {SDL_SCANCODE_UP}},     {Action::Down, {SDL_SCANCODE_DOWN}},
                  {Action::Fit, {SDL_SCANCODE_F}},     {Action::Spin, {SDL_SCANCODE_R}},
                  {Action::Alpha, {SDL_SCANCODE_A}},   {Action::Ripple, {SDL_SCANCODE_E}}};
    platform.actions(map);

    // The image: decoded and uploaded once, then named by the layer every frame.
    const ImageId planet = renderer.loadImage("examples/image_layer/assets/planet.png");

    StarField stars;
    stars.sheet = renderer.loadAtlas("examples/image_layer/assets/stars.png",
                                     AssetDimensions{.width = kCell, .height = kCell}, ContentKind::Tileset,
                                     ReadOrder::LeftRightThenDown, 4);
    const std::array<Rgba8, 3> starColors{{Rgba8{.r = 10, .g = 12, .b = 24}, Rgba8{.r = 120, .g = 130, .b = 170},
                                           Rgba8{.r = 240, .g = 240, .b = 255}}};
    stars.palette = renderer.uploadPalette(std::span<const Rgba8>(starColors));
    stars.cells.reserve(static_cast<std::size_t>(kMapW) * kMapH);
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x)
            stars.cells.push_back(TileCell{.atlas   = stars.sheet.atlasId,
                                           .tile    = static_cast<std::uint16_t>(stars.sheet[starTile(x, y)].tile),
                                           .palette = stars.palette});

    Fonts fonts;
    fonts.font = renderer.loadAtlas("examples/image_layer/assets/art/font.png",
                                    AssetDimensions{.width = 16, .height = 16}, ContentKind::Tileset,
                                    ReadOrder::LeftRightThenDown, 64, TransparentIndices::None, 0,
                                    AssetPolicy::Embed);
    fonts.text = renderer.loadPaletteImage("examples/image_layer/assets/palettes/font.png",
                                           ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim  = renderer.loadPaletteImage("examples/image_layer/assets/palettes/mono.png",
                                           ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    View view;
    loop.simTick([&](const InputState& in) {
        ++view.ticks;
        if (in.isHeld(Action::Left)) view.cx -= kMoveStep;
        if (in.isHeld(Action::Right)) view.cx += kMoveStep;
        if (in.isHeld(Action::Up)) view.cy -= kMoveStep;
        if (in.isHeld(Action::Down)) view.cy += kMoveStep;
        if (in.justPressed(Action::Fit)) view.fitted = !view.fitted;
        if (in.justPressed(Action::Spin)) view.spin = !view.spin;
        if (in.justPressed(Action::Alpha)) view.alpha = view.alpha == 25 ? 100 : view.alpha - 25;
        if (in.justPressed(Action::Ripple)) view.ripple = !view.ripple;
        if (view.spin) view.angle = std::fmod(view.angle + 1.0f, 360.0f);
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        frame.layers.push_back(starLayer(stars, view.ticks));
        frame.layers.push_back(planetLayer(planet, view));

        const std::vector<Sprite> sprites = statusSprites(view, fonts);
        DrawLayer                 text{.key = "text"};
        text.z       = 20;
        text.size    = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(text);

        renderer.renderFrame(frame);
    });

    std::printf(
        "Image layer: a truecolor planet loaded once with loadImage and shown on a raster layer over a star\n"
        "field. ARROWS move it, F fits it to twice its size, R spins it, A steps its alpha, E turns a ripple\n"
        "on and off. Close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
