#pragma once

// Which of a hosted machine's buttons are held, in that machine's own terms.
//
// A console's button set is fixed by its hardware, so the vocabulary is per platform and a
// per-platform header names it — gb::Buttons for the Game Boy family, with named fields the game
// fills. That value converts to this one, which carries the state as a single opaque word the engine
// passes through without inspecting, exactly as a declared place's address is an opaque 32-bit value.
// A console with a different button set is a different vocabulary rather than a wider shared enum.
//
// The state is a level, not an event: a controller has lines that are held or released, and the guest
// samples them when its own code reads. Hand over what is held now, every tick, and the guest sees
// whatever it looks for.

#include <cstdint>

namespace retropp {

struct GuestButtons {
    // One bit per button, in the guest platform's own order. The platform header that produces this
    // decides the packing; nothing between here and that machine's core reads it.
    std::uint64_t held = 0;

    [[nodiscard]] constexpr bool operator==(const GuestButtons&) const noexcept = default;
};

}  // namespace retropp
