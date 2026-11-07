// Game Boy machine state — a cartridge in a window, played from the pad, with a key that takes the
// machine's whole state and a key that puts it back.
//
// The cartridge is authored right here: a block the d-pad moves a pixel a frame, over a four-note round.
//
//   arrows   move the block (a pad's d-pad does too)
//   S        take the machine's state, and print the frame it stands at
//   L        put the state back: the block goes back to where it stood when S was pressed
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
#include "retropp/gb.h"             // gb::held, gb::Buttons — the pad
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

// The cartridge's own program, in two pieces: what it does once, and what it does every frame. A block
// the d-pad moves a pixel a frame, over a dark background, and a round of four notes, one every 32 frames.
constexpr std::string_view kSetupSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ei
    ld a, $50
    ldh [$FF81], a      ; the block's X
    ldh [$FF82], a      ; and its Y
    halt                ; the first vertical blank: video memory takes writes
    nop
    ld hl, $8000        ; tiles 0 and 1: empty, and every cell of the blank map shows tile 0
    ld b, $20
    xor a
blank:
    ld [hl+], a
    dec b
    jr nz, blank
    ld b, $10           ; tiles 2 and 3, one tall object: light, with an amber edge at each side
block:
    ld a, $FF
    ld [hl+], a
    ld a, $81
    ld [hl+], a
    dec b
    jr nz, block
    ld a, $80
    ldh [$FF68], a      ; background palette 0, from color 0, stepping
    ld a, $82
    ldh [$FF69], a
    ld a, $28
    ldh [$FF69], a      ; a dark blue
    ld a, $82
    ldh [$FF6A], a      ; object palette 0, from color 1, stepping
    ld a, $7F
    ldh [$FF6B], a
    ld a, $6F
    ldh [$FF6B], a      ; color 1: a pale cream
    ld a, $00
    ldh [$FF6B], a
    ldh [$FF6B], a      ; color 2, which the tiles leave unused
    ld a, $5F
    ldh [$FF6B], a
    ld a, $1E
    ldh [$FF6B], a      ; color 3: amber
    ld a, $97
    ldh [$FF40], a      ; the display on, tiles from $8000, background and tall objects
    ld a, $80
    ldh [$FF26], a      ; the sound chip on
    ld a, $FF
    ldh [$FF25], a      ; every channel to both sides
    ld a, $77
    ldh [$FF24], a      ; full volume
    ld a, $80
    ldh [$FF11], a      ; a square wave, half duty
    ld a, $A0
    ldh [$FF12], a      ; held at a moderate level
    ld a, $06
    ldh [$FF13], a
    ld a, $87
    ldh [$FF14], a      ; the first note
    jp $0200            ; on to the frame loop, placed there
)";

constexpr std::string_view kFrameSource = R"(
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ld c, a
    and $1F
    jr nz, pad          ; a note every 32 frames
    ld a, c
    rlca
    rlca
    rlca
    and $03             ; which of the four
    ld b, $06           ; C
    cp $01
    jr nz, pastE
    ld b, $39           ; E
pastE:
    cp $02
    jr nz, pastG
    ld b, $59           ; G
pastG:
    cp $03
    jr nz, sound
    ld b, $83           ; the C above
sound:
    ld a, b
    ldh [$FF13], a
    ld a, $87
    ldh [$FF14], a      ; sounding
pad:
    ld a, $20
    ldh [$FF00], a      ; the d-pad's four lines
    ldh a, [$FF00]
    ldh a, [$FF00]
    ldh a, [$FF00]      ; read as they settle: right, left, up, down from bit 0, 0 when held
    ld d, a
    ld a, $30
    ldh [$FF00], a
    ldh a, [$FF81]
    bit 0, d
    jr nz, notRight
    inc a
notRight:
    bit 1, d
    jr nz, notLeft
    dec a
notLeft:
    ldh [$FF81], a
    ld c, a             ; X, for the two objects
    ldh a, [$FF82]
    bit 2, d
    jr nz, notUp
    dec a
notUp:
    bit 3, d
    jr nz, notDown
    inc a
notDown:
    ldh [$FF82], a
    ld b, a             ; Y
    ld hl, $FE00        ; two tall objects, side by side
    ld [hl+], a
    ld a, c
    ld [hl+], a
    ld a, $02
    ld [hl+], a
    xor a
    ld [hl+], a
    ld a, b
    ld [hl+], a
    ld a, c
    add a, $08
    ld [hl+], a
    ld a, $02
    ld [hl+], a
    xor a
    ld [hl+], a
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
    const std::vector<std::uint8_t> setup = assembler.assemble(std::string(kSetupSource));
    const std::vector<std::uint8_t> frame = assembler.assemble(std::string(kFrameSource));
    std::copy(setup.begin(), setup.end(), rom.begin() + 0x150);
    std::copy(frame.begin(), frame.end(), rom.begin() + 0x200);
    return rom;
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────

// What one run of ticks left behind: every picture the machine finished, and every sample it sounded.
struct Stretch {
    std::vector<std::vector<std::uint8_t>> pictures;
    std::vector<std::int16_t>              samples;
};

// Ten ticks with right held, keeping video()'s pixels whenever its generation has moved. `sound` is what
// the machine's audio function fills, emptied here so it holds this stretch's alone.
Stretch tenTicks(Vm& machine, std::vector<std::int16_t>& sound) {
    constexpr int kTicks = 10;
    Stretch       kept;
    sound.clear();
    std::uint64_t generation = machine.video().generation;
    for (int tick = 0; tick < kTicks; ++tick) {
        machine.buttons(gb::Buttons{.right = true});
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
        machine.buttons(gb::Buttons{.right = true});
        machine.advanceTick();
    }

    const std::vector<std::uint8_t> state = machine.state();
    const bool                      took  = machine.restore(state);
    check(took && machine.state() == state, "a state put straight back gives the same bytes");

    const Stretch                   first      = tenTicks(machine, sound);
    const std::vector<std::uint8_t> firstBytes = machine.state();
    for (int tick = 0; tick < 30; ++tick) {
        machine.buttons(gb::Buttons{.left = true});
        machine.advanceTick();
    }
    const bool                      tookAgain   = machine.restore(state);
    const Stretch                   second      = tenTicks(machine, sound);
    const std::vector<std::uint8_t> secondBytes = machine.state();

    check(tookAgain && first.pictures.size() >= 8 && first.pictures.front() != first.pictures.back() &&
              first.pictures == second.pictures,
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

    // The d-pad on the arrows and a pad's own, so S and L stay the two state keys.
    platform.actions(ActionMap{
        {Action::TakeState, {SDL_SCANCODE_S}},
        {Action::PutBack, {SDL_SCANCODE_L}},
        {gb::Button::Up, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {gb::Button::Down, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {gb::Button::Left, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {gb::Button::Right, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
    });

    Vm::GBC machine{VmConfig{.video = true}};
    machine.hostRom(authorGame(machine));
    const RegionMapId<Places> places = machine.registerRegions(
        regions(region(&Places::frames, MemoryRegion{.at = 0xFF80, .size = 1}, "frame counter")));
    machine.run(Vm::Advance::OnTick);

    const auto frameNow = [&] { return static_cast<unsigned>(machine.read(places, &Places::frames).at(0)); };

    std::optional<std::vector<std::uint8_t>> kept;
    loop.simTick([&](const InputState& in) {
        machine.buttons(gb::held(in));
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
        "A cartridge played from the pad. The arrows move its block. S takes the machine's whole state; L "
        "puts it back, and the block goes back to where it stood when S was pressed.\n"
        "Close the window to quit.\n");
    WindowedHost host{loop, platform};
    host.run();
    machine.stop();
    return 0;
}
