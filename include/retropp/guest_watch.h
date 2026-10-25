#pragma once

// Where a hosted machine's own reads and writes are decided by the game's native code.
//
// A watch is keyed on an ACCESS: when the guest reads or writes a byte of the declared place, native
// code runs and says what that access does. Reach for a watch when the interesting thing is a place
// in memory, whichever instruction touches it; reach for an escape (guest_escape.h) when it is a
// place in the code.
//
// Watches are declared as ONE batch and the machine checks every entry when the batch is registered,
// so a table is answered once rather than one failure at a time during play — the same shape as
// registerRegions and registerEscapes. A watch names a place with MemoryRegion, the same value the
// read/write verbs name places with, bank-qualified where the console needs it (gb::banked).
//
// WHAT A HANDLER ANSWERS WITH. Each handler returns an AccessVerdict: proceed() lets the access
// happen as the cartridge intended, instead(v) substitutes a byte, and veto() prevents a write. The
// decision is made PER ACCESS, inside the handler, so a condition that changes ("freeze this only
// while the shield is up") is a branch in one handler rather than a watch armed and disarmed from
// the game thread every time the condition moves.
//
// TWO DIRECTIONS, DECLARED SEPARATELY. A watch declares .onRead, .onWrite, or both — at least one,
// or the entry is refused by its key. Each handler answers one question, so veto() reads clearly on
// the write side, and declaring only the direction you want is what keeps the machine cheap: the
// platform arms that direction alone, and a memory access is the hottest path a machine has.
//
// A handler fires SYNCHRONOUSLY ON THE THREAD THAT STEPS THE MACHINE, mid-instruction, with the
// machine parked at the access. It is handed the machine, so it reads declared places, calls the
// cartridge's own routines through bindRoutine, and declares or switches escapes and watches of its
// own, to any depth. Whatever a handler touches, the handler owns the thread-safety of.
//
// This is the primitive; what a game builds on it is the game's.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "retropp/memory_region.h"  // MemoryRegion — the place a watch is declared over
#include "retropp/object_key.h"     // ObjectKey — the required developer-supplied identity

namespace retropp {

class Vm;

// What a watch handler answers with: what the access it just intercepted actually does.
//
// A small value type with named constructors rather than an enum, because one case carries a byte —
// the same shape as TransparentIndices::of and Mapper::fromId. There is no default: a handler names
// the outcome it wants.
class AccessVerdict {
public:
    enum class Kind : std::uint8_t {
        Proceed,  // the access happens as the cartridge intended
        Veto,     // the write never lands (on a read, see below)
        Instead,  // this byte is used in place of the one the access carried
    };

    // The access happens as the cartridge intended: the guest's write lands, or the guest's read
    // answers with the byte the machine holds.
    [[nodiscard]] static AccessVerdict proceed() noexcept {
        return AccessVerdict{Kind::Proceed, 0};
    }

    // The write never lands — the byte at that place keeps the value it already had, and nothing the
    // store would have done to hardware happens.
    //
    // ON A READ THIS IS THE SAME AS proceed(): a read answers with a byte, so the guest receives the
    // one the machine holds. Use instead(v) to change what the guest sees.
    [[nodiscard]] static AccessVerdict veto() noexcept { return AccessVerdict{Kind::Veto, 0}; }

    // Use this byte instead: on a read, the value the guest sees rather than the one memory holds;
    // on a write, the value that lands rather than the one the guest was storing.
    [[nodiscard]] static AccessVerdict instead(std::uint8_t value) noexcept {
        return AccessVerdict{Kind::Instead, value};
    }

    [[nodiscard]] Kind         kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint8_t value() const noexcept { return value_; }

private:
    constexpr AccessVerdict(Kind kind, std::uint8_t value) noexcept : kind_(kind), value_(value) {}

    Kind         kind_;
    std::uint8_t value_;
};

// Whose accesses a watch answers for.
//
// The cartridge's own code is always watched — that is what a watch is, and it includes guest code
// running inside a bindRoutine call, because that is still the guest executing. What is declarable
// is whether the GAME's own deliberate read/write verbs go through the watch too: a game whose logic
// keys off writes may want its own writes to drive that logic as well.
//
// The platform's own stores are never watched under either value — seeding a booted image, planting a
// return landing, and the store that realizes instead(v) on a write. Those are the platform acting on
// the machine, rather than the machine accessing itself.
enum class AccessSource : std::uint8_t {
    Guest,         // the cartridge's own code only
    GuestAndGame,  // ... and the game's own Vm::read / Vm::write
};

// What runs when the guest touches a watched place. `at` is the address actually accessed, in the
// machine's own vocabulary — the declared base for a one-byte watch, and the byte's own address for
// a watch declared over a span, so one handler serving a region still knows which byte moved.
// `value` is the byte the access carries: on a read, what the machine would have answered; on a
// write, what the guest is storing.
using WatchHandler =
    std::function<AccessVerdict(Vm& machine, std::uint32_t at, std::uint8_t value)>;

// One declared watch. At least one of `onRead` and `onWrite` is declared — a watch with neither has
// no work to do, and is refused at registration by its key.
//
//   key     — how the game names this watch afterward, and what a registration failure reports.
//             Required: omitting it is a compile error.
//   at      — the place being watched. A one-byte place is MemoryRegion{.at = addr, .size = 1}; a
//             span is declared the same way any other place is, and every byte of it is watched.
//             Bank-qualified where the console needs it (gb::banked); a watch declared in a banked
//             window fires only while that bank is the live one.
//   from    — whose accesses fire it (AccessSource above). The cartridge's own code always does.
//   onRead  — what answers when the guest reads a byte of this place. veto() means proceed here.
//   onWrite — what answers when the guest writes a byte of this place.
//   armed   — whether it is live. Declared watches are live by default; a watch switched off keeps
//             its declaration and costs the machine nothing while it runs.
struct GuestWatch {
    ObjectKey    key;
    MemoryRegion at{};
    AccessSource from = AccessSource::Guest;
    WatchHandler onRead{};
    WatchHandler onWrite{};
    bool         armed = true;
};

// The declared batch for one machine.
struct WatchMap {
    std::vector<GuestWatch> declarations;
};

// Collect one or more GuestWatch declarations into the batch registerWatches takes.
template <class... Rest>
[[nodiscard]] WatchMap watches(GuestWatch first, Rest... rest) {
    WatchMap out;
    out.declarations.reserve(1 + sizeof...(rest));
    out.declarations.push_back(std::move(first));
    (out.declarations.push_back(std::move(rest)), ...);
    return out;
}

// One declared watch on a machine, named by its key. Obtained from Vm::watches() and used where it
// is obtained — it borrows its machine for the length of the expression and is not something to
// keep.
//
// SWITCHING ONE WHILE THE MACHINE RUNS IS FINE, and lands where every other verb does: a change
// issued from the game thread crosses to the thread that owns the machine and takes effect at the
// next step boundary, in the order issued, exactly as a write to a declared place does. It is
// therefore not visible to the very next read — `armed(false)` followed at once by `armed()` still
// answers true until that boundary. Issued from inside a handler it applies immediately instead:
// that code is already running on the machine's own thread.
class WatchRef {
public:
    // Whether this watch is live, and switching it. A switched-off watch stays declared, so
    // switching it back on needs no re-registration and no re-validation of its place.
    [[nodiscard]] bool armed() const;
    void               armed(bool on);

    // Drop the declaration entirely. Switching a watch off is the ordinary way to stop it; this is
    // for a game that wants the entry gone. Naming it again afterward throws until it is
    // re-declared.
    //
    // A handler that removes its OWN watch destroys the code it is running in. Remove another one,
    // or switch this one off and drop it once the handler has returned.
    void remove();

private:
    friend class WatchTable;
    WatchRef(Vm& machine, ObjectKey key) : machine_(&machine), key_(std::move(key)) {}

    Vm*       machine_;
    ObjectKey key_;
};

// The watches declared on one machine. Obtained from Vm::watches().
class WatchTable {
public:
    // Name a declared watch. Throws std::out_of_range naming the key if this machine has no watch by
    // that name — a mistyped key doing nothing quietly is the failure this refuses.
    [[nodiscard]] WatchRef operator[](std::string_view key) const;

    // Whether this machine declares a watch by that name, and how many it declares.
    [[nodiscard]] bool        contains(std::string_view key) const;
    [[nodiscard]] std::size_t size() const;

private:
    friend class Vm;
    explicit WatchTable(Vm& machine) : machine_(&machine) {}

    Vm* machine_;
};

}  // namespace retropp
