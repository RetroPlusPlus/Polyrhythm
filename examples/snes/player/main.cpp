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
// Reading the picture is those three lines, exactly as the Game Boy player's:
//
//     Vm machine{VMPlatform::Snes, VmConfig{.key = "…", .video = true}};
//     machine.run(Vm::Advance::OnTick);       // one engine tick advances it by one of its frames
//     screen.content = machine.video();       // the last complete frame it drew
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
//   --verify                           headless: assert each clock holds the hardware's cadence, exit
//                                      nonzero on any miss (CI runs this on every platform)
//
// Bring your own cartridge: pass its path as the third argument; it is read from disk and nothing more.
// A dev drives the window.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
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

constexpr int kGuestW = 256, kGuestH = 224;             // one SNES screen
constexpr int kViewW = kGuestW * 2, kViewH = kGuestH;   // two of them, side by side
constexpr int kScale = 4;                               // 512×224 × 4 = a 2048×896 window

// A player-level action, numbered clear of the twelve pad buttons (snes::Button is 0-11): the key that
// plugs and unplugs the second controller.
enum class Player : ActionId { TogglePort2 = 20 };

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
    std::printf("snes_player --verify: the SNES core holds each clock's cadence\n\n");

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

    // The SNES pad, bound to keys and a gamepad; editing these rows is all rebinding is. The 2 key is the
    // player's own — it plugs and unplugs the second controller.
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
    loop.simTick([&](const InputState& input) {
        if (!freeRunningStarted) {
            freeRunning.run(Vm::Advance::Continuously);
            freeRunningStarted = true;
        }
        if (input.justPressed(Player::TogglePort2)) {
            port2Plugged = !port2Plugged;
            std::printf("port 2: %s\n", port2Plugged ? "controller plugged in" : "unplugged");
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

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        // Each machine's picture is one screen wide and the layer spans both, so the right-hand one is
        // placed by scrolling its content half a viewport to the left. Outside its own dimensions a
        // picture draws nothing, which keeps the two halves from overlapping.
        const GuestFrameContent tickedPicture = ticked.video();
        const GuestFrameContent freePicture   = freeRunning.video();

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
        "Start, Right Shift = Select; A and B recolour the sprite, Start writes the save; 2 plugs or "
        "unplugs a second controller. Both machines take the same buttons.\n"
        "Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    ticked.stop();
    freeRunning.stop();
    if (capturing) {
        writeCapture(capPath, capRows);
    }
    return 0;
}
