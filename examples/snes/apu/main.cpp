// SNES audio unit — eight voices, driven by the game the way the console's CPU drives them.
//
// The pieces, one file each:
//   audio_unit.h/.cpp   the audio: an AudioSystem::SNES holding the audio unit alone, reached as places —
//                       the ports, audio RAM, the S-DSP's registers — loading samples and keying the eight
//                       voices; the system plays what the chip makes
//   samples.h/.cpp      the chip's sample format: a sixteen-sample wave as one looping BRR block
//   panel.h/.cpp        the UI: eight voice columns, lit while they sound, their pitch and wave shown
//   main.cpp            this file: the keys, the loop, and --verify
//
// The other way to drive the audio unit — a 65816 sound driver the console runs, uploading its own SPC700
// program and talking to it on the ports — is examples/snes/driver/.
//
// Keys:
//   1-8           key that voice on, or off if it sounds; a click on its pad does the same
//   the mouse     drags a voice's pitch bar, its volume bar and its pan track; a click on its wave name
//                 is TAB for that voice
//   LEFT / RIGHT  select a voice
//   UP / DOWN     the selected voice's pitch
//   TAB           the selected voice's wave: square, saw, triangle, pulse
//   - / =         the selected voice's volume
//   [ / ]         the selected voice's pan, left or right — its left and right volume registers set unequally
//   F             fullscreen
//
// Modes:
//   (no args)   the window
//   --verify    headless: greets the boot program on the ports, loads the four waves, keys two voices on
//               and hears them, keys them off and hears silence, keys one on again and hears it; exits
//               nonzero on any miss (CI runs this on every platform)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include "retropp/audio.h"  // AudioFrame, AudioSink
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

#include "audio_unit.h"
#include "panel.h"
#include "samples.h"

using namespace retropp;
using namespace demo;

namespace {

// A C major scale, one note a voice, for a sixteen-sample wave: $1000 is the sample at its own rate,
// 2'000 Hz, and a pitch is that times the note's frequency over 2'000.
constexpr std::array<std::uint16_t, kVoices> kScale{0x0218, 0x0259, 0x02A3, 0x02CB, 0x0323, 0x0385, 0x03F3, 0x0430};
constexpr std::uint16_t kPitchStep = 0x0010;
constexpr std::uint16_t kPitchMax  = 0x3FFF;
constexpr std::uint8_t  kVolumeStep = 0x08;
constexpr std::uint8_t  kVolumeMax  = 0x7F;

// A voice's two volume registers from its volume and its pan: the side panned away from loses up to all
// of it, the side panned toward keeps the whole volume.
void applyVolume(AudioUnit& unit, const PanelState& state, int v) {
    const auto i      = static_cast<std::size_t>(v);
    const int  volume = state.volume[i];
    const int  pan    = state.pan[i];
    const auto left   = static_cast<std::uint8_t>(volume * std::min(8, 8 - pan) / 8);
    const auto right  = static_cast<std::uint8_t>(volume * std::min(8, 8 + pan) / 8);
    unit.volume(v, left, right);
}

// The four waves into the unit's four sample slots, and every voice on the square at its scale note.
void setUp(AudioUnit& unit, PanelState& state) {
    for (int slot = 0; slot < AudioUnit::kSlots; ++slot) {
        unit.loadSample(slot, waves()[static_cast<std::size_t>(slot)].block);
    }
    for (int v = 0; v < kVoices; ++v) {
        unit.source(v, 0);
        unit.pitch(v, kScale[static_cast<std::size_t>(v)]);
        state.pitch[static_cast<std::size_t>(v)]  = kScale[static_cast<std::size_t>(v)];
        state.wave[static_cast<std::size_t>(v)]   = waves()[0].name;
        state.volume[static_cast<std::size_t>(v)] = 0x30;
        state.pan[static_cast<std::size_t>(v)]    = 0;
        applyVolume(unit, state, v);
    }
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────

// A sink that is not a device: it keeps the pull the system hands it, and the checks pull through it
// themselves, as the device would, to hear what the unit made.
class ListeningSink final : public AudioSink {
public:
    void start(unsigned, int, AudioPullFn pull) override { pull_ = std::move(pull); }
    void stop() override { pull_ = nullptr; }

    // Listen for `span`: pull everything the system has as time passes, and return the loudest sample.
    int listen(std::chrono::milliseconds span) {
        int        peak     = 0;
        const auto deadline = std::chrono::steady_clock::now() + span;
        std::vector<AudioFrame> frames(4096);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const std::size_t got = pull_ ? pull_(std::span<AudioFrame>(frames)) : 0;
            for (std::size_t i = 0; i < got; ++i) {
                peak = std::max({peak, std::abs(static_cast<int>(frames[i].left)),
                                 std::abs(static_cast<int>(frames[i].right))});
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return peak;
    }

private:
    AudioPullFn pull_;
};

int runVerify() {
    int        failures = 0;
    const auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_apu --verify: the game drives the SNES audio unit the way the console's CPU does\n\n");

    // The unit runs on its own clock from here; the checks give each listening span real time to pass.
    ListeningSink sink;
    AudioUnit     unit{sink};
    PanelState    state;
    using namespace std::chrono_literals;

    check(unit.bootProgramReady(), "the boot program is ready on the ports");
    check(unit.bootProgramAcknowledgesAKick(), "the boot program acknowledges a kick");

    setUp(unit, state);
    check(unit.audioRam(0x0500) == 0xC3, "the first wave's block reads back from audio RAM");
    check(unit.dspRegister(0x0C) == 0x60, "a DSP register reads back what was written");
    check(sink.listen(200ms) == 0, "nothing sounds before a key-on");

    unit.keyOn(0);
    unit.keyOn(4);
    const int two = sink.listen(500ms);
    std::printf("  voices 0 and 4 keyed on: loudest sample %d\n", two);
    check(two > 0, "two voices sound");
    unit.keyOff(0);
    unit.keyOff(4);
    sink.listen(500ms);  // the release, and what the output still held
    check(sink.listen(500ms) == 0, "and are silent once keyed off");
    unit.keyOn(0);
    check(sink.listen(500ms) > 0, "and a voice sounds again when keyed on again");

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

enum class Action : std::uint8_t {
    Voice0, Voice1, Voice2, Voice3, Voice4, Voice5, Voice6, Voice7,
    SelectLeft, SelectRight, PitchUp, PitchDown, NextWave, VolumeUp, VolumeDown, PanLeft, PanRight, Fullscreen
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesApu"},
        .window   = {.title = "Polyrhythm — the SNES audio unit, driven by the game"},
        .viewport = ViewportResolution{kViewW, kViewH},
        .timing   = TimingProfile::Snes,
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Voice0, {SDL_SCANCODE_1}},         {Action::Voice1, {SDL_SCANCODE_2}},
                  {Action::Voice2, {SDL_SCANCODE_3}},         {Action::Voice3, {SDL_SCANCODE_4}},
                  {Action::Voice4, {SDL_SCANCODE_5}},         {Action::Voice5, {SDL_SCANCODE_6}},
                  {Action::Voice6, {SDL_SCANCODE_7}},         {Action::Voice7, {SDL_SCANCODE_8}},
                  {Action::SelectLeft, {SDL_SCANCODE_LEFT}},  {Action::SelectRight, {SDL_SCANCODE_RIGHT}},
                  {Action::PitchUp, {SDL_SCANCODE_UP}},       {Action::PitchDown, {SDL_SCANCODE_DOWN}},
                  {Action::NextWave, {SDL_SCANCODE_TAB}},     {Action::Fullscreen, {SDL_SCANCODE_F}},
                  {Action::VolumeUp, {SDL_SCANCODE_EQUALS}},  {Action::VolumeDown, {SDL_SCANCODE_MINUS}},
                  {Action::PanLeft, {SDL_SCANCODE_LEFTBRACKET}}, {Action::PanRight, {SDL_SCANCODE_RIGHTBRACKET}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font   = renderer.loadAtlas("examples/snes/apu/assets/art/font.png", AssetDimensions{16, 16},
                                      ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                                      TransparentIndices::None, 0, AssetPolicy::Embed);
    fonts.text   = renderer.loadPaletteImage("examples/snes/apu/assets/palettes/font.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.active = renderer.loadPaletteImage("examples/snes/apu/assets/palettes/font_pick.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim    = renderer.loadPaletteImage("examples/snes/apu/assets/palettes/mono.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    // A flat dark tile: the low-z layer the panel's colored rectangles sit on.
    constexpr int                mapW = (kViewW + 7) / 8, mapH = (kViewH + 7) / 8;
    std::array<std::uint8_t, 64> faceArt{};
    const AtlasId                faceAtlas = renderer.uploadAtlas(faceArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1>   facePal{{Rgba8{18, 20, 28}}};
    const PaletteId              facePalId = renderer.uploadPalette(std::span<const Rgba8>(facePal));
    const std::vector<TileCell>  faceCells(static_cast<std::size_t>(mapW) * mapH,
                                           TileCell{.atlas = faceAtlas, .tile = 0, .palette = facePalId});

    // ── The audio: the unit, running on its own clock, its sound on the device ───────────────────
    AudioUnit  unit;
    PanelState state;
    setUp(unit, state);
    std::array<int, kVoices> waveOf{};  // which of the four waves each voice plays
    const auto nextWave = [&](int v) {
        const auto i  = static_cast<std::size_t>(v);
        waveOf[i]     = (waveOf[i] + 1) % AudioUnit::kSlots;
        state.wave[i] = waves()[static_cast<std::size_t>(waveOf[i])].name;
        unit.source(v, waveOf[i]);  // takes effect at the voice's next key-on
    };

    const auto toggle = [&](int v) {
        auto& keyed = state.keyed[static_cast<std::size_t>(v)];
        keyed       = !keyed;
        if (keyed) unit.keyOn(v); else unit.keyOff(v);
    };
    const auto setPitch = [&](int v, std::uint16_t pitch) {
        state.pitch[static_cast<std::size_t>(v)] = pitch;
        unit.pitch(v, pitch);
    };
    // A bar or a track under the mouse, held: the value where the cursor is.
    Hit        dragging;
    const auto drag = [&](Hit hit, Vec2i cursor) {
        const float value = valueAt(hit.part, hit.voice, cursor);
        const auto  i     = static_cast<std::size_t>(hit.voice);
        switch (hit.part) {
            case Part::Pitch:
                setPitch(hit.voice, static_cast<std::uint16_t>(std::max(kPitchStep, static_cast<std::uint16_t>(value * 0x1000))));
                break;
            case Part::Volume:
                state.volume[i] = static_cast<std::uint8_t>(value * kVolumeMax);
                applyVolume(unit, state, hit.voice);
                break;
            case Part::Pan:
                state.pan[i] = static_cast<int>(std::lround(value * 16.0f)) - 8;
                applyVolume(unit, state, hit.voice);
                break;
            default:
                break;
        }
    };

    // The tick is the game's: the keys and the mouse, each landing on the chip as it is pressed. The unit
    // runs on its own clock beside it, and the system carries what the chip makes to the device.
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        for (int v = 0; v < kVoices; ++v) {
            if (in.justPressed(static_cast<Action>(v))) toggle(v);
        }
        if (in.mouseJustPressed(MouseButton::Left)) {
            const Hit hit = hitTest(in.cursor());
            if (hit.voice >= 0) state.selected = hit.voice;
            if (hit.part == Part::Pad) toggle(hit.voice);
            else if (hit.part == Part::Wave) nextWave(hit.voice);
            else if (hit.part != Part::None) { dragging = hit; drag(hit, in.cursor()); }
        }
        if (dragging.part != Part::None) {
            if (!in.mouseHeld(MouseButton::Left)) dragging = {};
            else                                  drag(dragging, in.cursor());
        }
        if (in.justPressed(Action::SelectLeft))  state.selected = (state.selected + kVoices - 1) % kVoices;
        if (in.justPressed(Action::SelectRight)) state.selected = (state.selected + 1) % kVoices;

        const auto    v     = static_cast<std::size_t>(state.selected);
        std::uint16_t pitch = state.pitch[v];
        if (in.justPressed(Action::PitchUp))   pitch = static_cast<std::uint16_t>(std::min<int>(pitch + kPitchStep, kPitchMax));
        if (in.justPressed(Action::PitchDown)) pitch = static_cast<std::uint16_t>(std::max<int>(pitch - kPitchStep, kPitchStep));
        if (pitch != state.pitch[v]) setPitch(state.selected, pitch);
        bool mixed = false;
        if (in.justPressed(Action::VolumeUp))   { state.volume[v] = static_cast<std::uint8_t>(std::min<int>(state.volume[v] + kVolumeStep, kVolumeMax)); mixed = true; }
        if (in.justPressed(Action::VolumeDown)) { state.volume[v] = static_cast<std::uint8_t>(std::max<int>(state.volume[v] - kVolumeStep, 0)); mixed = true; }
        if (in.justPressed(Action::PanLeft))    { state.pan[v] = std::max(state.pan[v] - 1, -8); mixed = true; }
        if (in.justPressed(Action::PanRight))   { state.pan[v] = std::min(state.pan[v] + 1, 8); mixed = true; }
        if (mixed) applyVolume(unit, state, state.selected);
        if (in.justPressed(Action::NextWave)) nextWave(state.selected);
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer face{.key = "panel"};
        face.z       = 0;
        face.size    = PixelSize{kViewW, kViewH};
        face.content = TileContent{.widthInTiles  = mapW,
                                   .heightInTiles = mapH,
                                   .cells         = std::span<const TileCell>(faceCells),
                                   .wrap          = TileWrap::Blank};
        face.regions = voiceRegions(state);
        frame.layers.push_back(face);

        const std::vector<Sprite> sprites = textSprites(state, fonts);
        DrawLayer                 text{.key = "text"};
        text.z       = 10;
        text.size    = PixelSize{kViewW, kViewH};
        text.content = SpriteContent{.sprites = std::span<const Sprite>(sprites)};
        frame.layers.push_back(text);

        renderer.renderFrame(frame);
    });

    std::printf(
        "The SNES audio unit, driven by the game: four waves in audio RAM, eight voices on the S-DSP, no\n"
        "65816 program. 1-8 key a voice (or click its column); LEFT/RIGHT select one; UP/DOWN its pitch;\n"
        "-/= its volume; [/] its pan; TAB its wave, taken at its next key-on; F fullscreen; close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
