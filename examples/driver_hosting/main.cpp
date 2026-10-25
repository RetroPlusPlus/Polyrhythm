// Driver hosting — a media-player faceplate for two resident sound drivers.
//
// A game can host its OWN sound driver as a long-lived, addressable machine: register it on the
// AudioLibrary, host it on an AudioSystem, and drive it through the durable handle host() returns — using
// the same verbs as the audio player itself, play(id) / stop() / restart() / slots(...). This demo hosts two synthetic
// drivers on ONE AudioSystem, one of each family the surface supports, and gives them the IDENTICAL
// control column so the mirror image is the lesson: the same pads / faders / buttons drive both.
//
//   * LEFT — a RAM-FLAG driver: play / stop are realized as Instruction::write (the id lands in a memory
//     mailbox the driver polls each tick).
//   * RIGHT — an ARGUMENT driver: play / stop are realized as Instruction::call (the id rides a CPU
//     register into an entry the engine calls).
//
// Both expose the same surface — a music lane (play(id)), an sfx lane (play(id, Sfx)), a DRIVER VOL fader
// (slots(...)), STOP (its stop verb), RESET (restart — the declared .init performed again), EJECT (close),
// and a live slots() readout. The ONLY difference is the
// one line per verb in synthetic_drivers.h; the call sites here are identical. OUTPUT is the vmDriver mixer
// bus — one engine-side knob over both drivers. A system stop() would NOT close a resident driver; only its
// own close() does, which is why the handle owns close().
//
// The drivers are hand-assembled in synthetic_drivers.h so the demo needs no ROM. The panel drawing lives
// in panel.{h,cpp}; this file is the audio: registration, host(), and the verb calls behind each control.
//
// Mouse only: click a pad to cue it, drag a fader, click STOP / EJECT. F toggles fullscreen. Close the
// window to quit.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>       // TEMP --shot
#include <optional>
#include <span>
#include <string_view>   // TEMP --shot
#include <vector>

#include "retropp/audio_library.h"
#include "retropp/audio_mixer.h"
#include "retropp/audio_system.h"
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
#include "retropp/windowed_host.h"

#include "panel.h"
#include "synthetic_drivers.h"

using namespace retropp;
using namespace demo;

namespace {
enum class Action : std::uint8_t { Fullscreen };

// An optional<uint8_t> as an int the readout draws (-1 for a disengaged / unreadable field → "--").
[[nodiscard]] int optByte(std::optional<std::uint8_t> o) {
    return o.has_value() ? static_cast<int>(*o) : -1;
}
}  // namespace

int main(int argc, char** argv) {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "DriverHosting"},
        .window   = {.title = "Polyrhythm — hosted sound drivers"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Fullscreen, {SDL_SCANCODE_F}}};
    platform.actions(map);

    // ── Fonts (reused from the render-stats deck) ────────────────────────────────────────────────
    Fonts fonts;
    fonts.font = renderer.loadAtlas("examples/driver_hosting/assets/art/font.png",
                                    AssetDimensions{16, 16}, ContentKind::Tileset,
                                    ReadOrder::LeftRightThenDown, 64, TransparentIndices::None, 0,
                                    AssetPolicy::Embed);
    fonts.text   = renderer.loadPaletteImage("examples/driver_hosting/assets/palettes/font.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.active = renderer.loadPaletteImage("examples/driver_hosting/assets/palettes/font_pick.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim    = renderer.loadPaletteImage("examples/driver_hosting/assets/palettes/mono.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    // A flat dark faceplate tile: the low-z layer the panel's colored rects (regions) sit on.
    constexpr int mapW = (kViewW + 7) / 8, mapH = (kViewH + 7) / 8;
    std::array<std::uint8_t, 64> faceArt{};
    const AtlasId faceAtlas = renderer.uploadAtlas(faceArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1> facePal{{Rgba8{18, 20, 28}}};
    const PaletteId facePalId = renderer.uploadPalette(std::span<const Rgba8>(facePal));
    const std::vector<TileCell> faceCells(static_cast<std::size_t>(mapW) * mapH,
                                          TileCell{.atlas = faceAtlas, .tile = 0, .palette = facePalId});

    // ── The audio: one system, both drivers hosted ──────────────────────────────────────────────
    // Registration is the one hardware site; host() places the images, runs each driver's .init once, and
    // returns a durable typed handle. Both drivers share one slots struct, so both handles are one type.
    AudioSystem::GBC              sys{AudioKind::Chiptune};
    const DriverId<DemoSlots>   ramId = registerRamDriver();
    const DriverId<DemoSlots>   argId = registerArgDriver();
    HostedDriver<DemoSlots>     ram   = sys.host(ramId);
    HostedDriver<DemoSlots>     arg   = sys.host(argId);
    HostedDriver<DemoSlots>*    handles[2] = {&ram, &arg};

    PanelState state;
    state.left.title      = "RAM-FLAG DRIVER";
    state.left.mechanism  = "PLAY(ID) TO A MAILBOX";
    state.right.title     = "ARGUMENT DRIVER";
    state.right.mechanism = "PLAY(ID) IN A REGISTER";
    int     flashTimer = 0;
    Control dragging   = Control::None;

    const auto column = [&](int s) -> ColumnState& { return s == 0 ? state.left : state.right; };

    // A DRIVER VOL fader writes that driver's own volume slot (a driver-internal knob, distinct from the
    // engine's mixer bus). The slot encodes both master-volume nibbles; 1..7 avoids 0, which the driver
    // reads as "leave the volume alone" (the mailbox-zero convention).
    const auto applyVol = [&](int s, float v) {
        ColumnState& cs = column(s);
        cs.vol = std::clamp(v, 0.0f, 1.0f);
        const int vv = std::clamp(1 + static_cast<int>(std::lround(cs.vol * 6.0f)), 1, 7);
        if (cs.resident) handles[s]->slots(DemoSlots{.volume = static_cast<std::uint8_t>((vv << 4) | vv)});
    };
    // OUTPUT is the engine's vmDriver mixer bus — it scales every resident driver's voice at once.
    const auto applyOutput = [&](float v) {
        state.output = std::clamp(v, 0.0f, 1.0f);
        AudioMixer::instance().levels(
            AudioLevels{.vmDriver = static_cast<std::uint8_t>(std::lround(state.output * 255.0f))});
    };
    const auto applyFader = [&](Control c, float v) {
        if (c == Control::Output) applyOutput(v);
        else                      applyVol(side(c), v);
    };
    applyVol(0, 1.0f);
    applyVol(1, 1.0f);
    applyOutput(1.0f);

    // Perform the handle verb a clicked column control names — identical code for both drivers.
    const auto activate = [&](Control c) {
        const int s = side(c), l = local(c);
        if (s < 0) return;
        ColumnState&               cs = column(s);
        HostedDriver<DemoSlots>& h  = *handles[s];
        if (l >= 0 && l <= 3) {
            if (cs.resident) h.play(padSoundId(c));                    // music lane
        } else if (l >= 4 && l <= 6) {
            if (cs.resident) h.play(padSoundId(c), AudioType::Sfx);    // sfx lane
        } else if (l == 7) {
            if (cs.resident) h.stop();                                 // the driver's stop verb
        } else if (l == 10) {
            // Put the machine back the way it started — the driver's declared .init performed again,
            // which is what a console reset IS. Both synthetic drivers zero their state RAM in .init,
            // so the DRIVER VOL the fader wrote is gone afterward and the readout drops to 0.
            if (cs.resident) h.restart();
        } else if (l == 8) {
            h.close();                                                 // the resident voice closes
            cs.resident = false;
        }
    };

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());

        const Vec2i cursor = in.cursor();
        if (in.mouseJustPressed(MouseButton::Left)) {
            const Control c = hitTest(cursor);
            if (isFader(c)) {
                dragging = c;
                applyFader(c, faderValueAt(c, cursor));
            } else if (c != Control::None) {
                activate(c);
                if (padSoundId(c) != 0) {  // only pads flash
                    state.flash = c;
                    flashTimer  = 8;
                }
            }
        }
        if (dragging != Control::None) {
            if (!in.mouseHeld(MouseButton::Left)) dragging = Control::None;
            else                                  applyFader(dragging, faderValueAt(dragging, cursor));
        }
        if (flashTimer > 0 && --flashTimer == 0) state.flash = Control::None;

        // Live readouts — each driver's published slots(), read back wait-free.
        for (int s = 0; s < 2; ++s) {
            ColumnState& cs = column(s);
            if (cs.resident) {
                const DemoSlots ds = handles[s]->slots();
                cs.musicLastSeen = optByte(ds.musicLastSeen);
                cs.sfxLastSeen   = optByte(ds.sfxLastSeen);
                cs.volume        = optByte(ds.volume);
            } else {
                cs.musicLastSeen = cs.sfxLastSeen = cs.volume = -1;
            }
        }
    });

    // TEMP --shot: render one frame headless via captureViewport and write raw RGBA, then exit.
    if (argc > 1 && std::string_view(argv[1]) == "--shot") {
        state.left.musicLastSeen = 0x60;  state.left.sfxLastSeen = 0x40;  state.left.volume = 0x77;
        state.right.musicLastSeen = 0x90; state.right.sfxLastSeen = -1;    state.right.volume = 0x55;
        state.flash = Control::LMus1;
        FrameDrawState f;
        const std::vector<Region> regions = controlRegions(state);
        const std::vector<Sprite> sprites = textSprites(state, fonts);
        DrawLayer face{.key = "faceplate"};
        face.z = 0; face.size = PixelSize{kViewW, kViewH};
        face.content = TileContent{.widthInTiles = mapW, .heightInTiles = mapH,
                                   .cells = std::span<const TileCell>(faceCells), .wrap = TileWrap::Blank};
        face.regions = regions;
        f.layers.push_back(face);
        DrawLayer text{.key = "text"};
        text.z = 10; text.size = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        f.layers.push_back(text);
        const std::vector<Rgba8> px = renderer.captureViewport(f);
        std::ofstream out("/Users/erictomasso/.claude/jobs/caba2237/tmp/panel.rgba", std::ios::binary);
        out.write(reinterpret_cast<const char*>(px.data()),
                  static_cast<std::streamsize>(px.size() * sizeof(Rgba8)));
        std::printf("shot %dx%d %zu px\n", kViewW, kViewH, px.size());
        return 0;
    }

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        const std::vector<Region> regions = controlRegions(state);
        const std::vector<Sprite> sprites = textSprites(state, fonts);

        DrawLayer face{.key = "faceplate"};
        face.z       = 0;
        face.size    = PixelSize{kViewW, kViewH};
        face.content = TileContent{.widthInTiles = mapW, .heightInTiles = mapH,
                                   .cells = std::span<const TileCell>(faceCells),
                                   .wrap = TileWrap::Blank};
        face.regions = regions;
        frame.layers.push_back(face);

        DrawLayer text{.key = "text"};
        text.z       = 10;
        text.size    = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(text);

        renderer.renderFrame(frame);
    });

    std::printf(
        "driver hosting — two resident drivers on one AudioSystem, each driven through the handle host()\n"
        "returns. Both columns carry the SAME controls: music pads (play(id)), sfx pads (play(id, Sfx)),\n"
        "a DRIVER VOL fader (slots(...)), STOP (the stop verb), RESET (restart — the declared .init run\n"
        "again, which zeroes each driver's state RAM, so DRIVER VOL drops to 0), EJECT (close).\n"
        "LEFT realizes its verbs as\n"
        "mailbox writes, RIGHT as register calls — the only difference, and it is invisible at the call\n"
        "site. BOTTOM: the vmDriver mixer bus over both. Mouse only; F fullscreen; close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
