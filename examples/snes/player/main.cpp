// SNES player — one cartridge on two machines, on the two clocks a machine can run on.
//
// Run it plainly and it asks for a SNES ROM, then plays it twice at once, side by side, scaled into one
// window (choose nothing and it runs the built-in demo cartridge from examples/snes/cartridge):
//
//   LEFT   advanced by the engine's tick — one tick, one of its frames, on the game's own thread.
//   RIGHT  free-running on a clock of its own, at the hardware's cadence, on a thread of its own.
//
// Both take the same buttons, so the two 32x32 sprites move together — until you play them, where each
// takes a press at ITS OWN next step boundary and the two drift into two playthroughs. The clocks stay in
// step (the same number of frames to the frame over a minute); the games do not. Fuse the two halves
// stereoscopically and a one-frame divergence breaks the fusion at once.
//
// Both machines make sound; press 4 to choose what is heard — nothing, the LEFT machine, the RIGHT
// machine, or both at once with the left machine in the left speaker and the right machine in the
// right, and round again. Each machine's DSP frames, converted to the device's rate, go into a queue
// of its own on the thread that steps it; the device drains the queues it is listening to and drops
// the rest. A scope draws what the device took from each machine — the left machine in amber, the right
// in cyan — and only a machine being heard moves its trace. Press 5 to lay the scopes out one under each
// screen, or as one scope across both with the two traces over each other, where two machines making the
// same sound show as one line. Press 3 to switch the device between 48'000 Hz and 44'100 Hz: the pitch
// stays where it is, because the conversion follows the rate.
//
// Each machine's picture lands in a slot the size of the console's largest picture, 512×448, through the
// raster's `fit`: a 256×224 frame doubles each pixel, a 512-wide one lands one to one, and an interlaced
// run's two fields, woven, fill the height. Press 6 to cycle how an interlaced picture is shown — each
// field as it comes, woven straight, woven blended — on both machines at once. The demo cartridge's Y
// switches it to the half-pixel screen mode and its X switches the chip to interlace, so all four sizes
// are a key away.
//
// Reading the picture is those four lines, the fit being the one the Game Boy player does not need:
//
//     Vm machine{VMPlatform::Snes, VmConfig{.key = "…", .video = true}};
//     machine.run(Vm::Advance::OnTick);       // one engine tick advances it by one of its frames
//     RasterContent picture = machine.video(); // the last complete frame it drew, at whatever size
//     picture.fit = PixelSize{512, 448};       // ...shown in one slot
//
// PORT TWO is the player's to plug: press 2 to plug or unplug a second controller. The player owns this
// — a program knows which sockets it filled — and prints the port's state; the demo cartridge is a
// one-pad game and does not read port two.
//
// Modes:
//   (no args)                          pick a ROM through a file dialog and play it in a window
//                                      (cancel to run the built-in demo cartridge)
//   <seconds> [out.csv] [rom]          capture: run headless-timed and write one row per drawn frame
//                                      (the demo cartridge when no ROM is named, so it runs unattended)
//   --verify                           headless: assert each clock holds the hardware's cadence, the
//                                      cartridge's sound reaches a sink, and the picture takes each size
//                                      the demo cartridge draws; exit nonzero on any miss (CI runs this
//                                      on every platform)
//
// Bring your own cartridge: pass its path as the third argument; it is read from disk and nothing more.
// A dev drives the window.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include "retropp/audio.h"
#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/raster_content.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/snes.h"
#include "retropp/vm.h"
#include "retropp/windowed_host.h"

#include "examples/snes/cartridge/cartridge.h"

namespace {

using namespace retropp;

constexpr int kGuestW = 512, kGuestH = 448;             // one SNES screen: the console's largest picture
constexpr int kScopeH = 48;                             // the band under them, showing the sound
constexpr int kViewW = kGuestW * 2, kViewH = kGuestH + kScopeH;   // two screens side by side, a scope under each
constexpr int kScale = 2;                               // 1024×496 × 2 = a 2048×992 window
constexpr std::size_t kScopeFrames = kViewW;            // one frame per column of the whole band

// A player-level action, numbered clear of the twelve pad buttons (snes::Button is 0-11): the key that
// plugs and unplugs the second controller, the key that switches the device's rate, the key that
// cycles which machine is heard, the key that switches how the scopes are laid out, and the key that
// cycles how an interlaced picture is shown.
enum class Player : ActionId { TogglePort2 = 20, ToggleRate = 21, CycleHeard = 22, ToggleScope = 23, CycleWeave = 24 };

// ── Sound ───────────────────────────────────────────────────────────────────────────────────────
// A machine's sound reaches the device through a queue of its own: the thread that steps the machine
// pushes each frame as it is produced and SDL's audio thread pulls what it needs. One writer, one
// reader per queue, so two atomics are the whole synchronization. A full queue drops the frame: the
// machine is paced by its own clock, not by the device, so a speed factor above one makes more than
// the device takes and one below leaves it short.
class FrameQueue {
public:
    static constexpr std::size_t kCapacity = 8192;   // about 170 ms at 48'000 Hz

    bool push(AudioFrame frame) {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        if (w - r == kCapacity) {
            return false;
        }
        frames_[w % kCapacity] = frame;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    std::size_t pop(std::span<AudioFrame> out) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t n = std::min(out.size(), w - r);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = frames_[(r + i) % kCapacity];
        }
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    [[nodiscard]] std::size_t size() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    // Only while neither side is running.
    void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::array<AudioFrame, kCapacity> frames_{};
    std::atomic<std::size_t>          write_{0};
    std::atomic<std::size_t>          read_{0};
};

// Which machine is heard: nothing, the left (tick-advanced) machine, the right (free-running) one, or
// both at once — the left machine in the left speaker, the right machine in the right. The 4 key
// cycles it; the pull reads it.
enum class Heard : int { None = 0, Left = 1, Right = 2, Both = 3 };

const char* heardName(Heard heard) {
    switch (heard) {
        case Heard::None:  return "nothing";
        case Heard::Left:  return "the left machine";
        case Heard::Right: return "the right machine";
        case Heard::Both:  return "both — left machine in the left speaker, right machine in the right";
    }
    return "";
}

// The last kScopeFrames frames the device took from one machine, written by the pull on the audio
// thread and read by the render loop, under one lock taken once per pull and once per drawn frame. The
// pull writes only while that machine is heard, so a machine nobody hears leaves its scope still.
struct Scope {
    mutable std::mutex                    lock;
    std::array<AudioFrame, kScopeFrames> ring{};
    std::size_t                           head = 0;   // where the next frame goes

    void record(std::span<const AudioFrame> frames) {
        const std::lock_guard guard{lock};
        for (const AudioFrame& frame : frames) {
            ring[head] = frame;
            head       = (head + 1) % kScopeFrames;
        }
    }
};

// How the band shows the two machines' sound: a scope under each screen, or one scope across the whole
// band with both machines drawn over each other — where two traces that coincide show as one. The 5 key
// switches between them.
enum class ScopeView : std::uint8_t { Split, Overlaid };

// Each machine's color, in either view: the left machine amber, the right machine cyan.
struct TraceColor {
    std::uint8_t r, g, b;
};
constexpr TraceColor kLeftColor{.r = 255, .g = 200, .b = 64};
constexpr TraceColor kRightColor{.r = 64, .g = 200, .b = 255};

// How an interlaced picture is shown, on both machines: each field as it comes, or woven. The 6 key
// cycles it; the options each step hands to video() are the whole of the change.
enum class Shown : std::uint8_t { Fields, Straight, Blend };

const char* shownName(Shown shown) {
    switch (shown) {
        case Shown::Fields:   return "each field as it comes";
        case Shown::Straight: return "woven straight";
        case Shown::Blend:    return "woven, each line blended with the one below";
    }
    return "";
}

VideoOptions optionsFor(Shown shown) {
    switch (shown) {
        case Shown::Fields:   return {.interlacing = {.on = false}};
        case Shown::Straight: return {.interlacing = {.on = true, .type = Weave::Straight}};
        case Shown::Blend:    return {.interlacing = {.on = true, .type = Weave::Blend}};
    }
    return {};
}

// Paint the whole band its dark background.
void clearBand(std::vector<std::uint8_t>& pixels) {
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 16;  pixels[i + 1] = 16;  pixels[i + 2] = 24;  pixels[i + 3] = 255;
    }
}

// Draw one machine's sound as a trace in `color`, `width` columns wide from column `x0`: one frame a
// column, the newest `width` frames the device took from it, its two channels averaged.
void drawTrace(const Scope& scope, std::vector<std::uint8_t>& pixels, int x0, int width, TraceColor color) {
    std::array<AudioFrame, kScopeFrames> frames{};
    std::size_t                          head = 0;
    {
        const std::lock_guard guard{scope.lock};
        frames = scope.ring;
        head   = scope.head;
    }
    const std::size_t oldest   = head + kScopeFrames - static_cast<std::size_t>(width);
    const int         mid      = kScopeH / 2;
    int               previous = mid;
    for (int x = 0; x < width; ++x) {
        const AudioFrame& frame  = frames[(oldest + static_cast<std::size_t>(x)) % kScopeFrames];
        const int         sample = (static_cast<int>(frame.left) + static_cast<int>(frame.right)) / 2;
        const int         y      = std::clamp(mid - sample * (mid - 1) / 32768, 0, kScopeH - 1);
        for (int yy = std::min(previous, y); yy <= std::max(previous, y); ++yy) {
            const std::size_t p = (static_cast<std::size_t>(yy) * kViewW + static_cast<std::size_t>(x0 + x)) * 4;
            pixels[p] = color.r;  pixels[p + 1] = color.g;  pixels[p + 2] = color.b;  pixels[p + 3] = 255;
        }
        previous = y;
    }
}

// Paint the band in `view`: each machine under its own screen, or both across the band with the left
// machine drawn last, so where the two coincide the cyan is hidden under the amber.
void drawScopes(const Scope& left, const Scope& right, ScopeView view, std::vector<std::uint8_t>& pixels) {
    clearBand(pixels);
    if (view == ScopeView::Split) {
        drawTrace(left, pixels, 0, kGuestW, kLeftColor);
        drawTrace(right, pixels, kGuestW, kGuestW, kRightColor);
    } else {
        drawTrace(right, pixels, 0, kViewW, kRightColor);
        drawTrace(left, pixels, 0, kViewW, kLeftColor);
    }
}

// ── Choosing a ROM ──────────────────────────────────────────────────────────────────────────────
// How the dialog ended: cancelling is an answer; failing is the dialog never opening.
enum class DialogEnd : std::uint8_t { Chosen, Cancelled, Failed };

// What the dialog callback hands back to the waiting main thread. SDL may invoke the callback on another
// thread, so the path is written under the mutex and `finished` is the release/acquire handshake.
struct DialogOutcome {
    std::mutex        lock;
    std::string       path;
    DialogEnd         end = DialogEnd::Cancelled;
    std::string       error;
    std::atomic<bool> finished{false};
};

void onFileChosen(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* outcome = static_cast<DialogOutcome*>(userdata);
    {
        const std::lock_guard guard{outcome->lock};
        if (filelist == nullptr) {
            outcome->end    = DialogEnd::Failed;
            const char* why = SDL_GetError();
            outcome->error  = (why != nullptr) ? why : "";
        } else if (filelist[0] == nullptr) {
            outcome->end = DialogEnd::Cancelled;
        } else {
            outcome->end  = DialogEnd::Chosen;
            outcome->path = filelist[0];
        }
    }
    outcome->finished.store(true, std::memory_order_release);
}

// Ask for a cartridge before anything else is stood up. The dialog is a platform window, so the video
// subsystem has to be up; SDL refcounts initialization, and it is deliberately not torn down here (the
// panel's completion handler is still unwinding when the callback returns).
DialogEnd askForRom(std::filesystem::path& chosen) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::printf("Could not start SDL video, so no file dialog can be shown: %s\n", SDL_GetError());
        return DialogEnd::Failed;
    }
    const SDL_DialogFileFilter filters[]{
        {"SNES ROM", "sfc;smc"},
        {"All files", "*"},
    };
    DialogOutcome          outcome;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(
        props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
        const_cast<SDL_DialogFileFilter*>(static_cast<const SDL_DialogFileFilter*>(filters)));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                          static_cast<Sint64>(std::size(filters)));
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose a SNES ROM to run");
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, onFileChosen, &outcome, props);

    // Block until the callback fires, pumping events — the portal dialogs on Linux run over DBus and
    // never complete without it.
    while (!outcome.finished.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    SDL_DestroyProperties(props);

    const std::lock_guard guard{outcome.lock};
    if (outcome.end == DialogEnd::Failed) {
        std::printf("The file dialog could not be opened: %s\n",
                    outcome.error.empty() ? "no reason given" : outcome.error.c_str());
    }
    if (outcome.end == DialogEnd::Chosen) {
        chosen = std::filesystem::path{outcome.path};
    }
    return outcome.end;
}

// ── Capture mode ────────────────────────────────────────────────────────────────────────────────
// `retropp-snes_player-demo <seconds> [out.csv] [rom]` runs for a fixed time and writes one row per drawn
// frame — the same measurement, and the same columns, the Game Boy player writes: each machine's
// generation and its delta since the previous row, whether the frame reached the screen, and the panel's
// refresh (which decides what a correct delta histogram looks like).
struct CaptureRow {
    std::uint64_t frame      = 0;
    std::uint64_t tNs        = 0;   // since the first captured frame
    std::uint64_t tickedGen  = 0;
    std::uint64_t freeGen    = 0;
    std::uint64_t tickedStep = 0;   // generation delta since the previous row
    std::uint64_t freeStep   = 0;
    int           presented  = 0;   // whether this frame reached the screen
    float         displayHz  = 0.0f;
};

// Written once at the end, so nothing touches the disk during the run. Every frame is recorded — no
// threshold, no filtering, because the interesting frame is the one a filter would drop.
void writeCapture(const std::string& path, const std::vector<CaptureRow>& rows) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "snes_player: cannot open %s for writing\n", path.c_str());
        return;
    }
    std::fprintf(out, "frame,t_ns,ticked_gen,free_gen,ticked_step,free_step,presented,display_hz\n");
    for (const CaptureRow& r : rows) {
        std::fprintf(out, "%llu,%llu,%llu,%llu,%llu,%llu,%d,%.3f\n",
                     static_cast<unsigned long long>(r.frame),
                     static_cast<unsigned long long>(r.tNs),
                     static_cast<unsigned long long>(r.tickedGen),
                     static_cast<unsigned long long>(r.freeGen),
                     static_cast<unsigned long long>(r.tickedStep),
                     static_cast<unsigned long long>(r.freeStep),
                     r.presented, static_cast<double>(r.displayHz));
    }
    std::fclose(out);
    std::fprintf(stderr, "snes_player: wrote %zu rows to %s\n", rows.size(), path.c_str());
}

std::vector<std::uint8_t> readRom(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

// What to call the file an image's save is kept in: one path component, the characters that would make it
// more than one replaced. Empty when the name yields nothing usable, and the caller then names nothing.
std::string keptAs(const std::filesystem::path& file) {
    std::string name;
    for (const char c : file.stem().string()) {
        const bool plain = (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-' ||
                           c == '_' || c == ' ' || c == '.';
        name += plain ? c : '-';
    }
    return (name == "." || name == "..") ? std::string{} : name;
}

// ── Verify mode (the cadence gate) ──────────────────────────────────────────────────────────────
// Headless: no window, no GPU, no audio. One free-running machine holds the hardware's cadence, and the
// count of frames it finishes per wall second is measured from video().generation — the core keeps no
// named place to read, so the picture's own generation is the clock. Each moving phase must land within
// ±5% of its factor's target; the paused phase must not move.
void watchRate(Vm& machine, const char* label, double targetHz, int& failures) {
    const std::uint64_t start = machine.video().generation;
    const auto          t0    = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::uint64_t delta    = machine.video().generation - start;
    const double        measured = static_cast<double>(delta) / elapsed;
    const auto [num, den]        = machine.speed();
    std::printf("  %-12s {%u,%u}  %7.2f frames/s over %.2f s (target %7.2f)\n", label, num, den,
                measured, elapsed, targetHz);
    const bool held = targetHz == 0.0 ? delta == 0
                                      : std::fabs(measured - targetHz) <= targetHz * 0.05;
    if (!held) {
        std::printf("  VERIFY FAILED: %s measured %.2f against target %.2f\n", label, measured,
                    targetHz);
        ++failures;
    }
}

// The four factors on one region's clock: hardware speed, doubled, halved, paused.
void watchAllFactors(snaggletooth::Region region, const char* tag, double baseHz, int& failures) {
    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge(region));
    machine.video(true);
    machine.run(Vm::Advance::Continuously);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // reach steady state before measuring

    std::string label = std::string(tag) + " 1:1";
    watchRate(machine, label.c_str(), baseHz, failures);
    machine.speed(2, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    label = std::string(tag) + " 2:1";
    watchRate(machine, label.c_str(), baseHz * 2.0, failures);
    machine.speed(1, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    label = std::string(tag) + " 1:2";
    watchRate(machine, label.c_str(), baseHz / 2.0, failures);
    machine.speed(0, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the pause settles one step later
    label = std::string(tag) + " paused";
    watchRate(machine, label.c_str(), 0.0, failures);
    machine.stop();
}

int runVerify() {
    int failures = 0;
    std::printf("snes_player --verify: the SNES core holds each clock's cadence, its sound reaches a sink, and its picture takes each size\n\n");

    // The console's own rates: 236'250'000/11 Hz over 357'366 cycles a frame is 60.0988; PAL is
    // 21'281'370 Hz over 425'568 is 50.0070.
    watchAllFactors(snaggletooth::Region::Ntsc, "ntsc", 60.0988, failures);
    watchAllFactors(snaggletooth::Region::Pal, "pal", 50.0070, failures);

    // The other clock: the same cartridge advanced one tick at a time. One tick is one SNES frame, so a
    // fixed number of ticks moves the generation by that many (± the frame straddling the boundary).
    Vm::SNES unity{VmConfig{.video = true}};
    unity.hostRom(examples::snes::demoCartridge(snaggletooth::Region::Ntsc));
    unity.video(true);
    unity.run(Vm::Advance::OnTick);
    for (int tick = 0; tick < 600; ++tick) {
        unity.advanceTick();
    }
    const std::uint64_t lived = unity.video().generation;
    unity.stop();
    std::printf("\n  on the tick clock: 600 ticks finished %llu frames (one tick, one frame)\n",
                static_cast<unsigned long long>(lived));
    if (lived + 1 < 600 || lived > 601) {
        std::printf("  VERIFY FAILED: 600 ticks finished %llu frames, not 600 (±1)\n",
                    static_cast<unsigned long long>(lived));
        ++failures;
    }

    // The picture takes the size the cartridge's program has the chip draw. A press of Y switches the
    // demo to mode 5 and the frame widens to 512; a press of X sets the interlace bits and the machine
    // hands over fields, which weaving makes a 448-line picture and which, unwoven, arrive 224 tall. A
    // press is held two ticks so the auto-read sees it, and three more ticks carry the switch to a
    // delivered frame.
    Vm::SNES sized{VmConfig{.video = true}};
    sized.hostRom(examples::snes::demoCartridge(snaggletooth::Region::Ntsc));
    sized.run(Vm::Advance::OnTick);
    const auto ticks = [&sized](int n) {
        for (int i = 0; i < n; ++i) {
            sized.advanceTick();
        }
    };
    const auto press = [&](snes::Buttons held) {
        sized.buttons(snes::Ports{.one = held, .two = std::nullopt});
        ticks(2);
        sized.buttons(snes::Ports{.one = snes::Buttons{}, .two = std::nullopt});
        ticks(3);
    };
    ticks(3);
    const RasterContent plain = sized.video();
    press(snes::Buttons{.y = true});
    const RasterContent wide = sized.video();
    sized.video(true, {.interlacing = {.on = true}});
    press(snes::Buttons{.x = true});
    const RasterContent woven = sized.video();
    sized.video(true, {.interlacing = {.on = false}});
    ticks(3);
    const RasterContent fields = sized.video();
    sized.stop();
    std::printf("\n  the picture: %dx%d, then %dx%d in mode 5, %dx%d interlaced and woven, %dx%d as fields\n",
                plain.width, plain.height, wide.width, wide.height, woven.width, woven.height,
                fields.width, fields.height);
    if (plain.width != 256 || plain.height != 224 || wide.width != 512 || wide.height != 224 ||
        woven.width != 512 || woven.height != 448 || fields.width != 512 || fields.height != 224) {
        std::printf("  VERIFY FAILED: expected 256x224, 512x224, 512x448 and 512x224\n");
        ++failures;
    }

    // The cartridge's sound reaches a sink, headless: the frames one tick-advanced machine hands over at
    // 48'000 Hz over 120 ticks, and how loud they get. Each SNES frame is 532 or 533 of the chip's
    // frames, so 120 of them are 63'840..63'960, which the conversion makes 95'760..95'940; the
    // demo's first note is playing well inside the first frame and swings past ±10'000.
    Vm::SNES      sounding;
    std::uint64_t frames = 0;
    int           peak   = 0;
    sounding.hostRom(examples::snes::demoCartridge(snaggletooth::Region::Ntsc));
    sounding.enableAudio(48'000, [&frames, &peak](std::int16_t left, std::int16_t right) {
        ++frames;
        peak = std::max({peak, std::abs(static_cast<int>(left)), std::abs(static_cast<int>(right))});
    });
    sounding.run(Vm::Advance::OnTick);
    for (int tick = 0; tick < 120; ++tick) {
        sounding.advanceTick();
    }
    sounding.stop();
    std::printf("\n  sound: 120 ticks handed %llu frames at 48'000 Hz, peak %d\n",
                static_cast<unsigned long long>(frames), peak);
    if (frames < 95'760 || frames > 95'940 || peak < 5'000) {
        std::printf("  VERIFY FAILED: expected 95'760..95'940 frames and a peak of at least 5'000\n");
        ++failures;
    }

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    // Capture mode: `snes_player <seconds> [out.csv] [rom]`. Without arguments the player plays the demo
    // cartridge in a window. A third argument names a cartridge to host instead of the demo.
    const double      capSeconds = (argc > 1) ? std::strtod(argv[1], nullptr) : 0.0;
    const std::string capPath    = (argc > 2) ? argv[2] : "snes_player_capture.csv";
    const std::string romArg     = (argc > 3) ? argv[3] : "";
    const bool        capturing  = capSeconds > 0.0;

    // The image: a file named on the command line, one chosen through the picker, or the built-in demo
    // cartridge. Capture runs unattended, so with no ROM named it uses the demo rather than blocking on a
    // dialog; the interactive player asks for a ROM, and falls back to the demo if none is chosen.
    std::vector<std::uint8_t> rom;
    std::string               saveFor;
    if (!romArg.empty()) {
        const std::filesystem::path file{romArg};
        rom = readRom(file);
        if (rom.empty()) {
            std::printf("%s could not be read, or holds no bytes.\n", file.string().c_str());
            return 1;
        }
        saveFor = keptAs(file);
    } else if (capturing) {
        rom = examples::snes::demoCartridge(snaggletooth::Region::Ntsc);
    } else {
        std::filesystem::path file;
        switch (askForRom(file)) {
            case DialogEnd::Chosen:
                rom = readRom(file);
                if (rom.empty()) {
                    std::printf("%s could not be read, or holds no bytes.\n", file.string().c_str());
                    return 1;
                }
                saveFor = keptAs(file);
                break;
            case DialogEnd::Cancelled:
                std::printf("No ROM chosen — running the built-in demo cartridge.\n");
                rom = examples::snes::demoCartridge(snaggletooth::Region::Ntsc);
                break;
            case DialogEnd::Failed:
                rom = examples::snes::demoCartridge(snaggletooth::Region::Ntsc);
                break;
        }
    }

    const EngineConfig config{
        .identity     = {.organization = "Retro++", .application = "SnesPlayer"},
        .window       = {.title = "Polyrhythm — SNES player (tick-advanced | free-running)"},
        .viewport     = ViewportResolution{kViewW, kViewH},
        .timing       = TimingProfile::Snes,  // The run loop needs the SNES timing profile to tick-advance correctly
        .enhancements = {.windowScale = kScale}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};
    SdlAudioSink sink;  // The device: an SDL stream this program drains its own queue into.

    // The same image on two machines: the only difference between the two screens is the clock each runs
    // on. Both declare video at construction (off until asked) and are named apart, so each keeps its own
    // guest's save the way two cartridges in two consoles do not share a battery.
    Vm ticked{VMPlatform::Snes, VmConfig{.key = "snes-ticked", .video = true}};
    Vm freeRunning{VMPlatform::Snes, VmConfig{.key = "snes-free-running", .video = true}};
    for (Vm* machine : {&ticked, &freeRunning}) {
        if (!saveFor.empty()) {
            machine->batterySave(saveFor);
        }
        machine->hostRom(std::span<const std::uint8_t>(rom));
        machine->video(true);
    }
    Shown shown = Shown::Straight;   // the 6 key writes it; both machines take each change at their next step
    ticked.video(true, optionsFor(shown));
    freeRunning.video(true, optionsFor(shown));

    // The sound: each machine's frames go into its own queue from the thread that steps it — the game's
    // for the left machine, its own for the right. The device pulls on SDL's audio thread from the
    // queue or queues it is listening to, once each of those holds a twentieth of a second so the first
    // pull does not run it dry, and drains the rest so nothing stale waits to be switched in. What it
    // takes from each machine is what that machine's scope shows.
    FrameQueue tickedQueue;
    FrameQueue freeQueue;
    Scope      tickedScope;
    Scope      freeScope;
    unsigned   deviceRate = kAudioSampleRate;
    std::atomic<int> heard{static_cast<int>(Heard::None)};   // the 4 key writes it; the pull reads it
    std::vector<AudioFrame> scratchLeft(FrameQueue::kCapacity);    // where the pull drains what it does
    std::vector<AudioFrame> scratchRight(FrameQueue::kCapacity);   // not hand to the device
    bool  primed    = false;         // the pull's own; reset when what is heard changes
    Heard lastHeard = Heard::None;   // the pull's own; how it notices a change
    auto tickedSample = [&tickedQueue](std::int16_t left, std::int16_t right) {
        tickedQueue.push(AudioFrame{.left = left, .right = right});
    };
    auto freeSample = [&freeQueue](std::int16_t left, std::int16_t right) {
        freeQueue.push(AudioFrame{.left = left, .right = right});
    };
    auto pull = [&](std::span<AudioFrame> out) -> std::size_t {
        const Heard now = static_cast<Heard>(heard.load(std::memory_order_acquire));
        if (now != lastHeard) {
            lastHeard = now;
            primed    = false;
        }
        const std::size_t fill       = deviceRate / 20;
        const bool        wantsLeft  = now == Heard::Left || now == Heard::Both;
        const bool        wantsRight = now == Heard::Right || now == Heard::Both;
        if (!primed) {
            if ((wantsLeft && tickedQueue.size() < fill) || (wantsRight && freeQueue.size() < fill)) {
                return 0;
            }
            primed = true;
        }
        std::size_t got = 0;
        switch (now) {
            case Heard::None:
                tickedQueue.pop(std::span{scratchLeft});
                freeQueue.pop(std::span{scratchRight});
                return 0;
            case Heard::Left:
                got = tickedQueue.pop(out);
                freeQueue.pop(std::span{scratchRight});
                tickedScope.record(out.first(got));
                break;
            case Heard::Right:
                got = freeQueue.pop(out);
                tickedQueue.pop(std::span{scratchLeft});
                freeScope.record(out.first(got));
                break;
            case Heard::Both: {
                // One frame from each machine makes one frame for the device: the left machine's left
                // channel and the right machine's right. Only as many as both queues hold; the rest of
                // the fuller queue waits for the next pull. Each scope records the one channel the
                // device took from its machine.
                const std::size_t n = std::min({out.size(), tickedQueue.size(), freeQueue.size()});
                tickedQueue.pop(std::span{scratchLeft}.first(n));
                freeQueue.pop(std::span{scratchRight}.first(n));
                for (std::size_t i = 0; i < n; ++i) {
                    out[i]          = AudioFrame{.left = scratchLeft[i].left, .right = scratchRight[i].right};
                    scratchLeft[i]  = AudioFrame{.left = out[i].left, .right = out[i].left};
                    scratchRight[i] = AudioFrame{.left = out[i].right, .right = out[i].right};
                }
                tickedScope.record(std::span{scratchLeft}.first(n));
                freeScope.record(std::span{scratchRight}.first(n));
                got = n;
                break;
            }
        }
        return got;
    };
    ticked.enableAudio(deviceRate, tickedSample);
    freeRunning.enableAudio(deviceRate, freeSample);
    sink.start(deviceRate, kAudioChannels, pull);

    // The SNES pad, bound to keys and a gamepad; editing these rows is all rebinding is. The number keys
    // are the player's own: the 2 key plugs and unplugs the second controller, the 3 key switches the
    // device's rate, the 4 key cycles which machine is heard, the 5 key switches how the scopes are laid out,
    // the 6 key cycles how an interlaced picture is shown.
    ActionMap controls{
        {snes::Button::A,      {SDL_SCANCODE_X, PadButton::FaceLabelA}},
        {snes::Button::B,      {SDL_SCANCODE_Z, PadButton::FaceLabelB}},
        {snes::Button::X,      {SDL_SCANCODE_S, PadButton::FaceLabelX}},
        {snes::Button::Y,      {SDL_SCANCODE_A, PadButton::FaceLabelY}},
        {snes::Button::L,      {SDL_SCANCODE_Q, PadButton::ShoulderL}},
        {snes::Button::R,      {SDL_SCANCODE_W, PadButton::ShoulderR}},
        {snes::Button::Select, {SDL_SCANCODE_RSHIFT, PadButton::Select}},
        {snes::Button::Start,  {SDL_SCANCODE_RETURN, PadButton::Start}},
        {Player::TogglePort2,  {SDL_SCANCODE_2}},
        {Player::ToggleRate,   {SDL_SCANCODE_3}},
        {Player::CycleHeard,   {SDL_SCANCODE_4}},
        {Player::ToggleScope,  {SDL_SCANCODE_5}},
        {Player::CycleWeave,   {SDL_SCANCODE_6}},
    };
    controls.add(presets::directional(snes::Button::Up, snes::Button::Down, snes::Button::Left,
                                      snes::Button::Right));
    platform.actions(controls);

    ticked.run(Vm::Advance::OnTick);  // the engine's tick is its clock: one tick, one of its frames

    // The two clocks start at the same instant only if made to: the free-running machine begins consuming
    // wall time the moment run() returns, while the tick-advanced one waits for its first tick. So the
    // free one is started from inside that first tick, the one moment they share.
    bool freeRunningStarted = false;
    bool port2Plugged       = false;
    ScopeView scopeView     = ScopeView::Split;   // the 5 key writes it; the render loop reads it, on this thread
    loop.simTick([&](const InputState& input) {
        if (!freeRunningStarted) {
            freeRunning.run(Vm::Advance::Continuously);
            freeRunningStarted = true;
        }
        if (input.justPressed(Player::TogglePort2)) {
            port2Plugged = !port2Plugged;
            std::printf("port 2: %s\n", port2Plugged ? "controller plugged in" : "unplugged");
        }
        if (input.justPressed(Player::CycleHeard)) {
            // Nothing, the left machine, the right machine, both, and round again. The pull notices on
            // its next call and primes again from the queue or queues it now listens to.
            const Heard next = static_cast<Heard>((heard.load(std::memory_order_relaxed) + 1) % 4);
            heard.store(static_cast<int>(next), std::memory_order_release);
            std::printf("heard: %s\n", heardName(next));
        }
        if (input.justPressed(Player::ToggleScope)) {
            scopeView = (scopeView == ScopeView::Split) ? ScopeView::Overlaid : ScopeView::Split;
            std::printf("scopes: %s\n", scopeView == ScopeView::Split ? "one under each screen"
                                                                      : "one across both, traces overlaid");
        }
        if (input.justPressed(Player::CycleWeave)) {
            shown = static_cast<Shown>((static_cast<int>(shown) + 1) % 3);
            ticked.video(true, optionsFor(shown));
            freeRunning.video(true, optionsFor(shown));
            std::printf("interlaced picture: %s\n", shownName(shown));
        }
        if (input.justPressed(Player::ToggleRate)) {
            // Both machines park, their sound is pointed at the new rate, the device reopens at it, and
            // both run again. The queues start over so nothing made at the old rate plays at the new.
            deviceRate = (deviceRate == 48'000u) ? 44'100u : 48'000u;
            ticked.stop();
            freeRunning.stop();
            sink.stop();
            tickedQueue.clear();
            freeQueue.clear();
            primed = false;
            ticked.enableAudio(deviceRate, tickedSample);
            freeRunning.enableAudio(deviceRate, freeSample);
            sink.start(deviceRate, kAudioChannels, pull);
            ticked.run(Vm::Advance::OnTick);
            freeRunning.run(Vm::Advance::Continuously);
            std::printf("device: %u Hz — the pitch stays put\n", deviceRate);
        }
        // Port one is the pad; port two is a controller the player plugs, or an empty socket. Both
        // machines are handed the same two-port word — the left sees it at this tick, the right at its
        // own next step.
        const snes::Ports ports{
            .one = snes::held(input),
            .two = port2Plugged ? std::optional<snes::Buttons>{snes::held(input)} : std::nullopt};
        ticked.buttons(ports);
        freeRunning.buttons(ports);

        ticked.advanceTick();
    });

    float displayHz = 0.0f;
    if (const SDL_DisplayMode* mode =
            SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(platform.sdlWindow()))) {
        displayHz = mode->refresh_rate;
    }

    std::vector<CaptureRow> capRows;
    if (capturing) {
        capRows.reserve(static_cast<std::size_t>(capSeconds * 130.0) + 64);
    }
    const auto    capStart   = std::chrono::steady_clock::now();
    std::uint64_t capFrame   = 0;
    std::uint64_t lastTicked = 0;
    std::uint64_t lastFree   = 0;

    std::vector<std::uint8_t> scopePixels(static_cast<std::size_t>(kViewW) * kScopeH * 4);
    std::uint64_t             scopeGeneration = 0;
    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        // Each machine's picture lands in a slot one screen wide — the console's largest picture, so a
        // frame of any size fits it — and the layer spans both, so the right-hand one is placed by
        // scrolling its content half a viewport to the left. Outside its fit a picture draws nothing,
        // which keeps the two halves from overlapping.
        RasterContent tickedPicture = ticked.video();
        RasterContent freePicture   = freeRunning.video();
        tickedPicture.fit = PixelSize{kGuestW, kGuestH};
        freePicture.fit   = PixelSize{kGuestW, kGuestH};

        DrawLayer left{.key = "tick-advanced"};
        left.z       = 0;
        left.size    = PixelSize{kViewW, kViewH};
        left.content = tickedPicture;
        frame.layers.push_back(left);

        DrawLayer right{.key = "free-running"};
        right.z       = 1;
        right.size    = PixelSize{kViewW, kViewH};
        right.scroll  = LayerScroll{-kGuestW, 0};
        right.content = freePicture;
        frame.layers.push_back(right);

        // The scope band, under the two screens, is this program's own raster: it paints the band on the
        // CPU into its own RGBA buffer (drawScopes) and submits that buffer through RasterContent, the
        // content type a hosted machine's video() hands back — a layer shows any raster that arrives in
        // that form, whoever drew it. The buffer changes every frame, so its generation is bumped every
        // frame and the engine uploads it anew each time; a raster that held still would keep its
        // generation and upload once.
        drawScopes(tickedScope, freeScope, scopeView, scopePixels);
        ++scopeGeneration;
        DrawLayer band{.key = "scope"};
        band.z       = 2;
        band.size    = PixelSize{kViewW, kViewH};
        band.scroll  = LayerScroll{0, -kGuestH};
        band.content = RasterContent{.pixels     = scopePixels,
                                     .width      = kViewW,
                                     .height     = kScopeH,
                                     .format     = RasterPixelFormat::Rgba8888,
                                     .generation = scopeGeneration};
        frame.layers.push_back(band);

        renderer.renderFrame(frame);

        if (capturing) {
            const auto now = std::chrono::steady_clock::now();
            capRows.push_back(CaptureRow{
                .frame      = capFrame,
                .tNs        = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - capStart).count()),
                .tickedGen  = tickedPicture.generation,
                .freeGen    = freePicture.generation,
                .tickedStep = tickedPicture.generation - lastTicked,
                .freeStep   = freePicture.generation - lastFree,
                .presented  = renderer.renderStats().lastFrame.presented ? 1 : 0,
                .displayHz  = displayHz,
            });
            lastTicked = tickedPicture.generation;
            lastFree   = freePicture.generation;
            ++capFrame;
            if (std::chrono::duration<double>(now - capStart).count() >= capSeconds) {
                loop.exitRequest();
            }
        }
    });

    std::printf(
        "Running the cartridge on two machines from one image.\n"
        "  LEFT  — advanced by the engine's tick: one tick, one of its frames.\n"
        "  RIGHT — free-running on a clock of its own, at the hardware's cadence.\n"
        "Both clocks run at the same nominal rate, so the two stay in step; a frame of wobble now and "
        "then is each side crossing a frame boundary at a slightly different moment.\n"
        "Play it: arrows or a d-pad move the sprite, X = A, Z = B, S = X, A = Y, Q/W = L/R, Enter = "
        "Start, Right Shift = Select; A and B recolor the sprite, Start writes the save; 2 plugs or "
        "unplugs a second controller. 4 cycles what is heard: nothing, the left machine, the right "
        "machine, both (left machine in the left speaker, right machine in the right), and round again "
        "— it starts silent. 5 lays the scopes out one under each screen or overlaid across both. "
        "6 cycles how an interlaced picture is shown: each field as it comes, woven straight, woven "
        "blended — it starts woven straight. Y switches the cartridge to the half-pixel screen mode and "
        "X switches its chip to interlace. "
        "3 switches the device between 48 kHz and 44.1 kHz — the pitch stays put. "
        "Both machines take the same buttons.\n"
        "Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    ticked.stop();
    freeRunning.stop();
    sink.stop();
    if (capturing) {
        writeCapture(capPath, capRows);
    }
    return 0;
}
