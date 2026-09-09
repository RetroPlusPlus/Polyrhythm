// Game Boy player — one ROM on two machines, on the two clocks a machine can run on.
//
// Pick a ROM and watch it run twice at once, scaled 4× into a 1280×576 window. There is no HUD, no
// counter, no menu and no key: everything on screen is the machines' own pictures, side by side.
//
//   LEFT   advanced by the engine's tick — one tick, one of its frames, on the game's own thread.
//   RIGHT  free-running on a clock of its own, at the hardware's cadence, on a thread of its own.
//
// Both are given the same image and started at the same instant — which takes care, because a machine
// with a thread of its own starts the moment run() returns while a tick-advanced one waits for its first
// tick (see below). Both clocks then run at the same nominal rate — the engine's tick period for this
// machine IS its frame period, 70'224 cycles at 4'194'304 Hz — so the two stay in step, and what shows
// on screen is a frame of wobble now and then as each crosses a frame boundary at a slightly different
// moment. Reading the picture is IDENTICAL on both: `picture()` says which frame it is holding, never
// when it arrived, which is what lets one loop draw either.
//
// HOW TO READ THE TWO SCREENS PRECISELY: fuse them stereoscopically — cross or diverge your eyes until
// the two halves overlap into one image. Two identical pictures fuse into a single stable percept; a
// one-pixel offset or a single frame of divergence breaks the fusion into visible shimmer at once. It
// resolves differences that side-by-side comparison cannot, and it costs nothing to try.
//
// WHAT THIS DEMONSTRATES. A hosted machine's completed frames are a layer's content, so a machine's
// screen composites with native layers by z like tiles or sprites do:
//
//     machine.picture(true);                    // the machine draws (off by default — a raster costs cycles)
//     machine.run(Vm::Advance::OnTick);         // one engine tick advances it by one tick's worth
//     ...
//     screen.content = machine.picture();       // the last complete frame, held to the tick boundary
//
// The whole of the picture path is those three lines. Everything else here is choosing a file and
// standing up a window.
//
// ONE TICK IS ONE FRAME, exactly. The Game Boy tick period IS the machine's frame period by
// construction (70'224 cycles at 4'194'304 Hz), so this loop advances the machine by exactly one of
// its frames per tick, forever, with nothing to drift.
//
// WHAT THIS PLAYER REACHES IS THE PICTURE. It does not reach the machine's buttons or its sound, so a
// ROM boots, plays its logo and runs its attract mode, and cannot be played or heard. Each of those is
// its own surface on a hosted machine, and this player picks them up as they land.
//
// Bring your own ROM: the file you choose is read from disk at runtime, and read is all it ever is.
//
// A dev drives the window.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/vm.h"
#include "retropp/windowed_host.h"

namespace {

using namespace retropp;

constexpr int kGuestW = 160, kGuestH = 144;   // one machine's screen
constexpr int kViewW = kGuestW * 2, kViewH = kGuestH;  // two of them, side by side
constexpr int kScale = 4;                     // 320×144 × 4 = a 1280×576 window

// How the dialog ended. Cancelling and failing are different things: one is an answer, the other is
// the dialog never having opened, and only the second has an error worth printing.
enum class DialogEnd : std::uint8_t { Chosen, Cancelled, Failed };

// What the dialog callback hands back to the waiting main thread. SDL may invoke the callback on
// another thread, so the path is written under the mutex and `finished` is the release/acquire
// handshake the wait loop spins on.
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
            // The dialog itself failed. SDL_GetError is only meaningful here.
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

// Ask for the ROM before anything else is stood up, so the first thing on screen is the machine's
// picture rather than an empty window waiting for a file.
DialogEnd askForRom(std::filesystem::path& chosen) {
    // The dialog is a platform window, so the video subsystem has to be up. There is no window of our
    // own yet — the dialog is parentless, which every supported platform allows. SDL refcounts
    // subsystem initialization, so asking here is safe whether or not video is already up, and it is
    // deliberately NOT torn down afterwards: the panel's completion handler is still unwinding when
    // the callback returns, and quitting the subsystem out from under it hangs the process on macOS.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::printf("Could not start SDL video, so no file dialog can be shown: %s\n", SDL_GetError());
        return DialogEnd::Failed;
    }

    const SDL_DialogFileFilter filters[]{
        {"Game Boy ROM", "gb;gbc"},
        {"All files", "*"},
    };

    // The dialog says what it is FOR. The plain SDL_ShowOpenFileDialog carries no title, so the
    // properties form of the same dialog is used; a platform that cannot display a title shows its
    // stock chrome, which is the same dialog minus the words.
    DialogOutcome          outcome;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(
        props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
        const_cast<SDL_DialogFileFilter*>(static_cast<const SDL_DialogFileFilter*>(filters)));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                          static_cast<Sint64>(std::size(filters)));
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose a Game Boy ROM to run");
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, onFileChosen, &outcome, props);

    // Block until the callback fires. Pumping events is required, not merely polite: the portal-based
    // dialogs on Linux run over DBus and never complete without it.
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

std::vector<std::uint8_t> readRom(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

}  // namespace

int main() {
    std::filesystem::path file;
    switch (askForRom(file)) {
        case DialogEnd::Chosen:
            break;
        case DialogEnd::Cancelled:
            std::printf("No ROM was chosen, so there is nothing to run.\n");
            return 0;
        case DialogEnd::Failed:
            return 1;
    }

    const std::vector<std::uint8_t> rom = readRom(file);
    if (rom.empty()) {
        std::printf("%s could not be read, or holds no bytes.\n", file.string().c_str());
        return 1;
    }

    // Which machine to host it on is the image's own answer: a cartridge flagged for Color runs on a
    // Color machine, and everything else on the original. The platform parses no cartridge header —
    // this program holds the bytes, so it reads the one byte it needs.
    constexpr std::size_t kColorFlagByte = 0x0143;
    const bool            color = rom.size() > kColorFlagByte &&
                       (rom[kColorFlagByte] == 0x80 || rom[kColorFlagByte] == 0xC0);

    const EngineConfig config{
        .identity     = {.organization = "Retro++", .application = "GbPlayer"},
        .window       = {.title = "Polyrhythm — Game Boy player (tick-advanced | free-running)"},
        .viewport     = ViewportResolution{kViewW, kViewH},
        .enhancements = {.windowScale = kScale}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The same image on two machines, so the only difference between the two screens is the clock each
    // one runs on. Both are hosted and told to draw here; when each one STARTS is the delicate part,
    // and it is handled below.
    const VMPlatform model = color ? VMPlatform::GameBoyColor : VMPlatform::GameBoy;
    Vm               ticked{model};
    Vm               freeRunning{model};
    for (Vm* machine : {&ticked, &freeRunning}) {
        machine->hostRom(std::span<const std::uint8_t>(rom));
        machine->picture(true);  // the machine draws — off by default, because a raster costs cycles
    }
    ticked.run(Vm::Advance::OnTick);  // the engine's tick is its clock: one tick, one of its frames

    // THE TWO CLOCKS START AT DIFFERENT MOMENTS UNLESS THEY ARE MADE TO. A machine given a thread of
    // its own begins consuming wall-clock time the instant run() returns; a tick-advanced one does not
    // move until its first advanceTick, which is several window-and-loop-startup milliseconds later.
    // Starting them on adjacent lines would hand the free-running side a head start of a few frames —
    // and since both then run at the same rate, that offset would persist for the whole session rather
    // than washing out. So the free-running machine is started from inside the tick that first advances
    // the other one, which is the only moment the two share.
    bool freeRunningStarted = false;
    loop.simTick([&](const InputState&) {
        if (!freeRunningStarted) {
            freeRunning.run(Vm::Advance::Continuously);
            freeRunningStarted = true;
        }
        ticked.advanceTick();
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        // Each machine's picture is one screen wide and the layer spans both, so the right-hand one is
        // placed by scrolling its content half a viewport to the left. Outside its own dimensions a
        // picture draws nothing, which is what keeps the two halves from overlapping.
        DrawLayer left{.key = "tick-advanced"};
        left.z       = 0;
        left.size    = PixelSize{kViewW, kViewH};
        left.content = ticked.picture();
        frame.layers.push_back(left);

        DrawLayer right{.key = "free-running"};
        right.z       = 1;
        right.size    = PixelSize{kViewW, kViewH};
        right.scroll  = LayerScroll{-kGuestW, 0};
        right.content = freeRunning.picture();
        frame.layers.push_back(right);

        renderer.renderFrame(frame);
    });

    std::printf(
        "Running %s on two machines from one image.\n"
        "  LEFT  — advanced by the engine's tick: one tick, one of its frames.\n"
        "  RIGHT — free-running on a clock of its own, at the hardware's cadence.\n"
        "Both clocks run at the same nominal rate, so the two stay in step; a frame of wobble now and "
        "then is each side crossing a frame boundary at a slightly different moment.\n"
        "Close the window to quit.\n",
        file.filename().string().c_str());
    WindowedHost host{loop, platform};
    host.run();
    ticked.stop();
    freeRunning.stop();
    return 0;
}
