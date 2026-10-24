// Co-execution demo — a cartridge running its own world, with native code woven into it.
//
// The eight tracks are the cartridge's own: it keeps eight walkers in its work RAM and marches
// every one of them each frame, on its own thread, at the platform's own speed. The cartridge moves
// them; this program watches, and joins in. Every key is one way it can:
//
//   A     answers the cartridge's pace routine with a C++ function INSTEAD. The walkers scatter,
//         because that function asks the cartridge's OWN generator for each walker's pace — a
//         native call nested inside the escape, into the guest, while the guest is mid-frame.
//         Press it again and the cartridge's own rule is back exactly as it was.
//   UP    write a byte INTO the cartridge image while it runs. Its own rule reads that byte every
//   DOWN  frame, so the whole column marches faster or slower on the next step.
//   RIGHT run the machine at a fraction or a multiple of the platform's own speed, or paused.
//   LEFT
//   SPACE park the machine where it stands, and resume from there rather than from a fresh boot.
//   X     drop the observing escape's declaration. Its count stops; the world carries on, because
//         the escape was watching the guest, never driving it.
//   P     park, call the cartridge's OWN decompressor and decode its OWN pointer table, and resume
//         — content the running loop never reaches, obtained without playing the game to it.
//   S     put the generator's seed back where the cartridge sets it, through a place built on the
//         spot rather than declared.
//   H     decide the cartridge's own WRITES to its walkers. Cycles free, held and corralled: held
//         vetoes every store, so the column freezes where it stands while the cartridge marches on
//         believing it moved them; corralled answers with a byte of this program's own, so they
//         pack into the left of their tracks. One watch, branching per access.
//   L     answer the cartridge's own READ of its march step with a byte the image does not hold.
//         The panel keeps showing the stored byte, and the column marches by the answered one.
//
// The cartridge is authored right here and assembled by the engine's own SM83 assembler, so nothing
// comes from anyone's ROM. Its own picture is not what is drawn: a hosted machine's framebuffer is
// a separate surface. What the renderer draws is the guest's own memory, read live.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "retropp/clock.h"
#include "retropp/draw_state.h"
#include "retropp/engine_config.h"
#include "retropp/gb.h"             // gb::A / gb::Hram / gb::banked
#include "retropp/geometry.h"
#include "retropp/guest_escape.h"   // GuestEscape — where control leaves the guest
#include "retropp/input.h"
#include "retropp/input_actions.h"
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/palette.h"
#include "retropp/renderer.h"
#include "retropp/run_loop.h"
#include "retropp/sdl_platform.h"
#include "retropp/vm.h"             // Vm — hosting, running, reading, escaping, calling
#include "retropp/windowed_host.h"

using namespace retropp;

namespace {

// ── The panel ───────────────────────────────────────────────────────────────────────────────────

constexpr int kTilePx = 8;
constexpr int kGlyphPx = 16, kGlyphStride = 128 / kTilePx;  // font.png is 128 wide
constexpr int kCols = 64, kRows = 32;
constexpr int kViewW = kCols * kGlyphPx, kViewH = kRows * kGlyphPx;

constexpr int kNameCol  = 4;   // where a row's label starts, indented under its heading
constexpr int kValueCol = 22;  // where every row's value starts
constexpr int kKeyCol   = 40;  // where the key that changes a heading's rows sits
constexpr int kTrack    = 32;  // cells in a walker's track, which is the range its walk wraps in
constexpr int kTrackRow = 6;   // the first of the eight

// ── The cartridge ───────────────────────────────────────────────────────────────────────────────

// Where the cartridge's pieces live. Each block is assembled on its own and placed here, so the
// loop's `call` and this program's declarations name an address both sides agree on. Each address
// is also the limit of the block before it, and authoring checks that — a block that grows into its
// neighbor puts the guest's own `call` in the middle of somebody else's code.
constexpr std::uint32_t kProgram  = 0x0150;  // the game loop
constexpr std::uint32_t kPaceRule = 0x0200;  // the cartridge's own rule for a walker's pace
constexpr std::uint32_t kRandom   = 0x0220;  // the cartridge's own generator
constexpr std::uint32_t kReport   = 0x0240;  // what the loop calls at the end of every frame
constexpr std::uint32_t kCodeEnd  = 0x0260;  // the limit of the last block in that run
constexpr std::uint32_t kDecoder  = 0x1000;  // the cartridge's own decompressor
constexpr std::uint32_t kStepByte = 0x1200;  // the march step, in the cartridge's own ROM
constexpr std::uint32_t kPointers = 0x1300;  // the cartridge's own pointer table, 2 bytes an entry
constexpr std::uint32_t kPacked   = 0x1400;  // the packed table only the decompressor understands
constexpr std::uint32_t kStrings  = 0x1500;  // length-prefixed text the pointers point at
constexpr std::uint32_t kBankTwo  = 2 * 0x4000;  // bank 2, which the guest's loop never maps

constexpr std::uint32_t kWalkers  = 0xC010;  // the eight the loop marches, one byte each
constexpr std::uint32_t kUnpacked = 0xC020;  // where the decompressor leaves its work

constexpr std::uint32_t kFrames = 0xFF80;  // the guest's own frame counter, in high RAM
constexpr std::uint32_t kSeed   = 0xFF81;  // the generator's seed, where the cartridge keeps it

constexpr std::uint32_t kWalkerCount = 8;
constexpr std::uint8_t  kStartSeed   = 0x07;
constexpr std::uint8_t  kStartStep   = 0x01;

// The loop: park every walker at the left, then every frame advance the counter, ask the pace rule
// what each walker moves by and move it, and call the end-of-frame routine.
constexpr std::string_view kGameSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ld a, $07
    ldh [$FF81], a      ; the generator's seed, as the cartridge starts it
    ld hl, $C010
    ld c, $08
clear:
    ld [hl], $00        ; every walker starts at the left of its track
    inc hl
    dec c
    jr nz, clear
    ei
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ld hl, $C010
    ld c, $00
walk:
    ld a, c
    call $0200          ; the pace rule: which walker in A, what it moves by back in A
    ld b, a
    ld a, [hl]
    add a, b
    and $1F             ; a track is 32 cells wide, and a walk wraps in it
    ld [hl], a
    inc hl
    inc c
    ld a, c
    cp $08
    jr nz, walk
    call $0240          ; the cartridge's own end-of-frame routine
    jr loop
)";

// The cartridge's own pace rule, and its convention: which walker in A, what it moves by back in A.
// The cartridge marches them together, by a step it keeps in its own ROM, so they hold a column.
constexpr std::string_view kPaceSource = R"(
    ld a, [$1200]
    ret
)";

// The generator, and its convention: no inputs, the number in A. Its rule is seed * 5 + 13, which
// walks all 256 values before it repeats, over a seed the cartridge keeps in its own high RAM.
constexpr std::string_view kRandomSource = R"(
    ldh a, [$FF81]
    ld b, a
    add a, a            ; twice
    add a, a            ; four times
    add a, b            ; five times
    add a, $0D          ; plus thirteen
    ldh [$FF81], a
    ret
)";

// What the loop calls at the end of every frame. The cartridge does nothing here; the observing
// escape declared at it is what makes the call worth something to this program.
constexpr std::string_view kReportSource = R"(
    ret
)";

// The decompressor: run-length pairs at $1400 — a count and a value, ending at a count of zero —
// expanded byte by byte to $C020. The running loop never calls it.
constexpr std::string_view kDecoderSource = R"(
    ld hl, $1400
    ld de, $C020
next:
    ld a, [hl+]
    or a
    jr z, finished
    ld b, a             ; how many
    ld a, [hl+]
    ld c, a             ; of what
run:
    ld a, c
    ld [de], a
    inc de
    dec b
    jr nz, run
    jr next
finished:
    ret
)";

// The packed table, as the cartridge stores it: three of $AA, two of $BB, four of $CC.
constexpr std::uint8_t  kPackedTable[] = {3, 0xAA, 2, 0xBB, 4, 0xCC, 0};
constexpr std::uint32_t kUnpackedSize  = 9;

// The table in bank 2, which nothing the guest runs ever reads.
constexpr std::uint8_t kBankTable[] = {0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3};

// The cartridge's own text, stored as a length byte followed by that many bytes, at addresses the
// pointer table holds and nothing else records.
constexpr std::string_view kTexts[] = {
    "PULLED FROM A POINTER TABLE",
    "THE GAMES LOOP NEVER RAN HERE",
    "EACH ENTRY CARRIES ITS LENGTH",
    "AND THE ENGINE READ IT PARKED",
};
constexpr std::uint32_t kTextStride = 0x40;  // room for the longest of them, and its length byte

// The places this program names in the machine. Never instantiated — the struct exists so the
// places have names, and naming one wrong is a compile error rather than a bad address.
struct Places {
    MemoryRegion hram;      // the machine's own high RAM, named by the console's own header
    MemoryRegion walkers;   // the eight the cartridge marches
    MemoryRegion step;      // the march step, inside the cartridge image itself
    MemoryRegion pointers;  // the cartridge's own pointer table
    MemoryRegion bankTwo;   // a table in a bank the loop never maps
};

// One authored cartridge: 64 KiB behind an MBC1, so bank 2 is a real place to name. The entry jumps
// to the loop, the VBlank vector returns, each routine sits where its callers name it, and the
// tables sit where only this program and the decompressor look.
std::vector<std::uint8_t> authorCartridge(Vm& assembler) {
    std::vector<std::uint8_t> rom(0x10000, 0x00);
    rom[0x0143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x0147] = 0x01;  // MBC1
    rom[0x0148] = 0x01;  // 64 KiB
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;  // jp $0150

    const auto place = [&](std::uint32_t at, std::uint32_t limit, std::string_view source) {
        const std::vector<std::uint8_t> code = assembler.assemble(std::string(source));
        if (at + code.size() > limit) {
            throw std::length_error("the block at " + std::to_string(at) + " assembles to " +
                                    std::to_string(code.size()) + " bytes and runs past " +
                                    std::to_string(limit));
        }
        std::copy(code.begin(), code.end(), rom.begin() + at);
    };
    place(kProgram, kPaceRule, kGameSource);
    place(kPaceRule, kRandom, kPaceSource);
    place(kRandom, kReport, kRandomSource);
    place(kReport, kCodeEnd, kReportSource);
    place(kDecoder, kStepByte, kDecoderSource);

    rom[kStepByte] = kStartStep;
    std::copy(std::begin(kPackedTable), std::end(kPackedTable), rom.begin() + kPacked);
    std::copy(std::begin(kBankTable), std::end(kBankTable), rom.begin() + kBankTwo);

    for (std::size_t i = 0; i < std::size(kTexts); ++i) {
        const std::uint32_t at = kStrings + static_cast<std::uint32_t>(i) * kTextStride;
        rom[kPointers + i * 2]     = static_cast<std::uint8_t>(at & 0xFF);
        rom[kPointers + i * 2 + 1] = static_cast<std::uint8_t>(at >> 8);
        rom[at]                    = static_cast<std::uint8_t>(kTexts[i].size());
        std::copy(kTexts[i].begin(), kTexts[i].end(), rom.begin() + at + 1);
    }
    return rom;
}

// ── Reading the panel ───────────────────────────────────────────────────────────────────────────

enum class Action : std::uint8_t {
    Faster, Slower, LongerStep, ShorterStep, Park, Answer, Forget, Pull, Reseed, Hold, Lie,
    Fullscreen
};

// What this program does with the cartridge's own stores to its walkers, decided inside one
// handler as each store arrives rather than by arming and disarming a watch per mode.
enum class WalkerRule : std::uint8_t { Free, Held, Corralled };
constexpr std::array<std::string_view, 3> kWalkerRuleNames{
    {"THE CARTRIDGES OWN", "HELD WHERE THEY STAND", "CORRALLED TO THE LEFT"}};

// How fast the machine runs, as a fraction of the platform's own, and what to call that on a panel
// whose font carries letters and digits and nothing else.
struct Fraction {
    std::uint32_t    num;
    std::uint32_t    den;
    std::string_view name;
};
constexpr std::array<Fraction, 6> kSpeeds{{{.num = 0, .den = 1, .name = "PAUSED"},
                                           {.num = 1, .den = 4, .name = "A QUARTER"},
                                           {.num = 1, .den = 2, .name = "HALF"},
                                           {.num = 1, .den = 1, .name = "HARDWARE"},
                                           {.num = 2, .den = 1, .name = "DOUBLE"},
                                           {.num = 4, .den = 1, .name = "FOUR TIMES"}}};

// The font sheet carries digits, then letters, then a blank. Anything else lands on the blank.
[[nodiscard]] std::size_t glyphCell(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<std::size_t>(ch - '0');
    if (ch >= 'A' && ch <= 'Z') return static_cast<std::size_t>(10 + (ch - 'A'));
    if (ch >= 'a' && ch <= 'z') return static_cast<std::size_t>(10 + (ch - 'a'));
    return 36;
}

// `n` in `width` columns, zero-filled, so a rolling digit never shifts its row.
[[nodiscard]] std::string pad(std::uint64_t n, int width) {
    std::string s = std::to_string(n);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string           out;
    for (const std::uint8_t b : bytes) {
        if (!out.empty()) out.push_back(' ');
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

}  // namespace

int main() {
    const EngineConfig config{
        .identity = {.organization = "Retro++", .application = "Coexecution"},
        .window   = {.title = "Polyrhythm — co-execution"},
        .viewport = ViewportResolution{kViewW, kViewH},
    };
    EngineConfig::setActive(config);

    SteadyClock clock;
    RunLoop     loop{clock};
    SdlPlatform platform;
    Renderer    renderer{platform.device(), platform.sdlWindow()};

    ActionMap map{
        {Action::Faster, {SDL_SCANCODE_RIGHT, PadButton::DpadRight}},
        {Action::Slower, {SDL_SCANCODE_LEFT, PadButton::DpadLeft}},
        {Action::LongerStep, {SDL_SCANCODE_UP, PadButton::DpadUp}},
        {Action::ShorterStep, {SDL_SCANCODE_DOWN, PadButton::DpadDown}},
        {Action::Park, {SDL_SCANCODE_SPACE, PadButton::FaceSouth}},
        {Action::Answer, {SDL_SCANCODE_A, PadButton::FaceWest}},
        {Action::Forget, {SDL_SCANCODE_X, PadButton::FaceEast}},
        {Action::Pull, {SDL_SCANCODE_P, PadButton::FaceNorth}},
        {Action::Reseed, {SDL_SCANCODE_S, PadButton::Start}},
        {Action::Hold, {SDL_SCANCODE_H, PadButton::ShoulderL}},
        {Action::Lie, {SDL_SCANCODE_L, PadButton::ShoulderR}},
        {Action::Fullscreen, {SDL_SCANCODE_F, PadButton::Select}},
    };
    platform.actions(map);

    // ── The panel's font and its three palettes ──────────────────────────────────────────────────
    const AtlasManifest font = renderer.loadAtlas(
        "examples/coexecution/assets/art/font.png", AssetDimensions{kGlyphPx, kGlyphPx},
        ContentKind::Tileset, ReadOrder::LeftRightThenDown, 64, TransparentIndices::of({0}), 0,
        AssetPolicy::Embed);
    const PaletteId palText = renderer.loadPaletteImage(
        "examples/coexecution/assets/palettes/font.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palLive = renderer.loadPaletteImage(
        "examples/coexecution/assets/palettes/font_pick.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);
    const PaletteId palDim = renderer.loadPaletteImage(
        "examples/coexecution/assets/palettes/mono.png", ReadOrder::LeftRightThenDown, 0,
        AssetPolicy::Embed);

    // A glyph is 16px and the grid is 8px tiles, so each character stamps a 2×2 block.
    constexpr int         kMonW = kCols * 2, kMonH = kRows * 2;
    std::vector<TileCell> mon(static_cast<std::size_t>(kMonW) * kMonH,
                              TileCell{.atlas = font.atlasId, .tile = 0, .palette = palDim});
    const auto clearMon = [&] {
        for (TileCell& c : mon) {
            c.tile    = static_cast<std::uint16_t>(font[36].tile);
            c.palette = palDim;
        }
    };
    const auto put = [&](int col, int row, std::string_view text, PaletteId pal) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const int gc = col + static_cast<int>(i);
            if (gc < 0 || gc >= kCols || row < 0 || row >= kRows) continue;
            const auto base = static_cast<std::uint16_t>(font[glyphCell(text[i])].tile);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    TileCell& c = mon[static_cast<std::size_t>(row * 2 + dy) * kMonW + (gc * 2 + dx)];
                    c.tile    = static_cast<std::uint16_t>(base + dx + dy * kGlyphStride);
                    c.palette = pal;
                }
        }
    };

    // ── The machine ──────────────────────────────────────────────────────────────────────────────
    // Host the image, name the places, bind the routines and declare the escapes — all before the
    // machine runs, because that is when those terms are settled. From run() on, the declared places
    // are the observable set and the declared escapes are where control can leave the guest.
    Vm::GBC machine;
    machine.hostRom(authorCartridge(machine));

    const auto places = machine.registerRegions(regions(
        region(&Places::hram, gb::Hram, "high ram"),
        region(&Places::walkers, MemoryRegion{.at = kWalkers, .size = kWalkerCount}, "walkers"),
        region(&Places::step, MemoryRegion{.at = kStepByte, .size = 1}, "march step"),
        region(&Places::pointers, MemoryRegion{.at = kPointers, .size = 2, .count = 4},
               "text pointers"),
        region(&Places::bankTwo, MemoryRegion{.at = gb::banked(2, 0x4000), .size = 1, .count = 8},
               "bank two table")));

    // The cartridge's own routines, bound where they already sit: the address IS the routine, and
    // each binding transcribes the convention that routine already has.
    auto random     = machine.bindRoutine<std::uint8_t()>(kRandom, RoutineBinding{.output = gb::A});
    auto decompress = machine.bindRoutine<void()>(kDecoder, RoutineBinding{});

    // Both escapes fire on the machine's own thread, so what they share with the panel is atomic.
    std::atomic<std::uint64_t> seen{0};   // frames the observing escape watched go by
    std::atomic<std::uint64_t> rolls{0};  // calls the native rule made into the generator

    machine.registerEscapes(escapes(
        // The OBSERVING kind: this runs, then the cartridge's own instruction runs as it was going
        // to. It watches the guest and never drives it, which is why dropping it stops the count
        // and leaves the world alone.
        GuestEscape{.key     = "frame mark",
                    .at      = kReport,
                    .handler = [&](Vm&, std::uint32_t) { seen.fetch_add(1); }},
        // The ANSWERING kind: this runs INSTEAD of the pace rule, speaking the convention that rule
        // already has — the loop leaves the walker's number in A and reads its pace back out of A —
        // and builds its answer by calling the cartridge's OWN generator, nested inside the escape,
        // on the machine's own thread and in the guest's own context. Declared switched off, so the
        // cartridge's own rule marches them until A arms it.
        GuestEscape{.key      = "pace",
                    .at       = kPaceRule,
                    .replaces = routine(RoutineBinding{.inputs = {gb::A}, .output = gb::A},
                                        [&](std::uint8_t which) -> std::uint8_t {
                                            const std::uint8_t roll = random();
                                            rolls.fetch_add(1);
                                            return static_cast<std::uint8_t>(
                                                1 + ((roll + which) & 0x03));
                                        }),
                    .armed    = false}));

    // Watches decide the guest's own ACCESSES, where an escape decides its instructions. Both fire
    // on the machine's own thread, so what they share with the panel is atomic.
    std::atomic<WalkerRule> walkerRule{WalkerRule::Free};
    std::atomic<std::uint64_t> vetoed{0};   // stores of the cartridge's this program refused
    std::atomic<std::uint64_t> answered{0}; // reads of its march step it answered

    machine.registerWatches(watches(
        // ONE watch over the whole eight-byte place, branching as each store arrives. Holding them
        // is a veto: the store never lands, so the walkers stay exactly where they are while the
        // cartridge marches on believing it moved them. Corralling them substitutes a byte of this
        // program's own, so they pack into the left of their tracks.
        GuestWatch{.key     = "walkers",
                   .at      = MemoryRegion{.at = kWalkers, .size = kWalkerCount},
                   .onWrite =
                       [&](Vm&, std::uint32_t, std::uint8_t value) {
                           switch (walkerRule.load()) {
                               case WalkerRule::Free:
                                   return AccessVerdict::proceed();
                               case WalkerRule::Held:
                                   vetoed.fetch_add(1);
                                   return AccessVerdict::veto();
                               case WalkerRule::Corralled:
                                   return AccessVerdict::instead(
                                       static_cast<std::uint8_t>(value % (kTrack / 4)));
                           }
                           return AccessVerdict::proceed();
                       }},
        // A READ answered with a byte the image does not hold. The cartridge's own rule reads its
        // march step every frame; while this is armed it reads a 3 the panel never shows, because
        // the panel reads the byte that is actually stored.
        GuestWatch{.key    = "march step",
                   .at     = MemoryRegion{.at = kStepByte, .size = 1},
                   .onRead =
                       [&](Vm&, std::uint32_t, std::uint8_t) {
                           answered.fetch_add(1);
                           return AccessVerdict::instead(3);
                       },
                   .armed = false}));

    // The bank 2 table is content, not state: it reads the same every time, so once is enough. A
    // banked place resolves in the machine's decoded space, so no bank has to be mapped to reach it.
    std::vector<std::uint8_t> bankTwo;
    for (std::uint32_t i = 0; i < std::size(kBankTable); ++i) {
        bankTwo.push_back(machine.read(places, &Places::bankTwo, i).at(0));
    }

    int  speedIndex = 3;  // {1,1}, the platform's own speed
    bool running    = false;
    bool lying      = false;  // whether the march-step read is being answered
    machine.speed(kSpeeds[speedIndex].num, kSpeeds[speedIndex].den);
    machine.run();
    running = true;

    // The parked work below genuinely needs the machine stopped: calling its own routines, and a
    // place built on the spot rather than declared. Switching an escape does not — that crosses to
    // the running machine on its own, as reading and writing a declared place and the speed factor
    // all do.
    const auto whileParked = [&](auto&& act) {
        const bool wasRunning = running;
        if (wasRunning) machine.stop();
        act();
        if (wasRunning) machine.run();
    };

    std::string status = "THE EIGHT TRACKS ARE THE CARTRIDGES OWN CODE RUNNING";
    std::vector<std::uint8_t> walkers(kWalkerCount, 0);
    std::vector<std::uint8_t> unpacked;
    std::string               pulledText;
    std::uint32_t             textIndex = 0;

    // What the guest is doing, sampled once a tick from the last completed step's publish.
    std::uint64_t frames = 0, lived = 0;
    std::uint8_t  seed = 0, step = kStartStep;
    bool          firstSample = true, answering = false, watching = true;
    int           wantedStep  = kStartStep;

    loop.simTick([&](const InputState& in) {
        if (in.justPressed(Action::Fullscreen))
            platform.window().fullscreen(!platform.window().fullscreen());

        if (in.justPressed(Action::Faster) && speedIndex + 1 < static_cast<int>(kSpeeds.size())) {
            ++speedIndex;
            machine.speed(kSpeeds[speedIndex].num, kSpeeds[speedIndex].den);
            status = "SPEED IS A FRACTION OF THE PLATFORMS OWN";
        }
        if (in.justPressed(Action::Slower) && speedIndex > 0) {
            --speedIndex;
            machine.speed(kSpeeds[speedIndex].num, kSpeeds[speedIndex].den);
            status = kSpeeds[speedIndex].num == 0 ? "A PAUSED MACHINE LIVES BUT STANDS STILL"
                                                  : "SPEED IS A FRACTION OF THE PLATFORMS OWN";
        }

        // A declared write, into the cartridge image itself: its own pace rule reads this byte out
        // of its own ROM every frame, so the column marches on at the next step boundary.
        const int asked = wantedStep + (in.justPressed(Action::LongerStep) ? 1 : 0) -
                          (in.justPressed(Action::ShorterStep) ? 1 : 0);
        if (asked != wantedStep && asked >= 0 && asked <= 7) {
            wantedStep = asked;
            machine.write(places, &Places::step,
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>(wantedStep)});
            status = "THE MARCH STEP IS A BYTE INSIDE THE CARTRIDGE ITSELF";
        }

        if (in.justPressed(Action::Park)) {
            if (running) {
                machine.stop();
                running = false;
                status  = "PARKED WHERE IT STOOD WITH EVERY BYTE KEPT";
            } else {
                machine.run();
                running = true;
                status  = "RESUMED FROM THE PARK NOT FROM A FRESH BOOT";
            }
        }

        // Switched while the machine runs. The change lands at its next step boundary, so the walkers
        // change how they move without the world stopping for it.
        if (in.justPressed(Action::Answer)) {
            answering = !answering;
            machine.escapes()["pace"].armed(answering);
            status = answering ? "A NATIVE RULE SETS EVERY WALKERS PACE NOW"
                               : "THE CARTRIDGES OWN RULE IS BACK AS IT WAS";
        }

        // The rule changes with no verb crossing anywhere: the watch stays armed and its handler
        // reads this on the machine's own thread, once per store. That is what deciding per ACCESS
        // rather than per declaration buys.
        if (in.justPressed(Action::Hold)) {
            const auto next = static_cast<WalkerRule>(
                (static_cast<std::uint8_t>(walkerRule.load()) + 1) % 3);
            walkerRule.store(next);
            status = next == WalkerRule::Free
                         ? "ITS OWN STORES LAND AGAIN AND THE COLUMN WALKS"
                     : next == WalkerRule::Held
                         ? "EVERY STORE REFUSED THE COLUMN CANNOT MOVE"
                         : "ITS STORES ANSWERED THE COLUMN PACKS LEFT";
        }

        // Arming a watch DOES cross, on the same terms an escape's switch does: it lands at the
        // machine's next step boundary.
        if (in.justPressed(Action::Lie)) {
            lying = !lying;
            machine.watches()["march step"].armed(lying);
            status = lying ? "ITS OWN STEP READS AS A THREE WHATEVER IS STORED"
                           : "IT READS THE STEP ITS IMAGE ACTUALLY HOLDS";
        }

        if (in.justPressed(Action::Forget) && watching) {
            machine.escapes()["frame mark"].remove();
            watching = false;
            status   = "THE ESCAPE IS GONE ITS COUNT STOPS THE WORLD DOES NOT";
        }

        // The parked work: the machine stops, this program calls the cartridge's own decompressor
        // on a table the loop never touches, reads what it produced through a place built on the
        // spot, then decodes the cartridge's own pointer table to reach a string by its address.
        if (in.justPressed(Action::Pull)) {
            whileParked([&] {
                decompress();
                unpacked = machine.read(MemoryRegion{.at = kUnpacked, .size = kUnpackedSize});

                const std::vector<std::uint8_t> pointer =
                    machine.read(places, &Places::pointers, textIndex);
                const auto at = static_cast<std::uint32_t>(
                    pointer[0] | (static_cast<std::uint32_t>(pointer[1]) << 8));
                const std::uint8_t length =
                    machine.read(MemoryRegion{.at = at, .size = 1}).at(0);
                const std::vector<std::uint8_t> text =
                    machine.read(MemoryRegion{.at = at + 1, .size = length});
                pulledText = std::string(text.begin(), text.end());
                textIndex  = (textIndex + 1) % static_cast<std::uint32_t>(std::size(kTexts));
                status     = "ITS OWN DECODER AND POINTER TABLE RAN WHILE PARKED";
            });
        }

        // A write to a place built on the spot, which the machine takes only while it is parked.
        if (in.justPressed(Action::Reseed)) {
            whileParked([&] {
                machine.write(MemoryRegion{.at = kSeed, .size = 1},
                              std::vector<std::uint8_t>{kStartSeed});
                status = "THE SEED IS BACK WHERE THE CARTRIDGE SETS IT";
            });
        }

        // Everything the panel says about the guest comes from here: one read of the machine's own
        // high RAM and one of the walkers, answered by the last completed step. The guest's counter
        // is one byte, so what it has lived is accumulated from its steps.
        const std::vector<std::uint8_t> hram = machine.read(places, &Places::hram);
        const std::uint8_t              now  = hram[kFrames - gb::Hram.at];
        if (!firstSample) lived += static_cast<std::uint8_t>(now - frames);
        firstSample = false;
        frames      = now;
        seed        = hram[kSeed - gb::Hram.at];
        walkers     = machine.read(places, &Places::walkers);
        step        = machine.read(places, &Places::step).at(0);
    });

    FrameDrawState frame;
    const auto     buildFrame = [&]() {
        clearMon();
        const auto row = [&](int at, std::string_view name, std::string_view value, PaletteId pal) {
            put(kNameCol, at, name, palDim);
            put(kValueCol, at, value, pal);
        };
        const auto heading = [&](int at, std::string_view name, std::string_view keys) {
            put(2, at, name, palText);
            put(kKeyCol, at, keys, palLive);
        };

        put(2, 1, "COEXECUTION", palText);
        put(kKeyCol, 1, "F FULLSCREEN", palLive);
        put(2, 2, "A CARTRIDGE RUNS ITS OWN WORLD AND NATIVE CODE JOINS IN", palDim);

        // The eight tracks: the guest's own bytes, one walker a row, read this tick.
        heading(4, "ITS WORLD", "");
        put(kNameCol, 4 + 1, "EIGHT WALKERS IT MOVES EVERY FRAME", palDim);
        for (std::uint32_t w = 0; w < kWalkerCount; ++w) {
            const int at = kTrackRow + static_cast<int>(w);
            for (int i = 0; i < kTrack; ++i) put(kValueCol + i, at, "O", palDim);
            put(kValueCol + (walkers[w] % kTrack), at, "X", palLive);
        }

        heading(15, "HOW IT BEHAVES", "A RULE   H HOLDS   L ANSWERS");
        row(16, "WHO SETS THE PACE", answering ? "THIS PROGRAM" : "THE CARTRIDGE",
            answering ? palLive : palText);
        row(17, "MARCH STEP", pad(step, 1) + (lying ? "  IT READS 3" : "  UP DOWN"),
            lying ? palLive : palText);
        row(18, "SPEED", std::string(kSpeeds[speedIndex].name) + "  RIGHT LEFT",
            kSpeeds[speedIndex].num == 0 ? palLive : palText);
        row(19, "IT IS", running ? "RUNNING  SPACE PARKS IT" : "PARKED  SPACE RESUMES IT",
            running ? palText : palLive);
        {
            const auto rule = walkerRule.load();
            row(20, "ITS STORES ARE",
                std::string(kWalkerRuleNames[static_cast<std::size_t>(rule)]) + "  REFUSED " +
                    pad(vetoed.load(), 5),
                rule == WalkerRule::Free ? palText : palLive);
        }

        heading(21, "WHAT THIS PROGRAM SEES", "X FORGETS THE ESCAPE");
        row(22, "THE ESCAPE SAW", pad(seen.load(), 6) + (watching ? " FRAMES" : " AND IS GONE"),
            watching ? palText : palLive);
        row(23, "ITS OWN RAM SAYS", pad(lived, 6) + " FRAMES", palText);
        row(24, "NESTED CALLS", pad(rolls.load(), 6) + "  SEED " + pad(seed, 3), palText);

        heading(26, "WHILE IT IS PARKED", "P PULLS   S RESEEDS");
        row(27, "ITS OWN DECODER", unpacked.empty() ? "PRESS P" : hex(unpacked),
            unpacked.empty() ? palDim : palText);
        row(28, "ITS OWN TEXT", pulledText.empty() ? "PRESS P" : pulledText,
            pulledText.empty() ? palDim : palText);
        row(29, "IN BANK TWO", hex(bankTwo), palText);

        put(2, 31, status, palLive);

        frame.layers.clear();
        DrawLayer panel{.key = "panel"};
        panel.z       = 0;
        panel.size    = PixelSize{kViewW, kViewH};
        panel.content = TileContent{.widthInTiles  = kMonW,
                                    .heightInTiles = kMonH,
                                    .cells         = std::span<const TileCell>(mon),
                                    .wrap          = TileWrap::Blank};
        frame.layers.push_back(panel);
    };

    loop.renderLoop([&]() {
        buildFrame();
        renderer.renderFrame(frame);
    });

    std::printf(
        "co-execution — the eight tracks are a cartridge's own walkers, marched by its own code on\n"
        "its own thread. A answers its pace routine with a C++ function that calls the cartridge's\n"
        "own generator, and they scatter. UP and DOWN write a byte into the image while it runs,\n"
        "RIGHT and LEFT set the speed, SPACE parks it, X drops the watching escape, P calls its own\n"
        "decoder and pointer table with the machine parked, S puts the seed back, H decides its own\n"
        "stores to them, L answers its own read of the march step, F fullscreen.\n\n");

    WindowedHost host{loop, platform};
    host.run();

    machine.stop();
    return 0;
}
