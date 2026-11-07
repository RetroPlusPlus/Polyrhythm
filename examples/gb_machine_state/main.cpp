// Game Boy machine state — a cartridge in a window, with a key that takes the machine's whole state and
// a key that puts it back.
//
// The cartridge is authored right here: its stripes move a pixel a frame and the top row of every tile
// spells the frame count, so no two frames are the same picture.
//
//   S   take the machine's state, and print the frame it stands at
//   L   put the state back: the picture jumps to where it stood when S was pressed
//
//     std::vector<std::uint8_t> bytes = machine.state();
//     machine.restore(bytes);
//
// Modes:
//   (no args)   the window
//   --verify    headless: a state put back gives the same bytes; the frames and the sound after a
//               restore are the first run's, and the machine lands on the same bytes; a machine that has
//               never run carries on from a state; bytes that are not its own are refused; and a machine
//               on a thread of its own hands its state over and takes it back as it runs. Exits nonzero
//               on any miss (CI runs this on every platform)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/geometry.h"
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/vm.h"             // Vm — hosting the cartridge, running it, its state
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

constexpr int kGuestW = 160, kGuestH = 144;
constexpr int kScale  = 4;  // 160×144 × 4 = a 640×576 window

// The cartridge's own program: stripes that move a pixel a frame, the count spelled along the top
// row of every tile so no two frames are the same picture, and a tone that steps with it.
constexpr std::string_view kGameSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ei
    halt                ; the first vertical blank: video RAM takes writes
    nop
    ld hl, $8000        ; tile 0, which every cell of the blank map shows
    ld b, $10
stripes:
    ld a, $F0           ; four dark columns, four light
    ld [hl+], a
    dec b
    jr nz, stripes
    ld a, $80
    ldh [$FF26], a      ; the sound chip on
    ld a, $FF
    ldh [$FF25], a      ; every channel to both sides
    ld a, $77
    ldh [$FF24], a      ; full volume
    ld a, $80
    ldh [$FF11], a      ; a square wave, half duty
    ld a, $F0
    ldh [$FF12], a      ; loud, and held
    ld a, $87
    ldh [$FF14], a      ; sounding
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ldh [$FF43], a      ; the stripes move a pixel
    ld [$8000], a       ; and the top row of every tile spells the count
    ldh [$FF13], a      ; the tone steps with it
    jr loop
)";

// The one place this program reads. Never instantiated: the struct exists so the place has a name.
struct Places {
    MemoryRegion frames;  // the guest's own frame counter, in high RAM
};

// One authored cartridge: entry jumps to the program at $0150, the VBlank vector returns.
std::vector<std::uint8_t> authorGame(Vm& assembler) {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x0147] = 0x00;  // ROM only
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;                                          // entry: nop
    rom[0x0101] = 0xC3; rom[0x0102] = 0x50; rom[0x0103] = 0x01;  // jp $0150
    const std::vector<std::uint8_t> code = assembler.assemble(std::string(kGameSource));
    std::copy(code.begin(), code.end(), rom.begin() + 0x150);
    return rom;
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────

// What one run of ticks left behind: every picture the machine finished, and every sample it sounded.
struct Stretch {
    std::vector<std::vector<std::uint8_t>> pictures;
    std::vector<std::int16_t>              samples;
};

// Ten ticks, keeping video()'s pixels whenever its generation has moved. `sound` is what the machine's
// audio function fills, emptied here so it holds this stretch's alone.
Stretch tenTicks(Vm& machine, std::vector<std::int16_t>& sound) {
    constexpr int kTicks = 10;
    Stretch       kept;
    sound.clear();
    std::uint64_t generation = machine.video().generation;
    for (int tick = 0; tick < kTicks; ++tick) {
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
    std::printf("gb_machine_state --verify: a machine's whole state, taken and put back\n\n");

    Vm::GBC                         machine{VmConfig{.video = true}};
    const std::vector<std::uint8_t> image = authorGame(machine);
    std::vector<std::int16_t>       sound;
    machine.hostRom(image);
    machine.audio(true, {.rate = 48'000, .onSample = [&sound](std::int16_t left, std::int16_t right) {
                             sound.push_back(left);
                             sound.push_back(right);
                         }});
    machine.run(Vm::Advance::OnTick);
    for (int tick = 0; tick < 40; ++tick) {
        machine.advanceTick();
    }

    const std::vector<std::uint8_t> state = machine.state();
    const bool                      took  = machine.restore(state);
    check(took && machine.state() == state, "a state put straight back gives the same bytes");

    const Stretch                   first      = tenTicks(machine, sound);
    const std::vector<std::uint8_t> firstBytes = machine.state();
    for (int tick = 0; tick < 30; ++tick) {
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
        Vm::GBC                   fresh{VmConfig{.video = true}};
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

    {
        Vm::GB original;
        original.hostRom(image);
        const std::vector<std::uint8_t> foreign = original.state();
        check(!machine.restore(foreign), "a Game Boy's state is refused by a Game Boy Color");
    }

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
        .identity     = {.organization = "Retro++", .application = "GbMachineState"},
        .window       = {.title = "Polyrhythm — Game Boy machine state"},
        .viewport     = ViewportResolution{kGuestW, kGuestH},
        .enhancements = {.windowScale = kScale}};
    EngineConfig::setActive(config);
    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    platform.actions(ActionMap{
        {Action::TakeState, {SDL_SCANCODE_S}},
        {Action::PutBack, {SDL_SCANCODE_L}},
    });

    Vm::GBC machine{VmConfig{.video = true}};
    machine.hostRom(authorGame(machine));
    const RegionMapId<Places> places = machine.registerRegions(
        regions(region(&Places::frames, MemoryRegion{.at = 0xFF80, .size = 1}, "frame counter")));
    machine.run(Vm::Advance::OnTick);

    const auto frameNow = [&] { return static_cast<unsigned>(machine.read(places, &Places::frames).at(0)); };

    std::optional<std::vector<std::uint8_t>> kept;
    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::TakeState)) {
            kept = machine.state();
            std::printf("state taken at frame %u — %zu bytes\n", frameNow(), kept->size());
        }
        if (in.justPressed(Action::PutBack)) {
            if (kept && machine.restore(*kept)) {
                std::printf("put back — the machine stands at frame %u again\n", frameNow());
            } else {
                std::printf("nothing taken yet\n");
            }
        }
        machine.advanceTick();
    });

    FrameDrawState frame;
    loop.renderLoop([&]() {
        frame.layers.clear();
        DrawLayer screen{.key = "machine"};
        screen.z       = 0;
        screen.size    = PixelSize{kGuestW, kGuestH};
        screen.content = machine.video();
        frame.layers.push_back(screen);
        renderer.renderFrame(frame);
    });

    std::printf(
        "A cartridge whose picture changes every frame. S takes the machine's whole state; L puts it back, "
        "and the picture jumps to where it stood when S was pressed.\n"
        "Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    machine.stop();
    return 0;
}
