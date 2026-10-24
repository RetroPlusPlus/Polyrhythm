// Guest-nesting demo — native code calls the cartridge's own routines, in the guest's own context.
//
// An escape lets guest code hand control to this program. This is the other half of the same seam:
// this program calling INTO the guest, at any depth, and leaving the machine exactly as it found it.
// Two acts:
//
//   * ANSWERING WITH THE CARTRIDGE'S OWN CODE. The cartridge is authored right here, every byte of
//     it this file's own, its program assembled by the engine's own SM83 assembler. Its loop asks
//     its own damage routine for a number every frame. A replacement answers that routine natively
//     — and builds its answer by calling the cartridge's OWN generator, nested inside the escape,
//     while the machine runs on its own thread at hardware cadence. The damage the guest reads is
//     this program's rule over the cartridge's own randomness, and the generator's seed advances in
//     the guest's memory exactly as the cartridge intended.
//   * READING WHAT THE GAME NEVER REACHED. Then the machine is parked, and this program calls the
//     cartridge's own decompressor on a packed table the running loop never touches — content that
//     only the cartridge's own code knows how to unpack, obtained without playing the game to it.
//
// Headless: no window, no GPU, no audio device. Safe to run anywhere, including over SSH.
//
// With --verify the demo asserts what it observes — that the nested generator ran once per answer,
// that the seed advanced in step, that the damage is this program's rule over the guest's number,
// and that the decoder unpacked the table — and exits nonzero on any violation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "retropp/gb.h"             // gb::A / gb::B / gb::C — the routines' own conventions, named
#include "retropp/guest_escape.h"   // GuestEscape — where control leaves the guest
#include "retropp/memory_region.h"  // MemoryRegion — where a place is
#include "retropp/vm.h"             // Vm — hosting the cartridge, running it, calling into it

namespace {

// Where the cartridge's pieces live. Each is assembled as its own block and placed here, so the
// loop's `call` and this program's `bindRoutine` name an address both sides agree on.
constexpr std::uint32_t kProgram = 0x0150;  // the game loop
constexpr std::uint32_t kDamage  = 0x0180;  // the cartridge's own damage routine
constexpr std::uint32_t kRandom  = 0x01A0;  // the cartridge's own generator
constexpr std::uint32_t kDecoder = 0x1000;  // the cartridge's own decompressor
constexpr std::uint32_t kPacked  = 0x1400;  // the packed table only the decompressor understands

constexpr std::uint32_t kAttack  = 0xC000;  // what this program tells the guest to fight with
constexpr std::uint32_t kDefence = 0xC001;
constexpr std::uint32_t kDamaged = 0xC002;  // what the guest's loop publishes each frame
constexpr std::uint32_t kUnpacked = 0xC010; // where the decompressor leaves its work

// The loop: every frame, ask the damage routine what this attack does to this defense, and publish
// the answer. The seed the generator keeps lives in high RAM, where the cartridge put it.
constexpr std::string_view kGameSource = R"(
    ld a, $01
    ldh [$FFFF], a      ; IE: VBlank only
    xor a
    ldh [$FF80], a      ; the frame counter starts at zero
    ldh [$FF0F], a      ; count from the NEXT VBlank, not a stale pending one
    ld a, $07
    ldh [$FF81], a      ; the generator's seed, as the cartridge starts it
    ei
loop:
    halt
    nop
    ldh a, [$FF80]
    inc a
    ldh [$FF80], a      ; one more frame lived
    ld a, [$C000]
    ld b, a             ; attack rides in B
    ld a, [$C001]
    ld c, a             ; defence rides in C
    call $0180          ; the cartridge's own damage routine
    ld [$C002], a       ; the answer comes back in A, published for the outside
    jr loop
)";

// The damage routine, and its convention: attack in B, defense in C, answer in A. Its rule is the
// cartridge's own — and this program answers instead of it.
constexpr std::string_view kDamageSource = R"(
    ld a, b
    sub c
    ret
)";

// The generator, and its convention: no inputs, the number in A. Its rule is seed * 5 + 13, which
// walks all 256 values before it repeats. It advances a seed the cartridge keeps in its own high
// RAM, so calling it moves the guest's world exactly as the cartridge's own code does.
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

// The decompressor: run-length pairs at $1400 — a count and a value, ending at a count of zero —
// expanded byte by byte to $C010. The running loop never calls it; only the cartridge knows the
// format, and this is how a program gets the content out.
constexpr std::string_view kDecoderSource = R"(
    ld hl, $1400
    ld de, $C010
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
constexpr std::uint8_t kPackedTable[] = {3, 0xAA, 2, 0xBB, 4, 0xCC, 0};
constexpr std::uint8_t kExpected[]    = {0xAA, 0xAA, 0xAA, 0xBB, 0xBB, 0xCC, 0xCC, 0xCC, 0xCC};

// The places this program watches. Never instantiated — the struct exists so the places have names,
// and naming one wrong is a compile error rather than a bad address.
struct Places {
    retropp::MemoryRegion frames;    // the guest's own frame counter, in high RAM
    retropp::MemoryRegion seed;      // the generator's seed, in the guest's own high RAM
    retropp::MemoryRegion attack;
    retropp::MemoryRegion defence;
    retropp::MemoryRegion damage;    // what the guest's loop publishes
    retropp::MemoryRegion unpacked;  // what the decompressor leaves behind
};

// One authored cartridge: entry jumps to the loop, the VBlank vector returns, each routine sits
// where its callers name it, and the packed table sits where only the decompressor looks.
std::vector<std::uint8_t> authorGame(retropp::Vm& assembler) {
    std::vector<std::uint8_t> rom(0x8000, 0x00);
    rom[0x0143] = 0x80;  // CGB-flagged, so a Color machine boots it in CGB mode
    rom[0x0147] = 0x00;  // ROM only
    rom[0x0148] = 0x00;  // 32 KiB
    rom[0x0040] = 0xD9;  // VBlank vector: reti
    rom[0x0100] = 0x00;
    rom[0x0101] = 0xC3;
    rom[0x0102] = 0x50;
    rom[0x0103] = 0x01;  // jp $0150

    const auto place = [&](std::uint32_t at, std::string_view source) {
        const std::vector<std::uint8_t> code = assembler.assemble(std::string(source));
        std::copy(code.begin(), code.end(), rom.begin() + at);
    };
    place(kProgram, kGameSource);
    place(kDamage, kDamageSource);
    place(kRandom, kRandomSource);
    place(kDecoder, kDecoderSource);
    std::copy(std::begin(kPackedTable), std::end(kPackedTable), rom.begin() + kPacked);
    return rom;
}

void report(const char* what, bool ok, int& failures) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool verify   = argc > 1 && std::strcmp(argv[1], "--verify") == 0;
    int        failures = 0;

    retropp::Vm::GBC machine;
    machine.hostRom(authorGame(machine));

    const auto places = machine.registerRegions(retropp::regions(
        retropp::region(&Places::frames, retropp::MemoryRegion{.at = 0xFF80, .size = 1},
                        "frame counter"),
        retropp::region(&Places::seed, retropp::MemoryRegion{.at = 0xFF81, .size = 1}, "seed"),
        retropp::region(&Places::attack, retropp::MemoryRegion{.at = kAttack, .size = 1}, "attack"),
        retropp::region(&Places::defence, retropp::MemoryRegion{.at = kDefence, .size = 1},
                        "defence"),
        retropp::region(&Places::damage, retropp::MemoryRegion{.at = kDamaged, .size = 1}, "damage"),
        retropp::region(&Places::unpacked,
                        retropp::MemoryRegion{.at = kUnpacked, .size = 1, .count = 9}, "unpacked")));

    const auto byteAt = [&](retropp::MemoryRegion Places::*key) {
        return machine.read(places, key).at(0);
    };

    std::printf("guest_nesting: this program calls the cartridge's own routines\n");
    std::printf("  its damage routine is at 0x%04X, its generator at 0x%04X, its decompressor at "
                "0x%04X\n\n", kDamage, kRandom, kDecoder);

    // The cartridge's generator, bound where it already sits: the address IS the routine, and the
    // binding transcribes the convention it already has — no inputs, the number in A.
    auto random = machine.bindRoutine<std::uint8_t()>(
        kRandom, retropp::RoutineBinding{.output = retropp::gb::A});

    // ── Act one: a native answer built from the guest's own routine ──────────────────────────────
    // The replacement speaks the damage routine's convention (attack in B, defense in C, answer in
    // A) and, inside it, calls the cartridge's generator. That call runs the guest's own code on the
    // guest's own stack and gives every register back, so the loop that was interrupted mid-`call`
    // carries on without noticing — while the seed the generator keeps really does advance.
    std::mutex                mx;
    std::vector<std::uint8_t> rolls;  // what the guest's generator answered, each time
    std::atomic<int>          answered{0};

    machine.registerEscapes(retropp::escapes(retropp::GuestEscape{
        .key      = "damage",
        .at       = kDamage,
        .replaces = retropp::routine(
            retropp::RoutineBinding{.inputs = {retropp::gb::B, retropp::gb::C},
                                    .output = retropp::gb::A},
            [&](std::uint8_t attack, std::uint8_t defence) -> std::uint8_t {
                const std::uint8_t roll = random();  // the cartridge's own generator, nested
                {
                    const std::lock_guard<std::mutex> lock(mx);
                    rolls.push_back(roll);
                }
                answered.fetch_add(1);
                return static_cast<std::uint8_t>((attack - defence) + (roll & 0x0F));
            })}));

    machine.run();
    // Set the fight up once the machine is living: boot fills power-on RAM the way hardware does, so
    // a value written before boot is not the value the guest reads after it.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    machine.write(places, &Places::attack, std::vector<std::uint8_t>{40});
    machine.write(places, &Places::defence, std::vector<std::uint8_t>{15});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    machine.stop();

    // Read everything PARKED. How many frames fit in a wall-clock sleep varies run to run, so what
    // is asserted is the relation: one nested generator call per native answer, and a seed that
    // advanced once for each of them.
    const int          answers  = answered.load();
    const std::uint8_t lived    = byteAt(&Places::frames);
    const std::uint8_t damage   = byteAt(&Places::damage);
    const std::uint8_t seed     = byteAt(&Places::seed);
    std::vector<std::uint8_t> seen;
    {
        const std::lock_guard<std::mutex> lock(mx);
        seen = rolls;
    }

    // The generator's rule is seed = seed * 5 + 13 from a start of 7, so after N calls the seed is
    // what the cartridge's own arithmetic would have reached — this program never wrote it.
    std::uint8_t predicted = 7;
    for (int i = 0; i < answers; ++i) {
        predicted = static_cast<std::uint8_t>(predicted * 5 + 13);
    }

    std::printf("act one — a native rule over the cartridge's own randomness\n");
    std::printf("  the guest lived %u frames and read %d native answers\n", lived, answers);
    std::printf("  the cartridge's generator answered %zu times, the last of them %u\n",
                seen.size(), seen.empty() ? 0 : seen.back());
    std::printf("  its seed advanced 7 -> %u, which is where its own arithmetic lands\n", seed);
    std::printf("  attack 40, defence 15, and the damage the guest published was %u\n\n", damage);

    // ── Act two: reading what the game never reached ─────────────────────────────────────────────
    // The machine is parked. Its decompressor is bound where it sits and called on the guest's own
    // stack; it walks a table the running loop never touches and leaves the bytes in work RAM, where
    // this program reads them.
    auto decompress = machine.bindRoutine<void()>(kDecoder, retropp::RoutineBinding{});
    decompress();

    std::vector<std::uint8_t> unpacked;
    for (std::uint32_t i = 0; i < 9; ++i) {
        unpacked.push_back(machine.read(places, &Places::unpacked, i).at(0));
    }

    std::printf("act two — the cartridge's own decompressor, on a parked machine\n");
    std::printf("  packed as 3x$AA 2x$BB 4x$CC, and it unpacked to");
    for (const std::uint8_t b : unpacked) {
        std::printf(" %02X", b);
    }
    std::printf("\n\n");

    if (verify) {
        const bool matches =
            unpacked.size() == std::size(kExpected) &&
            std::equal(unpacked.begin(), unpacked.end(), std::begin(kExpected));
        const bool oneRollPerAnswer = static_cast<int>(seen.size()) == answers;
        const bool damageIsTheRule =
            seen.empty() || damage == static_cast<std::uint8_t>((40 - 15) + (seen.back() & 0x0F));

        std::printf("verify\n");
        report("the native rule answered while the guest ran", answers > 0, failures);
        report("one nested generator call per answer", oneRollPerAnswer, failures);
        report("the guest's own seed advanced once per call", seed == predicted, failures);
        report("the damage is this program's rule over the guest's number", damageIsTheRule,
               failures);
        report("the guest lived through every one of them", lived > 0, failures);
        report("the parked machine's own decompressor unpacked the table", matches, failures);
        std::printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    }

    return failures == 0 ? 0 : 1;
}
