// SNES driver hosting — a media-player faceplate for a sound driver hosted the way an SNES game carries one.
//
// An SNES game's music is two programs: 65816 code on the console, and an SPC700 program the console code
// uploads to the audio unit and then talks to through four communication ports. This demo hosts exactly
// that as a resident driver on AudioSystem::SNES, and drives it through the handle host() returns, with
// the same verbs a Game Boy driver takes — play(id) / slots(...) / stop() / close().
//
// The driver is three sources in drivers/, each registered as a path the build bakes:
//
//   init.asm   65816, at $00:8000 — the driver's .init: sends the sound program to the audio unit through
//              the protocol the unit's boot program waits in, and starts it
//   tick.asm   65816, at $00:8400 — called every frame: hands a waiting command to the sound program on
//              the ports, and copies what the program reports into the bytes the game reads back
//   sound.asm  SPC700, at $00:A000 — the sound program: three songs on one voice, an effect on another, a
//              fade; the console never runs it, the init uploads it
//
// The binding says the driver is 65816 code; the sound program's image says it is SPC700, so the build
// assembles each file with its own instruction set's assembler.
//
// The faceplate, mouse only:
//   SONGS           play(id) — the song from its first note
//   EFFECT          slots(...) — the command byte written directly: a falling chirp on the second voice
//   FADE            stop() — the song fades out over a second and a half
//   EJECT           close() — the driver's voice closes
//   OUTPUT          the vmDriver mixer bus
// The readout is slots(): the song and the note the sound program reports, its master volume, the
// effect's ticks left, and the frames the tick has run. F toggles fullscreen.
//
// Modes:
//   (no args)   the window
//   --verify    headless, on a capture sink: checks that the driver is silent until a song is played,
//               that frames arrive, that a song change moves the published song and its notes step, that
//               the effect runs, that a fade takes the volume to zero and ends the song, and that a song
//               after it plays at full volume again; exits nonzero on any miss (CI runs this on every
//               platform)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "retropp/audio.h"          // AudioSink, AudioFrame — the capture sink --verify listens on
#include "retropp/audio_library.h"  // HostedDriverBinding, DriverImagePath, slots / slot
#include "retropp/audio_mixer.h"
#include "retropp/audio_system.h"   // AudioSystem::SNES, HostedDriver
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
#include "retropp/snes.h"           // snes::A — the register the init call rides
#include "retropp/windowed_host.h"

#include "panel.h"

using namespace retropp;
using namespace demo;

namespace {

// Where the driver's three images sit in the image it runs in.
constexpr std::uint32_t kInit  = 0x008000;
constexpr std::uint32_t kTick  = 0x008400;
constexpr std::uint32_t kSound = 0x00A000;

// The driver's bytes in work RAM: the mailbox the game writes, and what the tick copies back.
constexpr std::uint32_t kCommand = 0x7E0020;
constexpr std::uint32_t kSong    = 0x7E0030;
constexpr std::uint32_t kNote    = 0x7E0031;
constexpr std::uint32_t kVolume  = 0x7E0032;
constexpr std::uint32_t kEffect  = 0x7E0033;
constexpr std::uint32_t kFrames  = 0x7E0034;

// The sound program's commands beside the three song numbers.
constexpr std::uint8_t kPlayEffect = 0x10;
constexpr std::uint8_t kFadeOut    = 0x20;
constexpr std::uint8_t kNoSong     = 0xFF;
constexpr std::uint8_t kFullVolume = 0x60;

// The game-facing slots. A field's type is the slot's width, the declaration order is its index.
struct SoundSlots {
    std::optional<std::uint8_t> command;  // the mailbox (Write)
    std::optional<std::uint8_t> song;     // the song playing, $FF when none (Read)
    std::optional<std::uint8_t> note;     // the note playing, 0 to 7 (Read)
    std::optional<std::uint8_t> volume;   // the master volume, $60 at full (Read)
    std::optional<std::uint8_t> effect;   // the effect's ticks left (Read)
    std::optional<std::uint8_t> frames;   // the frames the tick has run, as a byte (Read)
};

// The one hardware site: the images, where they go, the tick, the init, and the mailbox each verb writes.
DriverId<SoundSlots> registerSoundDriver() {
    const HostedDriverBinding binding{
        .images    = {DriverImagePath{.base = kInit, .path = "examples/snes/driver/drivers/init.asm"},
                      DriverImagePath{.base = kTick, .path = "examples/snes/driver/drivers/tick.asm"},
                      DriverImagePath{.base = kSound,
                                      .path = "examples/snes/driver/drivers/sound.asm",
                                      .isa  = Isa::Spc700}},
        .tickEntry = kTick,
        .init      = Instruction::call(kInit, snes::A, /*fixedValue=*/0),
        .isa       = Isa::Wdc65816,
    };
    const DriverVerbs verbs{
        .play = {.music = Instruction::write(Location::memory(kCommand), 1)},
        .stop = Instruction::write(Location::memory(kCommand), 1, /*fixedValue=*/kFadeOut),
    };
    return AudioLibrary::instance().registerDriver(
        binding, verbs,
        slots(slot(&SoundSlots::command, kCommand, SlotDirection::Write),
              slot(&SoundSlots::song, kSong, SlotDirection::Read),
              slot(&SoundSlots::note, kNote, SlotDirection::Read),
              slot(&SoundSlots::volume, kVolume, SlotDirection::Read),
              slot(&SoundSlots::effect, kEffect, SlotDirection::Read),
              slot(&SoundSlots::frames, kFrames, SlotDirection::Read)));
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────
// The system's own production thread runs the driver; this program is the audio device, pulling frames
// at the rate a device would.

// A sink that opens no device: it keeps the pull, and the program calls it.
class CaptureSink final : public AudioSink {
public:
    void start(unsigned rate, int, AudioPullFn pull) override {
        rate_ = rate;
        pull_ = std::move(pull);
    }
    void stop() override { pull_ = nullptr; }

    // Pull `n` frames as a device would; answers how many came, and the loudest sample among them.
    std::pair<std::size_t, int> drain(std::size_t n) {
        std::vector<AudioFrame> out(n);
        const std::size_t got = pull_ ? pull_(std::span<AudioFrame>(out)) : 0;
        int peak = 0;
        for (std::size_t i = 0; i < got; ++i) {
            peak = std::max({peak, std::abs(static_cast<int>(out[i].left)), std::abs(static_cast<int>(out[i].right))});
        }
        return {got, peak};
    }
    [[nodiscard]] unsigned rate() const noexcept { return rate_; }

private:
    AudioPullFn pull_;
    unsigned    rate_ = 0;
};

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_driver --verify: a sound driver hosted on the SNES core, driven through its handle\n\n");

    CaptureSink       sink;
    AudioSystem::SNES sys{AudioKind::Chiptune, sink};
    const HostedDriver<SoundSlots> driver = sys.host(registerSoundDriver());

    // Listen for up to `limit`, a hundredth of a second at a time, until `until` says what was heard is
    // enough; answers the frames and the loudest sample heard.
    struct Heard {
        std::size_t frames = 0;
        int         peak   = 0;
        bool        met    = false;
    };
    const auto listen = [&](std::chrono::milliseconds limit, const std::function<bool(const SoundSlots&)>& until) {
        Heard      heard;
        const auto end = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            const auto [got, peak] = sink.drain(sink.rate() / 100);
            heard.frames += got;
            heard.peak = std::max(heard.peak, peak);
            if (until && until(driver.slots())) {
                heard.met = true;
                break;
            }
        }
        return heard;
    };
    const auto byte = [](const std::optional<std::uint8_t>& v) { return v.value_or(0); };

    // Before any play: the sound program is uploaded and running, and silent.
    const Heard idle = listen(std::chrono::milliseconds{500}, nullptr);
    std::printf("  before a song: %zu frames, loudest sample %d; the tick has run %u frames\n", idle.frames,
                idle.peak, static_cast<unsigned>(byte(driver.slots().frames)));
    check(idle.frames > 0, "frames arrive from the hosted driver");
    check(idle.peak == 0, "and it is silent until a song is played");
    check(byte(driver.slots().song) == kNoSong, "the sound program reports no song");

    // A song: the published song moves to it, it sounds, and its notes step.
    driver.play(1);
    const Heard started = listen(std::chrono::milliseconds{2000}, [](const SoundSlots& s) { return s.song == 1; });
    check(started.met, "play(1) moves the published song to 1");
    std::set<std::uint8_t> notes;
    const Heard playing = listen(std::chrono::milliseconds{1200}, [&notes](const SoundSlots& s) {
        notes.insert(s.note.value_or(0));
        return false;
    });
    std::printf("  song 1: loudest sample %d, %zu different notes in 1.2 s\n", playing.peak, notes.size());
    check(playing.peak > 0, "and it sounds");
    check(notes.size() >= 3, "and its notes step, a quarter of a second each");

    // The effect: the command byte written through slots(), and the effect's ticks counted down.
    driver.slots(SoundSlots{.command = kPlayEffect});
    const Heard effect = listen(std::chrono::milliseconds{1000}, [](const SoundSlots& s) { return s.effect > 0; });
    check(effect.met, "slots(command = effect) starts the effect");

    // The fade: the volume falls to zero, the song ends, and the output falls silent.
    driver.stop();
    const Heard faded = listen(std::chrono::milliseconds{4000},
                               [](const SoundSlots& s) { return s.volume == 0 && s.song == kNoSong; });
    check(faded.met, "stop() fades the volume to zero and ends the song");
    listen(std::chrono::milliseconds{200}, nullptr);  // what the resampler holds drains
    const Heard after = listen(std::chrono::milliseconds{300}, nullptr);
    check(after.frames > 0 && after.peak == 0, "and the output is silent after it");

    // Another song after the fade plays at full volume.
    driver.play(0);
    const Heard again = listen(std::chrono::milliseconds{2000}, [](const SoundSlots& s) {
        return s.song == 0 && s.volume == kFullVolume;
    });
    check(again.met, "play(0) after the fade plays song 0 at full volume");

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

enum class Action : std::uint8_t { Fullscreen };

// An optional byte as the int the readout draws, -1 for one not read.
[[nodiscard]] int optByte(std::optional<std::uint8_t> o) { return o.has_value() ? static_cast<int>(*o) : -1; }

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesDriver"},
        .window   = {.title = "Polyrhythm — SNES sound driver"},
        .viewport = ViewportResolution{kViewW, kViewH},
        .timing   = TimingProfile::Snes,
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{{Action::Fullscreen, {SDL_SCANCODE_F}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font   = renderer.loadAtlas("examples/snes/driver/assets/art/font.png", AssetDimensions{16, 16},
                                      ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                                      TransparentIndices::None, 0, AssetPolicy::Embed);
    fonts.text   = renderer.loadPaletteImage("examples/snes/driver/assets/palettes/font.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.active = renderer.loadPaletteImage("examples/snes/driver/assets/palettes/font_pick.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim    = renderer.loadPaletteImage("examples/snes/driver/assets/palettes/mono.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    // A flat dark faceplate tile: the low-z layer the panel's colored rectangles sit on.
    constexpr int                mapW = (kViewW + 7) / 8, mapH = (kViewH + 7) / 8;
    std::array<std::uint8_t, 64> faceArt{};
    const AtlasId                faceAtlas = renderer.uploadAtlas(faceArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1>   facePal{{Rgba8{18, 20, 28}}};
    const PaletteId              facePalId = renderer.uploadPalette(std::span<const Rgba8>(facePal));
    const std::vector<TileCell>  faceCells(static_cast<std::size_t>(mapW) * mapH,
                                           TileCell{.atlas = faceAtlas, .tile = 0, .palette = facePalId});

    // ── The audio: one SNES system, the driver hosted on it ─────────────────────────────────────
    AudioSystem::SNES              sys{AudioKind::Chiptune};
    const HostedDriver<SoundSlots> driver = sys.host(registerSoundDriver());

    PanelState state;
    int        flashTimer = 0;
    bool       dragging   = false;

    const auto applyOutput = [&](float v) {
        state.output = std::clamp(v, 0.0f, 1.0f);
        AudioMixer::instance().levels(
            AudioLevels{.vmDriver = static_cast<std::uint8_t>(std::lround(state.output * 255.0f))});
    };
    applyOutput(1.0f);

    // Perform the handle verb a clicked control names.
    const auto activate = [&](Control c) {
        if (!state.resident) return;
        if (const int song = padSong(c); song >= 0) {
            driver.play(static_cast<std::uint32_t>(song));
        } else if (c == Control::Effect) {
            driver.slots(SoundSlots{.command = kPlayEffect});
        } else if (c == Control::Fade) {
            driver.stop();
        } else if (c == Control::Eject) {
            driver.close();
            state.resident = false;
        }
    };

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());

        const Vec2i cursor = in.cursor();
        if (in.mouseJustPressed(MouseButton::Left)) {
            const Control c = hitTest(cursor);
            if (c == Control::Output) {
                dragging = true;
                applyOutput(outputValueAt(cursor));
            } else if (c != Control::None) {
                activate(c);
                state.flash = c;
                flashTimer  = 8;
            }
        }
        if (dragging) {
            if (!in.mouseHeld(MouseButton::Left)) dragging = false;
            else                                  applyOutput(outputValueAt(cursor));
        }
        if (flashTimer > 0 && --flashTimer == 0) state.flash = Control::None;

        // The live readout — the driver's published slots(), read back wait-free.
        if (state.resident) {
            const SoundSlots s = driver.slots();
            state.song   = optByte(s.song);
            state.note   = optByte(s.note);
            state.volume = optByte(s.volume);
            state.effect = optByte(s.effect);
            state.frames = optByte(s.frames);
        } else {
            state.song = state.note = state.volume = state.effect = state.frames = -1;
        }
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer face{.key = "faceplate"};
        face.z       = 0;
        face.size    = PixelSize{kViewW, kViewH};
        face.content = TileContent{.widthInTiles  = mapW,
                                   .heightInTiles = mapH,
                                   .cells         = std::span<const TileCell>(faceCells),
                                   .wrap          = TileWrap::Blank};
        face.regions = controlRegions(state);
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
        "SNES sound driver — a 65816 driver hosted on AudioSystem::SNES, uploading its SPC700 sound program\n"
        "to the audio unit at init. SONGS play(id) one of three songs; EFFECT writes the command slot for a\n"
        "chirp on the second voice; FADE is stop(), a fade over a second and a half; EJECT closes the driver.\n"
        "The readout is slots(), what the sound program reports through the ports. OUTPUT is the vmDriver\n"
        "mixer bus. Mouse only; F fullscreen; close to quit.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
