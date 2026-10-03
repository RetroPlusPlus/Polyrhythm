// SNES co-execution demo — a cartridge running its own world, and this program reading and writing the
// places inside it while it runs.
//
// The picture on the left is the demo cartridge (examples/snes/cartridge) on its own thread, at the
// console's own speed. The arrows drive its sprite. The panel on the right is not the cartridge's picture:
// it is the machine's own memory, read every tick through places this program declares —
//
//   the 256 palette words      snes::Palette, the whole of CGRAM, drawn as a 16x16 grid of swatches
//   tiles 0 to 15 of video RAM  sixteen 32-byte entries at snes::videoRam(0), decoded from 4 bpp
//   the sprite's X and Y       two bytes of work RAM at $7E:0010, where the cartridge keeps them
//   the step                   one byte of the cartridge IMAGE at $00:8300, which its frame handler
//                              reads to decide how far a held direction moves the sprite
//   the pad registers          JOY1 at $4218-$4219, the buttons the console latched at vertical blank,
//                              and RDNMI at $4210 — registers, read as they stand: a read here clears
//                              no flag the cartridge's own read of $4210 would clear
//
// Every key writes one of those places, and the cartridge carries on with what it finds:
//
//   1 2    write the sprite's color, palette word 129, to blue or yellow
//   3 4    move the sprite 32 pixels left or right, by writing its X in work RAM
//   5 6    patch the step byte in the image, 1 to 8, while it runs; hold an arrow to see it
//   SPACE  park the machine where it stands, and resume from there
//
// And a routine is a typed C++ function, two ways. A second machine, `routines`, holds no cartridge: it
// runs `mix` — the average of two bytes, written as 65816 source in routines/mix.asm with no address of
// its own — placed where the machine has room. The cartridge carries the same routine at $00:8400, bound
// where it sits and called in the cartridge's own context while it is parked:
//
//   7      call the placed routine with the next argument, and write the result to the sprite's color
//   8      call the cartridge's own copy the same way — park the machine first, with SPACE
//
// A parked machine stands wherever its clock stopped, usually part-way through an instruction. A call
// into it lands at the next instruction boundary: the instruction in flight finishes under the
// cartridge's own registers before the routine's arguments go in, so the answer is exact wherever the
// machine was parked. The panel counts the calls 8 made and how many answered exactly.
//
// And native code woven into the cartridge while it runs, through two escapes and two watches the program
// declares before it starts. The frame handler steps the sprite for every held direction by calling its own
// `pace` with the coordinate in A and the direction in X, and calls an empty `report` as it finishes; `pace`
// reads the step byte, and the handler stores the sprite's Y:
//
//   P      answer `pace` natively: the sprite moves by the step averaged with 8 — 4 a frame for a step of
//          1 — the average computed by calling the cartridge's own `mix` from inside the escape
//   X      switch the escape at `report` on and off; the panel counts the frames it hears
//   H      the watch on the sprite's Y: free, every store vetoed, or every store held between 64 and 160
//   L      answer every read of the step byte with 3, while the image still holds what 5 and 6 wrote
//
// Modes:
//   (no args)   the window
//   --verify    headless: declares the six places, round-trips a palette word and the sprite's X, checks
//               a patched step moves the sprite that far a frame, reads the pad registers with Right held
//               and released, calls both routines for a value, calls the cartridge's own routine parked
//               at 240 different points of its frame, and switches each escape and watch on in turn and
//               checks what it does to the running cartridge; exits nonzero on any miss (CI runs this on
//               every platform)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
#include "retropp/guest_escape.h"   // GuestEscape — where control leaves the cartridge's code
#include "retropp/guest_watch.h"    // GuestWatch, AccessVerdict — the cartridge's own accesses, decided here
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/palette.h"
#include "retropp/raster_content.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/snes.h"           // snes::Palette, snes::videoRam — the machine's memories by name; snes::A, snes::X
#include "retropp/vm.h"             // Vm — hosting, running, reading and writing; Routine, RoutineBinding, Isa
#include "retropp/windowed_host.h"

#include "examples/snes/cartridge/cartridge.h"

using namespace retropp;

namespace {

// ── The window ──────────────────────────────────────────────────────────────────────────────────

constexpr int kGlyphPx = 16, kGlyphStride = 128 / 8;  // font.png is 128 wide, in 8px tiles
constexpr int kCols = 64, kRows = 32;
constexpr int kViewW = kCols * kGlyphPx, kViewH = kRows * kGlyphPx;  // 1024 x 512

constexpr int kSlotW = 512, kSlotH = 448;           // the console's largest picture, and the slot it lands in
constexpr int kSlotTop = (kViewH - kSlotH) / 2;     // centered down the left half
constexpr int kPanelX = kViewW - kSlotW;            // the right half, where the panel is
constexpr int kPanelW = kViewW - kPanelX;

constexpr int kHeadCol  = 33;  // a heading
constexpr int kNameCol  = 34;  // a row's label, indented under its heading
constexpr int kValueCol = 42;  // a row's value
constexpr int kKeyCol   = 48;  // the keys that change a heading's rows
constexpr int kHookCol  = 43;  // the escapes and watches, beside the swatches

constexpr int kSwatchPx = 8;                        // one palette word
constexpr int kSwatchX = 32, kSwatchY = 5 * kGlyphPx;
constexpr int kTileScale = 2;                       // one tile pixel, drawn 2x2
constexpr int kStripX = 32, kStripY = 15 * kGlyphPx;

// ── The cartridge's places ──────────────────────────────────────────────────────────────────────

constexpr std::uint32_t kSpriteXY  = 0x7E0010;  // sprite 0's X, then its Y, in work RAM
constexpr std::uint32_t kStepByte  = 0x008300;  // the step, in the cartridge image
constexpr std::uint32_t kMixEntry  = 0x008400;  // the cartridge's own `mix`, where it sits in the image
constexpr std::uint32_t kPace      = 0x008380;  // `pace`: a coordinate in A, a direction in X, the stepped coordinate back in A
constexpr std::uint32_t kReport    = 0x0083A0;  // `report`: an empty routine the frame handler calls as it ends
constexpr std::uint32_t kJoy1      = 0x004218;  // JOY1: the pad the console read at vertical blank, low byte first
constexpr std::uint32_t kRdnmi     = 0x004210;  // RDNMI: the NMI flag in bit 7, the CPU's version in bits 0-3
constexpr std::size_t   kColorWord = 129;       // object palette 0, color 1: the sprite's color
constexpr std::uint32_t kTileCount = 16;
constexpr std::uint8_t  kPaceWith = 8;                   // what the native `pace` averages the step with
constexpr std::uint8_t  kCorralTop = 64, kCorralBottom = 160;  // the band a corralled Y is held in
constexpr std::uint8_t  kStepAnswer = 3;                 // what a read of the step byte answers while L is on

// The routine's source, as the panel shows it; the file the machine assembles is routines/mix.asm.
constexpr std::array<std::string_view, 2> kMixSource{"STA $00  TXA  CLC  ADC $00", "ROR A  RTS"};

// `mix` as a C++ function: A and X in, the average back in A. The binding is the same for the routine
// the machine places from source and the one the cartridge already holds; the placing call spells it
// out, because the build reads the ISA from the call's own text to pick the assembler that bakes the file.
using Mix = Routine<std::uint8_t(std::uint8_t, std::uint16_t)>;
const RoutineBinding kMixBinding{.inputs = {snes::A, snes::X}, .output = snes::A, .isa = Isa::Wdc65816};

// Register `mix` from its source on a machine that hosts no cartridge. The path is a literal and the
// binding names the ISA, so the build assembles the file and bakes the bytes into this binary.
Mix placeMix(Vm& machine) {
    return machine.registerRoutine<std::uint8_t(std::uint8_t, std::uint16_t)>(
        "examples/snes/coexecution/routines/mix.asm",
        {.inputs = {snes::A, snes::X}, .output = snes::A, .isa = Isa::Wdc65816});
}

// What `mix` answers for A and X, computed here: the nine-bit sum halved, the carry back in bit 7.
[[nodiscard]] std::uint8_t mixOf(std::uint8_t a, std::uint8_t x) {
    return static_cast<std::uint8_t>((a + x) >> 1);
}

// JOY1 as one word, the high byte ($4219) on top: B Y Select Start Up Down Left Right, then A X L R.
[[nodiscard]] std::uint16_t joyWord(std::span<const std::uint8_t> joy) {
    return static_cast<std::uint16_t>(joy[0] | (joy[1] << 8));
}

// A byte as a gray palette word: the same five bits in red, green and blue.
[[nodiscard]] std::uint16_t grayWord(std::uint8_t value) {
    const std::uint16_t five = value >> 3;
    return static_cast<std::uint16_t>(five | five << 5 | five << 10);
}

// The places this program names in the machine. Never instantiated — the struct exists so the places
// have names, and naming one wrong is a compile error rather than a bad address.
struct Places {
    MemoryRegion palette;  // CGRAM, whole, by the console's own constant
    MemoryRegion tiles;    // the first sixteen 4 bpp tiles of video RAM, one entry each
    MemoryRegion sprite;   // the sprite's X and Y
    MemoryRegion step;     // one byte inside the cartridge image
    MemoryRegion pad;      // JOY1, two registers
    MemoryRegion nmi;      // RDNMI, one register
};

RegionMapId<Places> declarePlaces(Vm& machine) {
    return machine.registerRegions(regions(
        region(&Places::palette, snes::Palette, "palette"),
        region(&Places::tiles, MemoryRegion{.at = snes::videoRam(0), .size = 32, .count = kTileCount},
               "tiles"),
        region(&Places::sprite, MemoryRegion{.at = kSpriteXY, .size = 2}, "sprite"),
        region(&Places::step, MemoryRegion{.at = kStepByte, .size = 1}, "step"),
        region(&Places::pad, MemoryRegion{.at = kJoy1, .size = 2}, "pad"),
        region(&Places::nmi, MemoryRegion{.at = kRdnmi, .size = 1}, "nmi")));
}

// What the escapes and watches have done — counted on the machine's own thread, where their code runs, and
// read on the game's. `yMode` is how the watch on the sprite's Y answers while it is on: 1 vetoes every
// store, 2 holds every store inside the band; 0 is the watch switched off.
struct Hooks {
    std::atomic<int> paced{0};         // `pace` answered natively
    std::atomic<int> reports{0};       // `report` reached
    std::atomic<int> yDecided{0};      // stores to the sprite's Y the watch decided
    std::atomic<int> stepAnswered{0};  // reads of the step byte answered
    std::atomic<int> yMode{0};
};

// How far the native `pace` moves the sprite a frame: the step byte averaged with 8 by the cartridge's own
// `mix` — 4 for the image's step of 1, 8 for a step of 8.
std::uint8_t paceStride(const Mix& mix, std::uint8_t step) { return mix(step, kPaceWith); }

// The two escapes and two watches, declared before the machine runs, every one but `report` switched off.
// The native `pace` answers in the routine's own convention — the coordinate in A, the direction in X —
// and reads the step byte from the image as a third input. It calls the cartridge's own `mix` from inside
// the escape: the cartridge is between two instructions there, so the call is the same call a parked
// machine takes.
void declareHooks(Vm& machine, const Mix& mix, Hooks& hooks) {
    machine.registerEscapes(escapes(
        GuestEscape{.key      = "pace",
                    .at       = kPace,
                    .replaces = routine(RoutineBinding{.inputs = {snes::A, snes::X, Location::memory(kStepByte)},
                                                       .output = snes::A,
                                                       .isa    = Isa::Wdc65816},
                                        [mix, &hooks](std::uint8_t coordinate, std::uint16_t direction,
                                                      std::uint8_t step) -> std::uint8_t {
                                            ++hooks.paced;
                                            const std::uint8_t stride = paceStride(mix, step);
                                            return static_cast<std::uint8_t>(direction == 0x01 ? coordinate + stride
                                                                                               : coordinate - stride);
                                        }),
                    .armed    = false},
        GuestEscape{.key = "report", .at = kReport, .handler = [&hooks](Vm&, std::uint32_t) { ++hooks.reports; }}));
    machine.registerWatches(watches(
        GuestWatch{.key     = "y",
                   .at      = MemoryRegion{.at = kSpriteXY + 1, .size = 1},
                   .onWrite = [&hooks](Vm&, std::uint32_t, std::uint8_t y) {
                       ++hooks.yDecided;
                       return hooks.yMode == 1 ? AccessVerdict::veto()
                                               : AccessVerdict::instead(std::clamp(y, kCorralTop, kCorralBottom));
                   },
                   .armed = false},
        GuestWatch{.key    = "step",
                   .at     = MemoryRegion{.at = kStepByte, .size = 1},
                   .onRead = [&hooks](Vm&, std::uint32_t, std::uint8_t) {
                       ++hooks.stepAnswered;
                       return AccessVerdict::instead(kStepAnswer);
                   },
                   .armed = false}));
}

// A palette word as the console stores it — five bits each of red, green and blue, red lowest.
[[nodiscard]] std::uint16_t wordAt(std::span<const std::uint8_t> palette, std::size_t word) {
    return static_cast<std::uint16_t>(palette[word * 2] | (palette[word * 2 + 1] << 8));
}

void putWord(std::vector<std::uint8_t>& palette, std::size_t word, std::uint16_t value) {
    palette[word * 2]     = static_cast<std::uint8_t>(value & 0xFF);
    palette[word * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
}

// ── Verify mode ─────────────────────────────────────────────────────────────────────────────────
// Headless, one tick at a time, so every read answers a known step.

int runVerify() {
    int  failures = 0;
    auto check    = [&failures](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    std::printf("snes_coexecution --verify: the places inside a running SNES cartridge read and write\n\n");

    Hooks    hooks;
    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge());
    const auto places = declarePlaces(machine);
    const Mix  bound  = machine.bindRoutine<std::uint8_t(std::uint8_t, std::uint16_t)>(kMixEntry, kMixBinding);
    declareHooks(machine, bound, hooks);
    machine.run(Vm::Advance::OnTick);
    const auto ticks = [&machine](int n) {
        for (int i = 0; i < n; ++i) machine.advanceTick();
    };
    ticks(3);  // the cartridge's reset code has filled video memory and set the sprite's color

    std::vector<std::uint8_t> palette = machine.read(places, &Places::palette);
    check(wordAt(palette, kColorWord) == 0x7FFF, "palette word 129 reads the white the cartridge set");

    putWord(palette, kColorWord, 0x001F);
    machine.write(places, &Places::palette, palette);
    ticks(1);
    check(wordAt(machine.read(places, &Places::palette), kColorWord) == 0x001F,
          "palette word 129 reads back the red this program wrote");

    const std::vector<std::uint8_t> tile = machine.read(places, &Places::tiles, 3);
    check(tile[0] == 0xFF && tile[2] == 0x00, "tile 3 reads the cartridge's stripes");

    std::vector<std::uint8_t> sprite = machine.read(places, &Places::sprite);
    sprite[0]                        = 200;
    machine.write(places, &Places::sprite, sprite);
    ticks(1);
    check(machine.read(places, &Places::sprite).at(0) == 200, "the sprite's X reads back 200");

    machine.write(places, &Places::step, std::vector<std::uint8_t>{4});
    machine.buttons(snes::Ports{.one = snes::Buttons{.right = true}, .two = std::nullopt});
    ticks(2);  // the pad is read at the frame's vertical blank
    const std::uint8_t before = machine.read(places, &Places::sprite).at(0);
    ticks(1);
    const std::uint8_t after = machine.read(places, &Places::sprite).at(0);
    std::printf("  the step patched to 4: X went %u -> %u in one tick with Right held\n",
                static_cast<unsigned>(before), static_cast<unsigned>(after));
    check(static_cast<std::uint8_t>(after - before) == 4, "the sprite moves by the patched step");

    // Registers read as they stand: JOY1 holds what the console latched at vertical blank, and RDNMI
    // answers the CPU's version in its low bits.
    const std::uint16_t held = joyWord(machine.read(places, &Places::pad));
    const std::uint8_t  nmi  = machine.read(places, &Places::nmi).at(0);
    std::printf("  JOY1 with Right held: %04X; RDNMI: %02X\n", static_cast<unsigned>(held),
                static_cast<unsigned>(nmi));
    check(held == 0x0100, "JOY1 reads Right held, and nothing else");
    check((nmi & 0x0F) == 2, "RDNMI reads the CPU's version, 2");
    machine.buttons(snes::Ports{.one = snes::Buttons{}, .two = std::nullopt});
    ticks(2);
    check(joyWord(machine.read(places, &Places::pad)) == 0x0000, "JOY1 reads the pad released");

    // A routine placed from source on a machine of its own, and the cartridge's own copy called where it
    // sits, once the cartridge is parked. A binding for another CPU is refused.
    Vm::SNES routines;
    const Mix placed = placeMix(routines);
    check(placed(3, 5) == 4, "the placed routine averages 3 and 5 to 4");
    check(placed(255, 255) == 255, "and 255 and 255 to 255, the carry rotated back in");
    const std::uint8_t parkedX = machine.read(places, &Places::sprite).at(0);
    machine.stop();
    check(bound(3, 5) == 4, "the cartridge's own mix answers the same, called parked");
    check(machine.read(places, &Places::sprite).at(0) == parkedX, "and the cartridge stands where it was parked");

    // Parked wherever a tick of an uneven length leaves it — inside the frame handler or the idle loop,
    // often part-way through an instruction — the cartridge's own mix answers exactly every time, and
    // the cartridge carries on afterwards as if it had never been called.
    constexpr int kParkedCalls = 240;
    int           exact        = 0;
    for (int i = 0; i < kParkedCalls; ++i) {
        machine.run(Vm::Advance::OnTick);
        machine.advanceTick(std::chrono::nanoseconds{500'000 + (i * 1'234'567) % 16'000'000});
        machine.stop();
        const auto a = static_cast<std::uint8_t>(i * 37);
        if (bound(a, 200) == mixOf(a, 200)) ++exact;
    }
    std::printf("  parked at %d points of the frame: %d answered exactly\n", kParkedCalls, exact);
    check(exact == kParkedCalls, "the cartridge's own mix answers exactly wherever it was parked");
    machine.run(Vm::Advance::OnTick);
    machine.buttons(snes::Ports{.one = snes::Buttons{.right = true}, .two = std::nullopt});
    ticks(2);
    const std::uint8_t resumed = machine.read(places, &Places::sprite).at(0);
    ticks(1);
    check(static_cast<std::uint8_t>(machine.read(places, &Places::sprite).at(0) - resumed) == 4,
          "and the cartridge still moves the sprite by the step a frame");
    bool refused = false;
    try {
        const std::vector<std::uint8_t> ret{0x60};
        (void)routines.uploadRoutine<void()>(ret, {.isa = Isa::Sm83});
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "a binding for the SM83 is refused on this machine");

    // The escapes and watches, each switched on the running cartridge. A switch lands at the next step,
    // so each one is given a tick before what it does is measured. Right is still held.
    const auto spriteX = [&] { return machine.read(places, &Places::sprite).at(0); };
    const auto spriteY = [&] { return machine.read(places, &Places::sprite).at(1); };
    const int  heard   = hooks.reports;
    ticks(3);
    check(hooks.reports - heard == 3, "the escape at report hears one frame a tick");
    machine.escapes()["report"].armed(false);
    ticks(1);
    const int silenced = hooks.reports;
    ticks(3);
    check(hooks.reports == silenced, "and nothing once it is switched off");

    // The native pace moves the sprite mix(4, 8) = 6 a frame, whichever way the cartridge asked.
    const std::uint8_t stride = mixOf(4, kPaceWith);
    machine.escapes()["pace"].armed(true);
    ticks(1);
    std::uint8_t x1 = spriteX();
    ticks(1);
    std::uint8_t x2 = spriteX();
    std::printf("  pace answered natively: X went %u -> %u in one tick with Right held\n",
                static_cast<unsigned>(x1), static_cast<unsigned>(x2));
    check(static_cast<std::uint8_t>(x2 - x1) == stride,
          "pace answered by the cartridge's own mix, called inside the escape: 6 a frame to the right");
    machine.buttons(snes::Ports{.one = snes::Buttons{.left = true}, .two = std::nullopt});
    ticks(2);  // the pad is read at the frame's vertical blank
    x1 = spriteX();
    ticks(1);
    x2 = spriteX();
    check(static_cast<std::uint8_t>(x1 - x2) == stride, "and 6 a frame to the left");
    check(hooks.paced > 0, "and the escape answered instead of the routine");
    machine.escapes()["pace"].armed(false);
    machine.buttons(snes::Ports{.one = snes::Buttons{.right = true}, .two = std::nullopt});
    ticks(2);

    machine.watches()["step"].armed(true);
    ticks(1);
    x1 = spriteX();
    ticks(1);
    x2 = spriteX();
    check(static_cast<std::uint8_t>(x2 - x1) == kStepAnswer, "a read of the step byte answered 3 moves the sprite 3");
    check(machine.read(places, &Places::step).at(0) == 4, "while the image still holds 4");
    machine.watches()["step"].armed(false);

    machine.buttons(snes::Ports{.one = snes::Buttons{.down = true}, .two = std::nullopt});
    hooks.yMode = 1;
    machine.watches()["y"].armed(true);
    ticks(1);
    const std::uint8_t heldY = spriteY();
    ticks(3);
    check(spriteY() == heldY && hooks.yDecided > 0, "the sprite's Y holds with Down held while every store is vetoed");
    hooks.yMode = 2;
    ticks(60);
    std::printf("  corralled: Y %u after 60 ticks of Down\n", static_cast<unsigned>(spriteY()));
    check(spriteY() == kCorralBottom, "and stops at 160 while every store is held inside the band");
    machine.watches()["y"].armed(false);
    machine.buttons(snes::Ports{.one = snes::Buttons{}, .two = std::nullopt});

    std::printf("\ndone%s\n", failures == 0 ? "" : " — with failures");
    return failures == 0 ? 0 : 1;
}

// ── The panel ───────────────────────────────────────────────────────────────────────────────────

enum class Action : ActionId {
    ColorOne = 20, ColorTwo, Left32, Right32, StepDown, StepUp, Park, MixPlaced, MixBound,
    PaceNative, ReportEscape, WatchY, WatchStep,
};

// The font sheet carries digits, then letters, then a blank. Anything else lands on the blank.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    return 36;
}

// A byte or a word in hexadecimal, as many digits as asked for.
[[nodiscard]] std::string hex(unsigned value, int digits) {
    std::string s(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i, value >>= 4) {
        s[static_cast<std::size_t>(i)] = "0123456789ABCDEF"[value & 0xF];
    }
    return s;
}

// The buttons a JOY1 word holds, by name, in the word's own order from the top bit.
[[nodiscard]] std::string heldNames(std::uint16_t joy) {
    constexpr std::array<std::string_view, 12> kNames{"B", "Y", "SELECT", "START", "UP", "DOWN",
                                                      "LEFT", "RIGHT", "A", "X", "L", "R"};
    std::string said;
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (joy & (0x8000u >> i)) {
            said += (said.empty() ? "" : " ") + std::string{kNames[i]};
        }
    }
    return said.empty() ? "NOTHING HELD" : said;
}

// A palette word as 8-bit red, green and blue: each five-bit channel widened by repeating its top bits.
void paintWord(std::uint8_t* px, std::uint16_t word) {
    const auto widen = [](unsigned c) { return static_cast<std::uint8_t>((c << 3) | (c >> 2)); };
    px[0] = widen(word & 0x1F);
    px[1] = widen((word >> 5) & 0x1F);
    px[2] = widen((word >> 10) & 0x1F);
    px[3] = 255;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--verify") == 0) {
        return runVerify();
    }

    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "SnesCoexecution"},
        .window   = {.title = "Polyrhythm — SNES co-execution"},
        .viewport = ViewportResolution{kViewW, kViewH},
        .timing   = TimingProfile::Snes,
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::ColorOne, {SDL_SCANCODE_1}},
        {Action::ColorTwo, {SDL_SCANCODE_2}},
        {Action::Left32, {SDL_SCANCODE_3}},
        {Action::Right32, {SDL_SCANCODE_4}},
        {Action::StepDown, {SDL_SCANCODE_5}},
        {Action::StepUp, {SDL_SCANCODE_6}},
        {Action::Park, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::MixPlaced, {SDL_SCANCODE_7}},
        {Action::MixBound, {SDL_SCANCODE_8}},
        {Action::PaceNative, {SDL_SCANCODE_P}},  // not A: the directional preset binds WASD to the pad
        {Action::ReportEscape, {SDL_SCANCODE_X}},
        {Action::WatchY, {SDL_SCANCODE_H}},
        {Action::WatchStep, {SDL_SCANCODE_L}},
    };
    map.add(presets::directional(snes::Button::Up, snes::Button::Down, snes::Button::Left,
                                 snes::Button::Right));
    platform.actions(map);

    // ── The panel's font and its three palettes ──────────────────────────────────────────────────
    const AtlasManifest font = renderer.loadAtlas(
        "examples/snes/coexecution/assets/art/font.png", AssetDimensions{kGlyphPx, kGlyphPx},
        ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64, TransparentIndices::of({0}), 0,
        AssetPolicy::Embed);
    const PaletteId palText = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/font.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palLive = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/font_pick.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palDim = renderer.loadPaletteImage(
        "examples/snes/coexecution/assets/palettes/mono.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);

    // A glyph is 16px and the grid is 8px tiles, so each character stamps a 2x2 block.
    constexpr int         kMonW = kCols * 2, kMonH = kRows * 2;
    std::vector<TileCell> text(static_cast<std::size_t>(kMonW) * kMonH,
                               TileCell{.atlas = font.atlasId, .tile = 0, .palette = palDim});
    const auto clearText = [&] {
        for (TileCell& c : text) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palDim;
        }
    };
    const auto put = [&](int col, int row, std::string_view s, PaletteId pal) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kCols || row < 0 || row >= kRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(s[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = text[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile      = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette   = pal;
                }
        }
    };

    // ── The machine ──────────────────────────────────────────────────────────────────────────────
    // Host the image and name the places before it runs; from run() on, the declared places are what
    // this program reads and writes, each read answered by the machine's latest completed step.
    Hooks    hooks;
    Vm::SNES machine{VmConfig{.video = true}};
    machine.hostRom(examples::snes::demoCartridge());
    machine.video(true);
    const auto places = declarePlaces(machine);
    // The cartridge's own `mix`, bound where it sits — declared before the machine runs, called while
    // it is parked, and from inside the escape at `pace` while it runs.
    const Mix bound = machine.bindRoutine<std::uint8_t(std::uint8_t, std::uint16_t)>(kMixEntry, kMixBinding);
    declareHooks(machine, bound, hooks);
    machine.run(Vm::Advance::Continuously);
    bool paceNative = false, reportOn = true, stepAnswered = false;
    bool running = true;

    // The second machine: no cartridge, one routine placed from source.
    Vm::SNES  routines;
    const Mix placed = placeMix(routines);

    // What the panel shows, sampled once a tick.
    std::vector<std::uint8_t>              palette(snes::Palette.size, 0);
    std::array<std::vector<std::uint8_t>, kTileCount> tiles;
    std::uint8_t spriteX = 0, spriteY = 0, step = 1;
    std::uint16_t joy = 0;                   // JOY1, as the console latched it
    std::uint8_t  rdnmi = 0;                 // RDNMI, as it stands
    std::uint8_t mixA = 32;                  // the argument the next call takes; X is fixed at 200
    std::string  mixSaid = "";               // the last call, as the panel shows it
    int          boundCalls = 0, boundExact = 0;  // the cartridge's own mix, called parked
    std::string  status = "THE PANEL IS ITS OWN MEMORY";

    loop.simTick([&](const InputState& in) {
        machine.buttons(snes::Ports{.one = snes::held(in), .two = std::nullopt});

        // A write to palette word 129, through the whole palette the machine last published.
        const auto writeColor = [&](std::uint16_t color, const char* said) {
            std::vector<std::uint8_t> whole = machine.read(places, &Places::palette);
            putWord(whole, kColorWord, color);
            machine.write(places, &Places::palette, whole);
            status = said;
        };
        if (in.justPressed(Action::ColorOne)) writeColor(0x7C00, "PALETTE WORD 129 WRITTEN BLUE");
        if (in.justPressed(Action::ColorTwo)) writeColor(0x03FF, "PALETTE WORD 129 WRITTEN YELLOW");

        // The sprite's X is a byte of work RAM the cartridge moves every frame; this program moves it too.
        const int nudge = (in.justPressed(Action::Right32) ? 32 : 0) - (in.justPressed(Action::Left32) ? 32 : 0);
        if (nudge != 0) {
            std::vector<std::uint8_t> xy = machine.read(places, &Places::sprite);
            xy[0] = static_cast<std::uint8_t>(xy[0] + nudge);
            machine.write(places, &Places::sprite, xy);
            status = "ITS X WRITTEN IN WORK RAM";
        }

        // The step is a byte of the image itself: the frame handler reads it every frame.
        const int asked = step + (in.justPressed(Action::StepUp) ? 1 : 0) - (in.justPressed(Action::StepDown) ? 1 : 0);
        if (asked != step && asked >= 1 && asked <= 8) {
            machine.write(places, &Places::step, std::vector<std::uint8_t>{static_cast<std::uint8_t>(asked)});
            status = "THE STEP PATCHED IN THE IMAGE";
        }

        if (in.justPressed(Action::Park)) {
            if (running) {
                machine.stop();
                status = "PARKED WITH EVERY BYTE KEPT";
            } else {
                machine.run(Vm::Advance::Continuously);
                status = "RESUMED FROM WHERE IT STOOD";
            }
            running = !running;
        }

        // A routine called for a value, and the value written to the sprite's color. The placed one
        // answers on its own machine any time; the cartridge's own answers in the cartridge's context,
        // which is the cartridge's to give only while it is parked.
        const auto callMix = [&](const Mix& mix, const char* which, const char* said) {
            const std::uint8_t result = mix(mixA, 200);
            writeColor(grayWord(result), said);
            mixSaid = "MIX " + std::to_string(mixA) + " 200 = " + std::to_string(result) + "  " + which;
            const bool isExact = result == mixOf(mixA, 200);
            mixA = static_cast<std::uint8_t>(mixA + 32);
            return isExact;
        };
        if (in.justPressed(Action::MixPlaced)) callMix(placed, "PLACED", "THE PLACED ROUTINE SET THE COLOR");
        if (in.justPressed(Action::MixBound)) {
            if (running) {
                status = "PARK IT FIRST  SPACE";
            } else {
                // Parked wherever its clock stopped; the call lands at the next instruction boundary.
                ++boundCalls;
                if (callMix(bound, "BOUND", "THE CARTRIDGES OWN MIX SET THE COLOR")) ++boundExact;
            }
        }

        // The escapes and watches: a switch crosses to the machine's thread and lands at its next step.
        if (in.justPressed(Action::PaceNative)) {
            paceNative = !paceNative;
            machine.escapes()["pace"].armed(paceNative);
            status = paceNative ? "PACE ANSWERED BY MIX" : "PACE IS THE CARTRIDGES OWN";
        }
        if (in.justPressed(Action::ReportEscape)) {
            reportOn = !reportOn;
            machine.escapes()["report"].armed(reportOn);
            status = reportOn ? "THE ESCAPE AT REPORT IS ON" : "THE ESCAPE AT REPORT IS OFF";
        }
        if (in.justPressed(Action::WatchY)) {
            const int mode = (hooks.yMode + 1) % 3;
            hooks.yMode    = mode;
            machine.watches()["y"].armed(mode != 0);
            status = mode == 0 ? "ITS Y IS FREE" : mode == 1 ? "EVERY STORE TO ITS Y VETOED" : "ITS Y HELD FROM 64 TO 160";
        }
        if (in.justPressed(Action::WatchStep)) {
            stepAnswered = !stepAnswered;
            machine.watches()["step"].armed(stepAnswered);
            status = stepAnswered ? "EVERY READ OF THE STEP IS 3" : "THE STEP READS THE IMAGE";
        }

        palette = machine.read(places, &Places::palette);
        for (std::uint32_t t = 0; t < kTileCount; ++t) {
            tiles[t] = machine.read(places, &Places::tiles, t);
        }
        const std::vector<std::uint8_t> xy = machine.read(places, &Places::sprite);
        spriteX = xy[0];
        spriteY = xy[1];
        step    = machine.read(places, &Places::step).at(0);
        joy     = joyWord(machine.read(places, &Places::pad));
        rdnmi   = machine.read(places, &Places::nmi).at(0);
    });

    // The panel's pictures — the swatches and the decoded tiles — are this program's own raster, painted
    // on the CPU from the bytes read above.
    std::vector<std::uint8_t> panelPixels(static_cast<std::size_t>(kPanelW) * kViewH * 4);
    std::uint64_t             panelGeneration = 0;
    const auto paintPanel = [&] {
        for (std::size_t i = 0; i < panelPixels.size(); i += 4) {
            panelPixels[i] = 16;  panelPixels[i + 1] = 16;  panelPixels[i + 2] = 24;  panelPixels[i + 3] = 255;
        }
        const auto pixel = [&](int x, int y) { return &panelPixels[(static_cast<std::size_t>(y) * kPanelW + x) * 4]; };

        // The 256 palette words, sixteen to a row.
        for (std::size_t w = 0; w < 256; ++w) {
            const int x0 = kSwatchX + static_cast<int>(w % 16) * kSwatchPx;
            const int y0 = kSwatchY + static_cast<int>(w / 16) * kSwatchPx;
            for (int y = 0; y < kSwatchPx - 1; ++y)
                for (int x = 0; x < kSwatchPx - 1; ++x) paintWord(pixel(x0 + x, y0 + y), wordAt(palette, w));
        }

        // Sixteen 4 bpp tiles in a row. A tile's row y is planes 0 and 1 at bytes 2y and 2y+1, planes 2 and
        // 3 at bytes 16+2y and 17+2y, the leftmost pixel in each byte's top bit; its color is object palette
        // 0's, where the sprite reads it — so 1 and 2 recolor these too. Index 0 is left as the panel.
        for (std::uint32_t t = 0; t < kTileCount; ++t) {
            const std::vector<std::uint8_t>& b = tiles[t];
            if (b.size() != 32) continue;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int bit   = 7 - x;
                    const int index = ((b[2 * y] >> bit) & 1) | (((b[2 * y + 1] >> bit) & 1) << 1) |
                                      (((b[16 + 2 * y] >> bit) & 1) << 2) | (((b[17 + 2 * y] >> bit) & 1) << 3);
                    if (index == 0) continue;
                    for (int sy = 0; sy < kTileScale; ++sy)
                        for (int sx = 0; sx < kTileScale; ++sx)
                            paintWord(pixel(kStripX + (static_cast<int>(t) * 8 + x) * kTileScale + sx,
                                            kStripY + y * kTileScale + sy),
                                      wordAt(palette, 128 + static_cast<std::size_t>(index)));
                }
        }
        ++panelGeneration;
    };

    FrameDrawState frame;
    loop.renderLoop([&]() {
        clearText();
        const auto heading = [&](int row, std::string_view name, std::string_view keys) {
            put(kHeadCol, row, name, palText);
            put(kKeyCol, row, keys, palLive);
        };
        const auto row = [&](int at, std::string_view name, std::string_view value) {
            put(kNameCol, at, name, palDim);
            put(kValueCol, at, value, palText);
        };

        put(kHeadCol, 1, "SNES COEXECUTION", palText);
        put(kHeadCol, 2, "ITS MEMORY READ AS IT RUNS", palDim);
        heading(4, "PALETTE", "1 2 WORD 129");

        // Beside the swatches: the escapes and watches, each with its key, what it is doing, and a count.
        const auto hook = [&](int at, std::string_view key, std::string_view name, std::string_view state, int count) {
            put(kHookCol, at, key, palLive);
            put(kHookCol + 2, at, name, palDim);
            put(kHookCol + 9, at, state, palText);
            put(kHookCol + 16, at, std::to_string(count % 100'000), palText);
        };
        put(kHookCol, 6, "ESCAPES", palText);
        hook(7, "P", "PACE", paceNative ? "MIX" : "OWN", hooks.paced);
        hook(8, "X", "REPORT", reportOn ? "ON" : "OFF", hooks.reports);
        put(kHookCol, 10, "WATCHES", palText);
        const int yMode = hooks.yMode;
        hook(11, "H", "Y", yMode == 0 ? "FREE" : yMode == 1 ? "VETO" : "64 160", hooks.yDecided);
        hook(12, "L", "STEP", stepAnswered ? "AS 3" : "IMAGE", hooks.stepAnswered);
        heading(14, "VIDEO RAM TILES 0 TO 15", "");
        heading(17, "THE SPRITE", "ARROWS 3 4");
        row(18, "AT", "X " + std::to_string(spriteX) + "  Y " + std::to_string(spriteY));
        heading(19, "THE STEP", "5 6");
        row(20, "STEP", std::to_string(step) + " A FRAME IN THE IMAGE");
        heading(21, "PAD REGISTERS", "ARROWS");
        row(22, "4218", hex(joy, 4) + "  " + heldNames(joy));
        row(23, "4210", hex(rdnmi, 2) + (rdnmi & 0x80 ? "  NMI FLAG SET" : "  NMI FLAG CLEAR"));
        heading(24, "THE MACHINE", "SPACE");
        row(25, "IT IS", running ? "RUNNING" : "PARKED");
        heading(26, "MIX A X", "7 PLACED 8 BOUND");
        put(kNameCol, 27, kMixSource[0], palDim);
        put(kNameCol, 28, kMixSource[1], palDim);
        put(kNameCol, 29, mixSaid, palText);
        row(30, "BOUND", std::to_string(boundCalls) + " CALLS  " + std::to_string(boundExact) + " EXACT");
        put(kHeadCol, 31, status, palLive);

        paintPanel();

        frame.layers.clear();

        // The cartridge's picture, fitted to a slot the size of the console's largest picture.
        RasterContent picture = machine.video();
        picture.fit           = PixelSize{kSlotW, kSlotH};
        DrawLayer screen{.key = "cartridge"};
        screen.z       = 0;
        screen.size    = PixelSize{kViewW, kViewH};
        screen.scroll  = LayerScroll{0, -kSlotTop};
        screen.content = picture;
        frame.layers.push_back(screen);

        DrawLayer panel{.key = "panel"};
        panel.z       = 1;
        panel.size    = PixelSize{kViewW, kViewH};
        panel.scroll  = LayerScroll{-kPanelX, 0};
        panel.content = RasterContent{.pixels     = panelPixels,
                                      .width      = kPanelW,
                                      .height     = kViewH,
                                      .format     = RasterPixelFormat::Rgba8888,
                                      .generation = panelGeneration};
        frame.layers.push_back(panel);

        DrawLayer words{.key = "text"};
        words.z       = 2;
        words.size    = PixelSize{kViewW, kViewH};
        words.content = TileContent{.widthInTiles  = kMonW,
                                    .heightInTiles = kMonH,
                                    .cells         = std::span<const TileCell>(text),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(words);

        renderer.renderFrame(frame);
    });

    std::printf(
        "SNES co-execution — the demo cartridge on the left, its own memory on the right, read every tick\n"
        "through declared places. Arrows move the sprite. 1 and 2 write its palette word, 3 and 4 write its\n"
        "X in work RAM, 5 and 6 patch the step byte in the image while it runs, SPACE parks and resumes.\n"
        "The pad registers are read the same way, as they stand. 7 calls a routine placed from source on a\n"
        "machine of its own; 8 calls the cartridge's own copy, bound where it sits, while the cartridge is\n"
        "parked, and the panel counts how many of those calls answered exactly. Either call writes its\n"
        "answer to the sprite's color. P answers the cartridge's own pace routine natively, with its mix\n"
        "called from inside the escape; X switches the escape at report; H cycles the watch on the\n"
        "sprite's Y through free, vetoed and held; L answers every read of the step byte with 3.\n\n");

    WindowedHost host{loop, platform};
    host.run();

    machine.stop();
    return 0;
}
