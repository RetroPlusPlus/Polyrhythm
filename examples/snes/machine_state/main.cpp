// SNES machine state — the demo cartridge in a window, played from the pad, with a key that takes the
// machine's whole state and a key that puts it back.
//
//   arrows   move the cartridge's sprite (a pad's d-pad does too)
//   S        take the machine's state, and print the frame it stands at
//   L        put the state back: the picture and the sound go back to where they stood when S was pressed
//
//     std::vector<std::uint8_t> bytes = machine.state();
//     machine.restore(bytes);
//
// Modes:
//   (no args)   the window
//   --verify    headless: a state put back gives the same bytes; the frames and the sound after a
//               restore are the first run's, and the machine lands on the same bytes; a machine that has
//               never run carries on from a state; bytes that are not a state are refused; and a machine
//               on a thread of its own hands its state over and takes it back as it runs. Exits nonzero
//               on any miss (CI runs this on every platform)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/snes.h"           // snes::held, snes::Ports — the pads
#include "retropp/vm.h"             // Vm — hosting the cartridge, running it, its state
#include "retropp/windowed_host.h"

#include "examples/snes/cartridge/cartridge.h"

using namespace retropp;

namespace {

constexpr int kSlotW = 512, kSlotH = 448;  // the console's largest picture, and the slot it lands in

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────

// What one run of ticks left behind: every picture the machine finished, and every sample it sounded.
struct Stretch {
    std::vector<std::vector<std::uint8_t>> pictures;
    std::vector<std::int16_t>              samples;
};

// Port one holding one direction, the other port empty.
snes::Ports holding(snes::Buttons pad) { return snes::Ports{.one = pad, .two = std::nullopt}; }

// Ten ticks with right held, keeping video()'s pixels whenever its generation has moved. `sound` is what
// the machine's audio function fills, emptied here so it holds this stretch's alone.
Stretch tenTicks(Vm& machine, std::vector<std::int16_t>& sound) {
    constexpr int kTicks = 10;
    Stretch       kept;
    sound.clear();
    std::uint64_t generation = machine.video().generation;
    for (int tick = 0; tick < kTicks; ++tick) {
        machine.buttons(holding(snes::Buttons{.right = true}));
        machine.advanceTick();
        const RasterContent picture = machine.video();
        if (picture.generation != generation) {
            generation = picture.generation;
            kept.pictures.emplace_back(picture.pixels.begin(), picture.pixels.end());
        }
    }
    kept.samples = sound;
    return kept;
}

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "ok" : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_machine_state --verify: a machine's whole state, taken and put back\n\n");

    const std::vector<std::uint8_t> image = examples::snes::demoCartridge();
    Vm::SNES                        machine{VmConfig{.video = true}};
    std::vector<std::int16_t>       sound;
    machine.hostRom(image);
    machine.audio(true, {.rate = 48'000, .onSample = [&sound](std::int16_t left, std::int16_t right) {
                             sound.push_back(left);
                             sound.push_back(right);
                         }});
    machine.run(Vm::Advance::OnTick);
    for (int tick = 0; tick < 40; ++tick) {
        machine.buttons(holding(snes::Buttons{.right = true}));
        machine.advanceTick();
    }

    const std::vector<std::uint8_t> state = machine.state();
    const bool                      took  = machine.restore(state);
    check(took && machine.state() == state, "a state put straight back gives the same bytes");

    const Stretch                   first      = tenTicks(machine, sound);
    const std::vector<std::uint8_t> firstBytes = machine.state();
    for (int tick = 0; tick < 30; ++tick) {
        machine.buttons(holding(snes::Buttons{.left = true}));
        machine.advanceTick();
    }
    const bool                      tookAgain   = machine.restore(state);
    const Stretch                   second      = tenTicks(machine, sound);
    const std::vector<std::uint8_t> secondBytes = machine.state();

    check(tookAgain && first.pictures.size() >= 8 && first.pictures == second.pictures,
          "the frames after a restore are the frames the first run drew");
    const bool sounded = std::any_of(first.samples.begin(), first.samples.end(),
                                     [](std::int16_t sample) { return sample != 0; });
    check(sounded && first.samples == second.samples, "and its sound is the first run's, sample for sample");
    check(firstBytes == secondBytes, "tick for tick the machine lands on the same bytes");

    {
        Vm::SNES                  fresh{VmConfig{.video = true}};
        std::vector<std::int16_t> unheard;
        fresh.hostRom(image);
        const bool tookFresh = fresh.restore(state);
        fresh.run(Vm::Advance::OnTick);
        const Stretch carried = tenTicks(fresh, unheard);
        fresh.stop();
        check(tookFresh && carried.pictures == first.pictures,
              "a machine that has never run carries on from a state with no boot");
    }

    const std::vector<std::uint8_t> before = machine.state();
    const std::span<const std::uint8_t> cut(state.data(), state.size() - 1);
    const bool                          refusedCut = !machine.restore(cut);
    check(refusedCut && machine.state() == before, "bytes cut short are refused and the machine is as it was");

    const std::vector<std::uint8_t> noState(4'096, 0xA5);
    check(!machine.restore(noState), "bytes that are no state are refused");

    machine.stop();
    machine.run();
    const std::vector<std::uint8_t> running = machine.state();
    const bool                      tookRunning = !running.empty() && machine.restore(running);
    machine.stop();
    check(tookRunning, "a machine on a thread of its own hands its state over and takes it back as it runs");

    return failures == 0 ? 0 : 1;
}

enum class Action : ActionId { TakeState = 20, PutBack };

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesMachineState"},
        .window   = {.title = "Polyrhythm — SNES machine state"},
        .viewport = ViewportResolution{kSlotW, kSlotH},
        .timing   = TimingProfile::Snes,
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    // The d-pad on the arrows and a pad's own, so S and L stay the two state keys.
    platform.actions(ActionMap{
        {Action::TakeState, {SDL_SCANCODE_S}},
        {Action::PutBack, {SDL_SCANCODE_L}},
        {snes::Button::Up, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {snes::Button::Down, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {snes::Button::Left, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {snes::Button::Right, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
    });

    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge());
    machine.run(Vm::Advance::OnTick);

    std::optional<std::vector<std::uint8_t>> kept;
    unsigned                                 keptFrame = 0;
    loop.simTick([&](const InputState& in) {
        machine.buttons(snes::Ports{.one = snes::held(in), .two = std::nullopt});
        if (in.justPressed(Action::TakeState)) {
            kept      = machine.state();
            keptFrame = static_cast<unsigned>(machine.video().generation);
            std::printf("state taken at frame %u — %zu bytes\n", keptFrame, kept->size());
        }
        if (in.justPressed(Action::PutBack)) {
            if (kept && machine.restore(*kept)) {
                std::printf("put back — the machine stands at frame %u again\n", keptFrame);
            } else {
                std::printf("nothing taken yet\n");
            }
        }
        machine.advanceTick();
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        RasterContent picture = machine.video();
        picture.fit           = PixelSize{kSlotW, kSlotH};
        DrawLayer screen{.key = "machine"};
        screen.z       = 0;
        screen.size    = PixelSize{kSlotW, kSlotH};
        screen.content = picture;
        frame.layers.push_back(screen);
        renderer.renderFrame(frame);
    });

    std::printf(
        "The demo cartridge, played from the pad. The arrows move its sprite. S takes the machine's whole "
        "state; L puts it back, and the picture jumps to where it stood when S was pressed.\n"
        "Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    machine.stop();
    return 0;
}
