#pragma once

// SNES / 65816 controller vocabulary — the platform-specific half of the VM host surface.
//
// vm.h is system-agnostic; this header supplies the SNES's controller vocabulary as the value a game
// hands vm.buttons(). The console has two controller ports, so this header names both. A game binds its
// input to the Button actions below and reads them each tick with held(), exactly as it does for the Game
// Boy family (gb.h).
//
// The SNES's routine and register vocabulary — the 65816 register set, the memory map — is not here: a
// hosted cartridge runs its own code, and naming the places inside it is a later capability. What this
// header ships is the pad.

#include <cstdint>
#include <optional>

#include "retropp/guest_buttons.h"  // GuestButtons — the opaque word vm.buttons() takes
#include "retropp/input.h"          // ActionId + InputState — the Button vocabulary is an Actions enum

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

}  // namespace retropp::snes
