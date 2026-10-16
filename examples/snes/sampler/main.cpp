// SNES sampler — audio files played through the console's sound chip, cued like any other audio.
//
// The game registers audio files on the AudioLibrary — the same registerAudio("x.wav") the Game Boy's
// audio-file path takes, plus a .brr sample in the chip's own format — and plays them on an
// AudioSystem::SNES of the Chiptune kind with play(id, {.voice, .effect}). The system converts each file to
// the chip's sample format the first time it is cued and keeps it in audio RAM whole when it fits, or
// delivers it through the voice's window as the voice plays when it does not — LONG is six seconds, longer
// than all of the sound chip's RAM. It hosts the engine's sound driver for the chip by itself, and keys the
// cue's voice on with the cue's pitch, volume and pan, playing the way the cue's mode says — round until
// stopped, once, or struck again at a tempo. The chip's one effect path is set for the whole system with
// echo(Echo{…}) / echo(Reverb{…}); a cue's .echo / .reverb say whether the voice feeds it. effect(voice,
// AudioEffect{…}) changes a voice as it plays — pitch, volume, pan, the sends — with no new key-on;
// stop(voice) keys one voice off; stop() keys them all off.
//
// The pieces, one file each:
//   panel.h/.cpp   the UI: eight voice columns, each with the sample it strikes, its pitch, volume and
//                  pan, and the effect on its cue
//   main.cpp       this file: the registrations, the keys, the loop, and --verify
//
// Keys:
//   1-8           strike that voice with its sample, or stop it if it is sounding; a click on its pad does
//                 the same
//   SPACE         stop() — every voice off
//   the mouse     drags a voice's pitch bar, its volume bar and its pan track; a click on its sample name
//                 is TAB for that voice
//   LEFT / RIGHT  select a voice
//   UP / DOWN     the selected voice's pitch
//   TAB           the selected voice's sample: kick, snare, hat, pluck, square, long — taken at its next
//                 strike
//   E             send the selected voice into the system's effect path, or take it out
//   R             switch the system's one path between an echo and a reverb
//   M             the selected voice's play mode: continuous, once, repeat — taken at its next strike
//   9 / 0         the selected voice's repeat tempo, strikes a minute
//   F             fullscreen
// Pitch, volume, pan and the send follow the voice as it plays; the sample is read at the key-on.
//
// Modes:
//   (no args)   the window
//   --verify    headless: cues the kick once and hears it play out, sets the path to an echo and hears the
//               square feed it and keep sounding, changes its pitch as it plays, stops its voice and hears
//               silence, cues a one-shot once and hears it play out while a continuous one holds, sets the
//               path to a reverb and cues two voices feeding it, changes the path under a sounding voice and
//               hears it play on, stops them all, then cues LONG once and hears it through to its end — its
//               silences where the file has them — while a short sample sounds on another voice; exits
//               nonzero on any miss (CI runs this on every platform)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/audio.h"  // AudioFrame, AudioSink
#include "retropp/audio_effect.h"
#include "retropp/audio_library.h"
#include "retropp/audio_system.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/windowed_host.h"

#include "panel.h"

using namespace retropp;
using namespace demo;

namespace {

// The samples: five audio files the engine converts, and one already in the chip's format. LONG is a
// six-second phrase, longer than the sound chip's RAM, with a gap of silence from 3.0 s to 3.4 s and
// silence from 5.6 s.
struct Sample {
    std::string_view name;
    AudioId          id;
};
std::array<Sample, 6> registerSamples() {
    // One registration a statement: the build reads each call's path to bake the file into the binary.
    AudioLibrary& lib    = AudioLibrary::instance();
    const AudioId kick   = lib.registerAudio("examples/snes/sampler/assets/samples/kick.wav", AudioType::Sfx, AssetPolicy::Embed);
    const AudioId snare  = lib.registerAudio("examples/snes/sampler/assets/samples/snare.wav", AudioType::Sfx, AssetPolicy::Embed);
    const AudioId hat    = lib.registerAudio("examples/snes/sampler/assets/samples/hat.wav", AudioType::Sfx, AssetPolicy::Embed);
    const AudioId pluck  = lib.registerAudio("examples/snes/sampler/assets/samples/pluck.wav", AudioType::Sfx, AssetPolicy::Embed);
    const AudioId square = lib.registerAudio("examples/snes/sampler/assets/samples/square.brr", AudioType::Sfx, AssetPolicy::Embed);
    const AudioId phrase = lib.registerAudio("examples/snes/sampler/assets/samples/long.wav", AudioType::Sfx, AssetPolicy::Embed);
    return {{{"KICK", kick}, {"SNARE", snare}, {"HAT", hat}, {"PLUCK", pluck}, {"SQUARE", square}, {"LONG", phrase}}};
}

// The play modes, cycled by M; a repeat's tempo is the voice's own.
enum class Mode : std::uint8_t { Continuous, Once, Repeat };
constexpr float kTempoStep = 10.0f;
constexpr float kTempoMin  = 30.0f;
constexpr float kTempoMax  = 300.0f;
PlayMode playModeFor(Mode mode, float tempo) {
    switch (mode) {
        case Mode::Once:   return PlayMode::once();
        case Mode::Repeat: return PlayMode::repeat(tempo);
        default:           return PlayMode::continuous();
    }
}
std::string modeName(Mode mode, float tempo) {
    switch (mode) {
        case Mode::Once:   return "ONCE";
        case Mode::Repeat: return "REP " + std::to_string(static_cast<int>(tempo));
        default:           return "CONT";
    }
}

// The system's one effect path, and the two configurations R switches it between. Its shape — an echo or a
// reverb — is the system's, set with snes.echo(…); it is not per voice. Each voice either feeds the path (E)
// or does not — one send, since the chip has one path.
const Echo   kEcho{.delay = 0.192f, .feedback = 0.4f, .level = 0.6f};
const Reverb kReverb{.decay = 0.7f, .level = 0.5f};
std::string_view sendName(bool send) { return send ? "SEND" : ""; }

// Each voice's cue, as the panel shows it: a pitch register value ($1000 as recorded), a volume $00-$7F, a
// pan -8..8 — the chip's own units, which the cue takes in audio terms — and whether the voice feeds the
// system's effect path.
AudioEffect effectFor(std::uint16_t pitch, std::uint8_t volume, int pan, bool send) {
    return AudioEffect{.pitch  = static_cast<float>(pitch) / 4096.0f,
                       .volume = static_cast<float>(volume) / 127.0f,
                       .pan    = static_cast<float>(pan) / 8.0f,
                       .echo   = send};  // the voice's send into the system's one path
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────

// A sink that is not a device: it keeps the pull the system hands it, and the checks pull through it
// themselves, as the device would, to hear what the chip made.
class ListeningSink final : public AudioSink {
public:
    void start(unsigned rate, int, AudioPullFn pull) override {
        rate_ = rate;
        pull_ = std::move(pull);
    }
    void stop() override { pull_ = nullptr; }

    [[nodiscard]] unsigned rate() const noexcept { return rate_; }

    // Record `seconds` of what the system makes, frame for frame as the device would take it, giving up
    // after `limit`. `pulled` runs after each pull with the seconds recorded so far. The frames are the
    // machine's own time, however fast the pulls come.
    std::vector<AudioFrame> record(double seconds, std::chrono::milliseconds limit,
                                   const std::function<void(double)>& pulled) {
        std::vector<AudioFrame> heard;
        const auto              want     = static_cast<std::size_t>(seconds * rate_);
        const auto              deadline = std::chrono::steady_clock::now() + limit;
        std::vector<AudioFrame> frames(4096);
        while (heard.size() < want && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const std::size_t got = pull_ ? pull_(std::span<AudioFrame>(frames)) : 0;
            heard.insert(heard.end(), frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(got));
            pulled(static_cast<double>(heard.size()) / rate_);
        }
        return heard;
    }

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
    unsigned    rate_ = 0;
};

// The loudest sample on one side of `frames` between two times, in seconds after frame `origin`.
int loudest(const std::vector<AudioFrame>& frames, std::size_t origin, unsigned rate, double from, double to,
            bool left) {
    int        peak  = 0;
    const auto first = origin + static_cast<std::size_t>(from * rate);
    const auto last  = std::min(frames.size(), origin + static_cast<std::size_t>(to * rate));
    for (std::size_t i = first; i < last; ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(left ? frames[i].left : frames[i].right)));
    }
    return peak;
}

int runVerify() {
    int        failures = 0;
    const auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_sampler --verify: audio files through the SNES's sound chip\n\n");
    using namespace std::chrono_literals;

    const std::array<Sample, 6> samples = registerSamples();
    const AudioId kick = samples[0].id, hat = samples[2].id, pluck = samples[3].id, square = samples[4].id,
                  phrase = samples[5].id;
    ListeningSink     sink;
    AudioSystem::SNES snes{AudioKind::Chiptune, sink};

    check(sink.listen(200ms) == 0, "nothing sounds before a cue");

    snes.play(kick, {.voice = 0, .mode = PlayMode::once()});
    const int one = sink.listen(600ms);
    std::printf("  the kick on voice 0, once: loudest sample %d\n", one);
    check(one > 0, "an audio file cued once sounds through the chip");
    sink.listen(400ms);
    check(sink.listen(300ms) == 0, "and is silent once its pass has played out");

    // The effect path, set once for the system as an echo that repeats once; the square feeds it.
    snes.echo(Echo{.delay = 0.096f, .feedback = 0.0f, .level = 0.5f});
    snes.play(square, {.voice = 1, .effect = AudioEffect{.pitch = 0.5f, .echo = true}});
    check(sink.listen(500ms) > 0, "the .brr square sounds, an octave down, feeding the echo path");
    check(sink.listen(500ms) > 0, "and keeps sounding — it loops");
    snes.effect(1, AudioEffect{.pitch = 2.0f, .volume = 0.5f});
    check(sink.listen(500ms) > 0, "and plays on through a change of pitch and volume");
    snes.stop(1);
    sink.listen(600ms);  // the key-off, the release, and the echo's one repeat
    check(sink.listen(300ms) == 0, "stop(voice) keys it off");

    // The modes: the kick once ends by itself; the kick continuous plays round until stopped.
    snes.play(kick, {.voice = 5, .mode = PlayMode::once()});
    snes.play(kick, {.voice = 6, .mode = PlayMode::continuous(), .effect = AudioEffect{.volume = 0.3f}});
    sink.listen(600ms);
    snes.stop(6);
    sink.listen(300ms);
    check(sink.listen(300ms) == 0, "a kick cued once has ended by itself; the continuous one holds until stopped");

    // The path reconfigured as a reverb, for the system; voice 3 feeds it with a reverb send.
    snes.echo(kReverb);
    snes.play(pluck, {.voice = 2, .effect = AudioEffect{.pan = -0.5f}});
    snes.play(square, {.voice = 3, .effect = AudioEffect{.volume = 0.5f, .reverb = true}});
    check(sink.listen(400ms) > 0, "two voices cued together sound, voice 3 feeding the reverb");

    // The path changed on a sounding voice — a reverb, then an echo — never cuts the voice off.
    snes.echo(Echo{.delay = 0.128f, .feedback = 0.3f, .level = 0.5f});
    check(sink.listen(400ms) > 0, "a voice keeps sounding as the system's path changes under it");
    snes.stop();
    sink.listen(1000ms);  // the releases and the path's tail
    check(sink.listen(300ms) == 0, "stop() keys every voice off");

    // LONG, once, panned hard left; a second in, the hat on another voice, panned hard right — so the left
    // side hears LONG alone and the right side the hat. Every time is measured from LONG's first sound.
    bool cued = true;
    try {
        snes.play(phrase, {.voice = 0, .mode = PlayMode::once(), .effect = AudioEffect{.pan = -1.0f}});
    } catch (const std::exception&) {
        cued = false;
    }
    check(cued, "LONG, a file longer than the sound chip's RAM, cued once");
    bool                          hatCued = false;
    const std::vector<AudioFrame> heard   = sink.record(6.8, 30s, [&](double seconds) {
        if (!hatCued && seconds >= 1.0) {
            snes.play(hat, {.voice = 1, .mode = PlayMode::once(), .effect = AudioEffect{.pan = 1.0f}});
            hatCued = true;
        }
    });
    const unsigned rate   = sink.rate();
    std::size_t    origin = 0;
    while (origin < heard.size() && heard[origin].left == 0) {
        ++origin;
    }
    std::printf("  LONG's first sound at frame %zu; %zu frames heard after it\n", origin, heard.size() - origin);
    const auto side = [&](double from, double to) { return loudest(heard, origin, rate, from, to, true); };
    check(side(2.4, 2.9) > 0, "it sounds from 2.4 s to 2.9 s, past where a sample loaded whole runs out of room");
    check(side(3.1, 3.3) == 0, "it is silent from 3.1 s to 3.3 s, the file's own gap at its own place");
    check(side(3.6, 4.0) > 0, "it sounds again from 3.6 s to 4.0 s");
    check(heard.size() >= origin + static_cast<std::size_t>(6.5 * rate) && side(6.0, 6.5) == 0,
          "it is silent from 6.0 s on: the pass ended on the file's last block");
    check(loudest(heard, origin, rate, 0.0, 6.5, false) > 0, "the hat cued on another voice while LONG plays sounds");

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

enum class Action : std::uint8_t {
    Voice0, Voice1, Voice2, Voice3, Voice4, Voice5, Voice6, Voice7,
    StopAll, SelectLeft, SelectRight, PitchUp, PitchDown, NextSample, Send, PathSwitch,
    NextMode, TempoDown, TempoUp, VolumeUp, VolumeDown, PanLeft, PanRight, Fullscreen
};

constexpr std::uint16_t kPitchStep  = 0x0080;
constexpr std::uint16_t kPitchMax   = 0x3FFF;
constexpr std::uint8_t  kVolumeStep = 0x08;
constexpr std::uint8_t  kVolumeMax  = 0x7F;

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesSampler"},
        .window   = {.title = "Polyrhythm — SNES sampler"},
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
                  {Action::StopAll, {SDL_SCANCODE_SPACE}},    {Action::Fullscreen, {SDL_SCANCODE_F}},
                  {Action::SelectLeft, {SDL_SCANCODE_LEFT}},  {Action::SelectRight, {SDL_SCANCODE_RIGHT}},
                  {Action::PitchUp, {SDL_SCANCODE_UP}},       {Action::PitchDown, {SDL_SCANCODE_DOWN}},
                  {Action::NextSample, {SDL_SCANCODE_TAB}},   {Action::Send, {SDL_SCANCODE_E}},
                  {Action::PathSwitch, {SDL_SCANCODE_R}},      {Action::NextMode, {SDL_SCANCODE_M}},
                  {Action::TempoDown, {SDL_SCANCODE_9}},     {Action::TempoUp, {SDL_SCANCODE_0}},
                  {Action::VolumeUp, {SDL_SCANCODE_EQUALS}},  {Action::VolumeDown, {SDL_SCANCODE_MINUS}},
                  {Action::PanLeft, {SDL_SCANCODE_LEFTBRACKET}}, {Action::PanRight, {SDL_SCANCODE_RIGHTBRACKET}}};
    platform.actions(map);

    Fonts fonts;
    fonts.font   = renderer.loadAtlas("examples/snes/sampler/assets/art/font.png", AssetDimensions{16, 16},
                                      ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64,
                                      TransparentIndices::None, 0, AssetPolicy::Embed);
    fonts.text   = renderer.loadPaletteImage("examples/snes/sampler/assets/palettes/font.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.active = renderer.loadPaletteImage("examples/snes/sampler/assets/palettes/font_pick.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);
    fonts.dim    = renderer.loadPaletteImage("examples/snes/sampler/assets/palettes/mono.png",
                                             ReadOrder::LeftRightThenDown, 0, AssetPolicy::Embed);

    // A flat dark tile: the low-z layer the panel's colored rectangles sit on.
    constexpr int                mapW = (kViewW + 7) / 8, mapH = (kViewH + 7) / 8;
    std::array<std::uint8_t, 64> faceArt{};
    const AtlasId                faceAtlas = renderer.uploadAtlas(faceArt.data(), 8, 8).atlasId;
    const std::array<Rgba8, 1>   facePal{{Rgba8{18, 20, 28}}};
    const PaletteId              facePalId = renderer.uploadPalette(std::span<const Rgba8>(facePal));
    const std::vector<TileCell>  faceCells(static_cast<std::size_t>(mapW) * mapH,
                                           TileCell{.atlas = faceAtlas, .tile = 0, .palette = facePalId});

    // ── The audio: the samples on the library, one SNES system to cue them on ──────────────────────
    const std::array<Sample, 6> samples = registerSamples();
    AudioSystem::SNES           snes{AudioKind::Chiptune};
    snes.echo(kEcho);  // the effect path starts as an echo; R switches it to a reverb and back

    PanelState                 state;
    std::array<int, kVoices>   sampleOf{};  // which sample each voice strikes
    std::array<bool, kVoices>  send{};      // the voice feeds the system's effect path, or does not
    std::array<Mode, kVoices>  modeOf{};
    std::array<float, kVoices> tempoOf{};
    bool                       pathIsReverb = false;  // the system's one path: an echo, or a reverb
    for (int v = 0; v < kVoices; ++v) {
        const auto i    = static_cast<std::size_t>(v);
        sampleOf[i]     = v % static_cast<int>(samples.size());
        state.wave[i]   = samples[static_cast<std::size_t>(sampleOf[i])].name;
        state.pitch[i]  = 0x1000;
        state.volume[i] = 0x60;
        state.pan[i]    = 0;
        state.effect[i] = sendName(send[i]);
        tempoOf[i]      = 120.0f;
        state.mode[i]   = modeName(modeOf[i], tempoOf[i]);
    }
    state.systemPath = "ECHO 192 MS";

    // The Voice a runtime voice number names: an element of the system's `voices`, copied into a cue.
    const auto voiceOf = [](int v) { return AudioSystem::SNES::voices[static_cast<std::size_t>(v)]; };

    // A strike: the voice's sample at the voice's pitch, volume, pan and sends. A second press on a
    // sounding voice stops it.
    const auto strike = [&](int v) {
        const auto i = static_cast<std::size_t>(v);
        snes.play(samples[static_cast<std::size_t>(sampleOf[i])].id,
                  {.voice  = voiceOf(v),
                   .mode   = playModeFor(modeOf[i], tempoOf[i]),
                   .effect = effectFor(state.pitch[i], state.volume[i], state.pan[i], send[i])});
        state.keyed[i] = true;
    };
    const auto toggle = [&](int v) {
        const auto i = static_cast<std::size_t>(v);
        if (state.keyed[i]) {
            snes.stop(voiceOf(v));
            state.keyed[i] = false;
        } else {
            strike(v);
        }
    };
    const auto nextSample = [&](int v) {
        const auto i  = static_cast<std::size_t>(v);
        sampleOf[i]   = (sampleOf[i] + 1) % static_cast<int>(samples.size());
        state.wave[i] = samples[static_cast<std::size_t>(sampleOf[i])].name;
    };
    // A change to a voice's pitch, volume, pan or sends reaches it as it plays.
    const auto changed = [&](int v) {
        const auto i = static_cast<std::size_t>(v);
        snes.effect(voiceOf(v), effectFor(state.pitch[i], state.volume[i], state.pan[i], send[i]));
    };
    const auto nextMode = [&](int v) {
        const auto i  = static_cast<std::size_t>(v);
        modeOf[i]     = static_cast<Mode>((static_cast<int>(modeOf[i]) + 1) % 3);
        state.mode[i] = modeName(modeOf[i], tempoOf[i]);
    };
    const auto tempo = [&](int v, float step) {
        const auto i  = static_cast<std::size_t>(v);
        tempoOf[i]    = std::clamp(tempoOf[i] + step, kTempoMin, kTempoMax);
        state.mode[i] = modeName(modeOf[i], tempoOf[i]);
    };
    const auto toggleSend = [&](int v) {
        const auto i    = static_cast<std::size_t>(v);
        send[i]         = !send[i];
        state.effect[i] = sendName(send[i]);
        changed(v);
    };
    // R switches the system's one path between an echo and a reverb; every voice feeding it follows.
    const auto switchPath = [&]() {
        pathIsReverb = !pathIsReverb;
        if (pathIsReverb) {
            snes.echo(kReverb);
            state.systemPath = "REVERB";
        } else {
            snes.echo(kEcho);
            state.systemPath = "ECHO 192 MS";
        }
    };
    // A bar or a track under the mouse, held: the value where the cursor is.
    Hit        dragging;
    const auto drag = [&](Hit hit, Vec2i cursor) {
        const float value = valueAt(hit.part, hit.voice, cursor);
        const auto  i     = static_cast<std::size_t>(hit.voice);
        switch (hit.part) {
            case Part::Pitch:
                state.pitch[i] = static_cast<std::uint16_t>(std::max<int>(kPitchStep, static_cast<int>(value * 0x1000)));
                break;
            case Part::Volume:
                state.volume[i] = static_cast<std::uint8_t>(value * kVolumeMax);
                break;
            case Part::Pan:
                state.pan[i] = static_cast<int>(std::lround(value * 16.0f)) - 8;
                break;
            default:
                return;
        }
        changed(hit.voice);
    };

    // The tick is the game's: a key strikes a voice or stops one, and the system does the rest.
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen)) platform.window().fullscreen(!platform.window().fullscreen());
        for (int v = 0; v < kVoices; ++v) {
            if (in.justPressed(static_cast<Action>(v))) toggle(v);
        }
        if (in.justPressed(Action::StopAll)) {
            snes.stop();
            state.keyed.fill(false);
        }
        if (in.mouseJustPressed(MouseButton::Left)) {
            const Hit hit = hitTest(in.cursor());
            if (hit.voice >= 0) state.selected = hit.voice;
            if (hit.part == Part::Pad) toggle(hit.voice);
            else if (hit.part == Part::Wave) nextSample(hit.voice);
            else if (hit.part != Part::None) { dragging = hit; drag(hit, in.cursor()); }
        }
        if (dragging.part != Part::None) {
            if (!in.mouseHeld(MouseButton::Left)) dragging = {};
            else                                  drag(dragging, in.cursor());
        }
        if (in.justPressed(Action::SelectLeft))  state.selected = (state.selected + kVoices - 1) % kVoices;
        if (in.justPressed(Action::SelectRight)) state.selected = (state.selected + 1) % kVoices;

        const auto v     = static_cast<std::size_t>(state.selected);
        bool       moved = false;
        if (in.justPressed(Action::PitchUp))     { state.pitch[v] = static_cast<std::uint16_t>(std::min<int>(state.pitch[v] + kPitchStep, kPitchMax)); moved = true; }
        if (in.justPressed(Action::PitchDown))   { state.pitch[v] = static_cast<std::uint16_t>(std::max<int>(state.pitch[v] - kPitchStep, kPitchStep)); moved = true; }
        if (in.justPressed(Action::VolumeUp))    { state.volume[v] = static_cast<std::uint8_t>(std::min<int>(state.volume[v] + kVolumeStep, kVolumeMax)); moved = true; }
        if (in.justPressed(Action::VolumeDown))  { state.volume[v] = static_cast<std::uint8_t>(std::max<int>(state.volume[v] - kVolumeStep, 0)); moved = true; }
        if (in.justPressed(Action::PanLeft))     { state.pan[v] = std::max(state.pan[v] - 1, -8); moved = true; }
        if (in.justPressed(Action::PanRight))    { state.pan[v] = std::min(state.pan[v] + 1, 8); moved = true; }
        if (moved) changed(state.selected);
        if (in.justPressed(Action::NextSample))  nextSample(state.selected);
        if (in.justPressed(Action::NextMode))    nextMode(state.selected);
        if (in.justPressed(Action::TempoDown))   tempo(state.selected, -kTempoStep);
        if (in.justPressed(Action::TempoUp))     tempo(state.selected, kTempoStep);
        if (in.justPressed(Action::Send))        toggleSend(state.selected);
        if (in.justPressed(Action::PathSwitch))  switchPath();
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
        "SNES sampler: audio files registered on the library, played through the console's sound chip with\n"
        "play(id, {.voice, .effect}). 1-8 strike a voice (or click its pad), again to stop it; SPACE stops\n"
        "all; LEFT/RIGHT select; UP/DOWN pitch; -/= volume; [/] pan; E sends the voice into the effect\n"
        "path; R switches the path between an echo and a reverb — the send follows the voice as it plays;\n"
        "TAB the voice's sample and M its mode (continuous, once, repeat at 9/0's tempo), taken at\n"
        "its next strike; drag the bars with the mouse; F fullscreen; close to quit. LONG is a six-second\n"
        "file, longer than the sound chip's RAM, streamed through the voice as it plays.\n\n");

    WindowedHost host{loop, platform};
    host.run();
    return 0;
}
