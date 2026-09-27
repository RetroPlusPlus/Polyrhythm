#pragma once

// The SNES's vocabulary — the platform-specific half of the VM host surface.
//
// vm.h is system-agnostic; this header supplies what a binding, a declaration or an ActionMap names on
// this console: the pad and its two ports, the 65816 register file as Location constants, the machine's
// memories as MemoryRegion constants, and the helpers that name a memory the bus cannot reach.
// `retropp/gb.h` is the Game Boy family's.

#include <cstdint>
#include <optional>

#include "retropp/guest_buttons.h"  // GuestButtons — the opaque word vm.buttons() takes
#include "retropp/input.h"          // ActionId + InputState — the Button vocabulary is an Actions enum
#include "retropp/location.h"       // Location — a register a binding names
#include "retropp/memory_region.h"  // MemoryRegion — the machine's memories are constants of it

namespace retropp::snes {

// The SNES pad's twelve buttons as an Actions enum — the vocabulary a game binds its input to, shipped
// because a console's button set is fixed by the hardware rather than chosen by the game. The order is the
// pad's own shift order, which is the order the auto-read registers hold and a recorded run names.
//
//   ActionMap controls{{snes::Button::A, {SDL_SCANCODE_X, PadButton::FaceLabelA}}, …};
//   controls.add(presets::directional(snes::Button::Up, snes::Button::Down,
//                                     snes::Button::Left, snes::Button::Right));
//   platform.actions(controls);
//
// A game that also has actions of its own numbers them clear of these — one id space, 64 wide.
enum class Button : ActionId { B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R };

// One pad, held or not. held(input) builds it from the actions above, and a game that drives a guest some
// other way can fill the fields itself.
struct Buttons {
    bool b      = false;
    bool y      = false;
    bool select = false;
    bool start  = false;
    bool up     = false;
    bool down   = false;
    bool left   = false;
    bool right  = false;
    bool a      = false;
    bool x      = false;
    bool l      = false;
    bool r      = false;

    // This pad in port one, port two an empty socket. The bits are the pad's own shift order in the low
    // twelve, and the thirteenth marks the port occupied — a pad holding nothing read apart from a socket
    // with no pad in it. The machine takes the converted value; nothing between here and its core reads
    // the bits.
    [[nodiscard]] constexpr operator GuestButtons() const noexcept {
        return GuestButtons{.held = static_cast<std::uint64_t>(b) << 0 |
                                    static_cast<std::uint64_t>(y) << 1 |
                                    static_cast<std::uint64_t>(select) << 2 |
                                    static_cast<std::uint64_t>(start) << 3 |
                                    static_cast<std::uint64_t>(up) << 4 |
                                    static_cast<std::uint64_t>(down) << 5 |
                                    static_cast<std::uint64_t>(left) << 6 |
                                    static_cast<std::uint64_t>(right) << 7 |
                                    static_cast<std::uint64_t>(a) << 8 |
                                    static_cast<std::uint64_t>(x) << 9 |
                                    static_cast<std::uint64_t>(l) << 10 |
                                    static_cast<std::uint64_t>(r) << 11 |
                                    std::uint64_t{1} << 15};  // port one occupied
    }
};

// Both controller ports. An empty optional is an empty socket — a port with no controller in it, which a
// program tells apart from a pad with nothing held. snes::Ports{} is a console with nothing plugged in.
struct Ports {
    std::optional<Buttons> one;
    std::optional<Buttons> two;

    // Port one in the low half, port two shifted into the high half; an empty optional leaves its half
    // zero, which the machine reads as an empty socket.
    [[nodiscard]] constexpr operator GuestButtons() const noexcept {
        std::uint64_t held = 0;
        if (one) {
            held |= static_cast<GuestButtons>(*one).held;
        }
        if (two) {
            held |= static_cast<GuestButtons>(*two).held << 16;
        }
        return GuestButtons{.held = held};
    }
};

// The buttons held this tick, read off the Button actions. Hand it straight to the machine:
//
//   vm.buttons(snes::held(input));
//
// A pure read — the engine stores nothing and decides nothing; what each button is bound to was settled by
// the map the game submitted. A button counts as down if it is held at the tick OR was pressed since the
// last one, so a tap shorter than a tick still reaches the guest, exactly as gb::held does.
[[nodiscard]] inline Buttons held(const InputState& input) noexcept {
    const auto down = [&input](Button b) noexcept {
        return input.isHeld(b) || input.justPressed(b);
    };
    return Buttons{
        .b      = down(Button::B),
        .y      = down(Button::Y),
        .select = down(Button::Select),
        .start  = down(Button::Start),
        .up     = down(Button::Up),
        .down   = down(Button::Down),
        .left   = down(Button::Left),
        .right  = down(Button::Right),
        .a      = down(Button::A),
        .x      = down(Button::X),
        .l      = down(Button::L),
        .r      = down(Button::R),
    };
}

// ── Registers ───────────────────────────────────────────────────────────────────────────────────

// The 65816 register file, as Location constants a binding names. The enumerator order IS the backend's
// register id and fixes each register's width: the five 8-bit registers first, then the six 16-bit ones.
enum class Reg : std::uint16_t { A, B, P, DB, PB, C, X, Y, D, S, PC };

// The readable spelling at a binding site:
//
//   RoutineBinding{.inputs = {snes::A, snes::X}, .output = snes::A}
//
// A and B are the accumulator's low and high bytes, and C is the whole 16-bit accumulator. X and Y are 16
// bits wide whatever the index-width flag says; in 8-bit index mode the chip uses the low byte, so a
// binding that carries a byte in one carries it as a std::uint16_t.
inline constexpr Location A  = Location::reg(static_cast<std::uint16_t>(Reg::A));   // accumulator, low byte
inline constexpr Location B  = Location::reg(static_cast<std::uint16_t>(Reg::B));   // accumulator, high byte
inline constexpr Location P  = Location::reg(static_cast<std::uint16_t>(Reg::P));   // the status byte
inline constexpr Location DB = Location::reg(static_cast<std::uint16_t>(Reg::DB));  // the data bank
inline constexpr Location PB = Location::reg(static_cast<std::uint16_t>(Reg::PB));  // the program bank
inline constexpr Location C  = Location::reg(static_cast<std::uint16_t>(Reg::C));   // the whole accumulator
inline constexpr Location X  = Location::reg(static_cast<std::uint16_t>(Reg::X));   // index register X
inline constexpr Location Y  = Location::reg(static_cast<std::uint16_t>(Reg::Y));   // index register Y
inline constexpr Location D  = Location::reg(static_cast<std::uint16_t>(Reg::D));   // the direct page
inline constexpr Location S  = Location::reg(static_cast<std::uint16_t>(Reg::S));   // the stack pointer
inline constexpr Location PC = Location::reg(static_cast<std::uint16_t>(Reg::PC));  // the program counter

// ── A routine's return ──────────────────────────────────────────────────────────────────────────

// A routine a JSL enters and an RTL leaves. Wraps the entry address a bindRoutine, an escape's `.at` or
// an Instruction::call names; a bare address is a routine a JSR enters and an RTS leaves. The engine
// pushes the landing a JSL would and, for a replaced routine, stands an RTL at the entry.
//
//   auto decode = machine.bindRoutine<std::uint16_t(std::uint16_t)>(snes::rtl(0x018000),
//                                                                  {.inputs = {snes::C}, .output = snes::C});
[[nodiscard]] constexpr std::uint32_t rtl(std::uint32_t entry) noexcept { return entry | 0x80000000u; }

// ── The machine's memories ──────────────────────────────────────────────────────────────────────

// Where a value lives on this console, as the 32-bit address a MemoryRegion, a Location or a routine's
// entry carries. A plain 24-bit bus address names work RAM, the cartridge image or its save the way the
// console's own map reaches them; a memory the bus cannot name is reached by name through one of the
// helpers below, which folds which memory into the top byte. The backend decodes it. Every alias of a
// byte names that byte: `0x000010`, `0x7E0010` and `0xBF0010` are one cell of work RAM.
enum class Space : std::uint8_t {
    Bus      = 0x00,  // the 65816's own bus: work RAM, the cartridge, its save
    VideoRam = 0x01,  // the picture chip's 64 KB, a byte address
    Palette  = 0x02,  // CGRAM, 512 bytes
    Sprites  = 0x03,  // OAM, 544 bytes
    AudioRam = 0x04,  // the audio unit's 64 KB
};

// Byte `at` of `space`: the space in the top byte, the offset in the 24 bits below it.
[[nodiscard]] constexpr std::uint32_t inSpace(Space space, std::uint32_t at) noexcept {
    return (static_cast<std::uint32_t>(space) << 24) | (at & 0x00FFFFFFu);
}

// Byte `at` of each memory the bus cannot name — the `.at` of a place inside one:
//
//   MemoryRegion{.at = snes::videoRam(0x2000), .size = 32}  // one 4 bpp tile, 0x2000 bytes in
//
// Each is written the way the chip reads it: no port address steps and no latch moves.
[[nodiscard]] constexpr std::uint32_t videoRam(std::uint16_t at) noexcept { return inSpace(Space::VideoRam, at); }
[[nodiscard]] constexpr std::uint32_t palette(std::uint16_t at)  noexcept { return inSpace(Space::Palette, at); }
[[nodiscard]] constexpr std::uint32_t sprites(std::uint16_t at)  noexcept { return inSpace(Space::Sprites, at); }
[[nodiscard]] constexpr std::uint32_t audioRam(std::uint16_t at) noexcept { return inSpace(Space::AudioRam, at); }

// Each memory as a MemoryRegion: the same value a game fills in for its own content, filled in here for
// the hardware. Read or write one straight away —
//
//   const std::vector<std::uint8_t> colors = vm.read(snes::Palette);
//
// — or name a piece of one by building a MemoryRegion at that address instead.
//
// These are the memories that are the same size in every console, so they can be constants. The
// cartridge and its save are not among them: their sizes are the image's, so a place inside either is a
// plain bus address (0x008000, 0x700000).
//
// `count` is 1 on all of them — a whole memory is the degenerate case of an array with one entry — so
// read(…) with no index hands back the entire memory.
//
// A register is a place too, at its bus address ($004210, or the same offset in any system bank): a
// read answers the byte the program's own read would, with nothing moved — the NMI flag a read of $4210
// clears stays set. A register takes no write through a place; a program writes one through a routine.
inline constexpr MemoryRegion WorkRam  = {.at = 0x7E0000, .size = 0x20000};  // 128 KB, banks $7E-$7F
inline constexpr MemoryRegion VideoRam = {.at = videoRam(0), .size = 0x10000};  // tiles and maps, by byte
inline constexpr MemoryRegion Palette  = {.at = palette(0), .size = 0x200};  // the 256 palette words
inline constexpr MemoryRegion Sprites  = {.at = sprites(0), .size = 0x220};  // 128 entries + their high bits
inline constexpr MemoryRegion AudioRam = {.at = audioRam(0), .size = 0x10000};  // the audio unit's 64 KB

}  // namespace retropp::snes
