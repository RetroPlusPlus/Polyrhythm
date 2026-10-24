// Hello, world! — the bare minimum: open a window and show "Hello, world!" on screen. It loads one
// committed indexed image (examples/assets/hello_world.png — index 0 = background, 1 = ink) as a
// single-asset atlas, draws it as one sprite centered in the viewport, and runs until you close the
// window. Nothing else.
//
// (For a first *interactive* program — input + a scrolling world — see examples/controller_scrolling.cpp
// and docs/guide/getting-started.md.) Built on every CI platform so it keeps compiling against the live
// engine; run it on a dev machine (CI has no display).

#include <array>
#include <span>
#include <string>

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

    // Set the active config ONCE; the bare engine ctors below then inherit it (window + input from
    // EngineConfig::active, viewport + timing from the per-type defaults setActive fans out) — no
    // per-ctor threading. Explicit threading (e.g. RunLoop{clock, config.timing}) still works as an
    // override; this is the recommended minimal startup. A default config is the faithful GBC baseline.
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Hello World"},
        .window = {.title = "Hello, world!"}};
    EngineConfig::setActive(config);

    SteadyClock clock;
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};
    RunLoop     loop{clock};

    // Load the committed text image. loadAtlas decodes the PNG, uploads it once, and slices it — here
    // as ONE whole-image asset (ContentKind::Single) — returning a manifest whose single slot gives us
    // the image's atlas cell and pixel size. The assets dir is copied next to the executable after the
    // build; SDL_GetBasePath finds it.
    const AtlasManifest text = renderer.loadAtlas(
        "assets/hello_world.png",
        AssetDimensions::GameBoy8x8, ContentKind::Single,
        ReadOrder::LeftRightThenDown, /*count=*/0, TransparentIndices::GameBoy);

    // A two-color palette: index 0 is the sheet's transparent index (the background shows through),
    // index 1 is the ink. Color is applied at render time — never baked into the art.
    const std::array<Rgba8, 2> palette{{{0, 0, 0}, {235, 235, 245}}};
    const PaletteId pal = renderer.uploadPalette(std::span<const Rgba8>(palette));

    // One sprite, the whole image (the manifest's single slot), centered in the 160×144 viewport. The
    // sprite names its sheet (`atlas`) and palette directly — both per-sprite handles.
    const std::array<Sprite, 1> sprites{Sprite{
        .key     = "text",
        .x       = (config.viewport.width  - text[0].dimensions.width)  / 2,
        .y       = (config.viewport.height - text[0].dimensions.height) / 2,
        .size    = text[0].dimensions,
        .atlas   = text.atlasId,
        .tile    = text[0].tile,
        .palette = pal}};

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer layer{.key = "text"};
        layer.z       = 0;
        layer.size    = PixelSize{config.viewport.width, config.viewport.height};
        layer.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(std::move(layer));
        renderer.renderFrame(frame);
    });

    WindowedHost{loop, platform}.run();
    return 0;
}
