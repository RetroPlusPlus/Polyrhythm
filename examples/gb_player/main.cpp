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
// THE TWO GAMES WILL SEPARATE ONCE YOU PLAY THEM, and that is the setup rather than a fault. Both
// machines are handed the same buttons, but each takes them at ITS OWN next step boundary — so a press
// that lands a frame earlier on one side puts that Mario somewhere the other one is not, and from there
// they are two playthroughs. The clocks stay together; the games do not. Over a minute of real play the
// two run the same number of frames to the frame, which is what says the separation is the input
// landing and never the pacing.
//
// HOW TO READ THE TWO SCREENS PRECISELY: fuse them stereoscopically — cross or diverge your eyes until
// the two halves overlap into one image. Two identical pictures fuse into a single stable percept; a
// one-pixel offset or a single frame of divergence breaks the fusion into visible shimmer at once. It
// resolves differences that side-by-side comparison cannot, and it costs nothing to try.
//
// WHAT THIS DEMONSTRATES. A hosted machine's completed frames are a layer's content, so a machine's
// screen composites with native layers by z like tiles or sprites do:
//
//     Vm machine{model, VmConfig{.key = "…", .video = true}};  // named, and drawing; both are off until asked
//     machine.run(Vm::Advance::OnTick);              // one engine tick advances it by one of its frames
//     ...
//     screen.content = machine.video();              // the last complete frame it drew
//
// The whole of the picture path is those three lines. Everything else here is choosing a file and
// standing up a window.
//
// ONE TICK IS ONE FRAME, exactly. The Game Boy tick period IS the machine's frame period by
// construction (70'224 cycles at 4'194'304 Hz), so this loop advances the machine by exactly one of
// its frames per tick, forever, with nothing to drift.
//
// WHAT THIS PLAYER REACHES IS THE PICTURE AND THE BUTTONS. A ROM boots and plays; its sound is its own
// surface on a hosted machine, and this player picks that up when it lands. Both machines take the same
// buttons, so the two screens are played together.
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
#include "retropp/gb.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
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

// ── Capture mode ────────────────────────────────────────────────────────────────────────────────
// `retropp-gb_player-demo <seconds> [out.csv]` picks a ROM as usual, runs for a fixed time, and writes
// one row per rendered frame. Run plainly, it captures nothing and takes no arguments.
//
// What it measures: how each machine's frames line up with the frames the game draws. `picture()`
// answers with a generation — how many frames that machine has finished — so the DELTA between two
// consecutive rows says what happened to that machine's output over one drawn frame:
//
//     0  the same picture was drawn twice — the machine finished nothing in that interval
//     1  one machine frame, one drawn frame — the cadence is preserved exactly
//    ≥2  the machine finished more than one and only the newest was drawn; the rest were dropped
//
// A tick-advanced machine advances exactly once per tick, so its delta is 1 by construction. A machine
// on a clock of its own is sampled rather than stepped, so its deltas are the measurement. The display
// refresh is recorded on every row because it decides what a correct histogram even looks like — a
// panel running at twice the machine's rate makes a 0 the expected case rather than a defect.
struct CaptureRow {
    std::uint64_t frame      = 0;
    std::uint64_t tNs        = 0;   // since the first captured frame
    std::uint64_t tickedGen  = 0;
    std::uint64_t freeGen    = 0;
    std::uint64_t tickedStep = 0;   // generation delta since the previous row
    std::uint64_t freeStep   = 0;
    int           presented  = 0;   // whether this frame reached the screen — the honest frame counter
    float         displayHz  = 0.0f;
};

// Write the buffered rows once, at the end. Nothing touches the disk during the run, so the capture
// does not perturb the thing it is measuring. Every frame is recorded — there is no threshold and no
// filtering, because the interesting frame is always the one a filter would have dropped.
void writeCapture(const std::string& path, const std::vector<CaptureRow>& rows) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "gb_player: cannot open %s for writing\n", path.c_str());
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
    std::fprintf(stderr, "gb_player: wrote %zu rows to %s\n", rows.size(), path.c_str());
}

std::vector<std::uint8_t> readRom(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
}

// What to call the file an image's data is kept in. A battery save's name is one path component, and
// an image's name is whatever a player happened to call it on disk — so the characters that would make
// it more than one component are replaced. Any program naming a file after something a player chose
// does this; the platform refuses such a name rather than quietly writing somewhere else.
//
// EMPTY when the image's name yields nothing usable, and the caller then says nothing at all: the
// machine keeps whatever the platform calls a save by default, which is not this program's business to
// know or to repeat.
std::string keptAs(const std::filesystem::path& file) {
    std::string name;
    for (const char c : file.stem().string()) {
        const bool plain = (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '-' ||
                           c == '_' || c == ' ' || c == '.';
        name += plain ? c : '-';
    }
    return (name == "." || name == "..") ? std::string{} : name;
}

}  // namespace

int main(int argc, char** argv) {
    // Capture mode: `gb_player <seconds> [out.csv] [rom]`. Without arguments the player just plays and
    // asks for a ROM through the picker. Naming a ROM skips the dialog, which is what lets a capture run
    // start to finish without a hand on it.
    const double      capSeconds = (argc > 1) ? std::strtod(argv[1], nullptr) : 0.0;
    const std::string capPath    = (argc > 2) ? argv[2] : "gb_player_capture.csv";
    const std::string romArg     = (argc > 3) ? argv[3] : "";
    const bool        capturing  = capSeconds > 0.0;

    std::filesystem::path file{romArg};
    if (romArg.empty()) {
        switch (askForRom(file)) {
            case DialogEnd::Chosen:
                break;
            case DialogEnd::Cancelled:
                std::printf("No ROM was chosen, so there is nothing to run.\n");
                return 0;
            case DialogEnd::Failed:
                return 1;
        }
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
    // one runs on. Both declare their outputs at construction — video is off unless a machine is asked
    // for it, because a raster costs cycles a machine that never displays should not pay. When each one
    // STARTS is the delicate part, and it is handled below.
    //
    // Each is NAMED, so what its guest writes survives the program: a player who saves inside the game
    // finds that save here again next time. The two are named apart because they are two machines — one
    // set of data each, the way two cartridges in two consoles do not share one battery.
    const VMPlatform model = color ? VMPlatform::GameBoyColor : VMPlatform::GameBoy;
    Vm               ticked{model, VmConfig{.key = "ticked", .video = true}};
    Vm               freeRunning{model, VmConfig{.key = "free-running", .video = true}};
    // This program plays whatever it is handed, so it cannot have said at construction what the data
    // belongs to. It says so here, once it has the image — before hosting it, so what the last session
    // kept is read back as the cartridge arrives. An image whose name yields nothing usable is not
    // named at all, and keeps its data under whatever the platform calls a save by default.
    const std::string saveFor = keptAs(file);
    for (Vm* machine : {&ticked, &freeRunning}) {
        if (!saveFor.empty()) {
            machine->batterySave(saveFor);
        }
        machine->hostRom(std::span<const std::uint8_t>(rom));
    }
    // The guest's own button vocabulary, bound to whatever this program likes. Editing these rows is
    // all rebinding ever is.
    ActionMap controls{
        {gb::Button::A,      {SDL_SCANCODE_X, PadButton::FaceLabelA}},
        {gb::Button::B,      {SDL_SCANCODE_Z, PadButton::FaceLabelB}},
        {gb::Button::Select, {SDL_SCANCODE_RSHIFT}},
        {gb::Button::Start,  {SDL_SCANCODE_RETURN, PadButton::Start}},
    };
    controls.add(presets::directional(gb::Button::Up, gb::Button::Down,
                                      gb::Button::Left, gb::Button::Right));
    platform.actions(controls);

    ticked.run(Vm::Advance::OnTick);  // the engine's tick is its clock: one tick, one of its frames

    // THE TWO CLOCKS START AT DIFFERENT MOMENTS UNLESS THEY ARE MADE TO. A machine given a thread of
    // its own begins consuming wall-clock time the instant run() returns; a tick-advanced one does not
    // move until its first advanceTick, which is several window-and-loop-startup milliseconds later.
    // Starting them on adjacent lines would hand the free-running side a head start of a few frames —
    // and since both then run at the same rate, that offset would persist for the whole session rather
    // than washing out. So the free-running machine is started from inside the tick that first advances
    // the other one, which is the only moment the two share.
    bool freeRunningStarted = false;
    loop.simTick([&](const InputState& input) {
        if (!freeRunningStarted) {
            freeRunning.run(Vm::Advance::Continuously);
            freeRunningStarted = true;
        }
        // Both machines run the same image, so both get the same buttons — the left screen sees them
        // at this tick, the right one at its own next step.
        const gb::Buttons pad = gb::held(input);
        ticked.buttons(pad);
        freeRunning.buttons(pad);

        ticked.advanceTick();
    });

    // The panel's refresh, read once: it decides what a correct delta histogram looks like, so it is
    // recorded rather than assumed.
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
        // picture draws nothing, which is what keeps the two halves from overlapping.
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
            // Recorded AFTER the submission, so the row describes the frame that was just drawn from
            // these two pictures — and `presented` says whether it reached the screen at all.
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
        "Running %s on two machines from one image.\n"
        "  LEFT  — advanced by the engine's tick: one tick, one of its frames.\n"
        "  RIGHT — free-running on a clock of its own, at the hardware's cadence.\n"
        "Both clocks run at the same nominal rate, so the two stay in step; a frame of wobble now and "
        "then is each side crossing a frame boundary at a slightly different moment.\n"
        "Play it: X = A, Z = B, Enter = Start, Right Shift = Select, arrows or WASD = the d-pad; a "
        "gamepad works too. Both machines take the same buttons.\n"
        "Close the window to quit.\n",
        file.filename().string().c_str());
    WindowedHost host{loop, platform};
    host.run();
    ticked.stop();
    freeRunning.stop();
    if (capturing) {
        writeCapture(capPath, capRows);
    }
    return 0;
}
