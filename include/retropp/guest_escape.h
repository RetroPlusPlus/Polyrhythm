#pragma once

// Where a hosted machine's own code hands control to the game's native code, and what runs when it
// does.
//
// A game declares the places it wants to intercept as ONE batch and the machine checks every entry
// when the batch is registered, so a table of escapes is answered once rather than one failure at a
// time during play — the same shape as registerRegions. An escape names a place with the machine's
// own vocabulary, bank-qualified where the console needs it (gb::banked), exactly as MemoryRegion.at
// does.
//
// WHAT AN ESCAPE DOES, PRECISELY. When the machine is about to execute the instruction at `at`, the
// handler runs to completion first; the instruction then executes, exactly once, whatever the handler
// did. The handler runs on the host's clock, and the guest's clock stands still for it, so a machine
// with escapes holds the same cadence as one without.
//
// The handler receives the machine, so it reads and writes declared places while it runs. It fires
// SYNCHRONOUSLY ON THE THREAD THAT STEPS THE MACHINE: for a machine the game drives, that is the
// game's own call; for a machine running on its own thread (Vm::run), that thread. Whatever a handler
// touches, the handler owns the thread-safety of.
//
// REPLACING A ROUTINE is the other kind of escape: `.replaces = routine(binding, fn)` declares that a
// native function ANSWERS INSTEAD of the routine at `at` — while the escape is live the guest never
// executes that routine's body, and switching it off or removing it puts the routine back exactly as
// it was. The binding transcribes the calling convention the routine already has in the cartridge
// (where its callers leave the arguments, where they expect the answer), and the platform marshals both
// directions synchronously while the machine is parked at the entry — so the caller reads a correct
// answer in the same step, in the register or memory its own code was written to read. See vm.h's
// routine(...) for the declaration and the direction the values flow.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "retropp/location.h"    // Location — where a value lives in the target machine
#include "retropp/object_key.h"  // ObjectKey — the required developer-supplied identity

namespace retropp {

class Vm;

// The marshaled form of a native function answering for a guest routine — what `.replaces` holds.
// Consumers never fill this directly: routine(binding, fn) in vm.h builds one, deducing the widths
// from the function's signature. The fields mirror what the platform needs at fire time: where to FIND
// each argument in the parked machine (the guest's callers put them there), where to PUT the answer
// (the guest's callers read it from there), and the native body with its typing erased.
struct NativeRoutine {
    std::vector<Location>   inputs;           // argument i is read from inputs[i]
    std::vector<int>        inputWidths;      // bytes of argument i, from the signature
    std::optional<Location> output;           // the return value is written here (nullopt = void)
    int                     outputWidth = 0;  // bytes of the return value
    std::uint32_t           declaredEntryOffset = 0;  // carried only so registration can refuse it
    bool                    declaredHardwarePacing = false;  // likewise — pacing has no meaning here

    // The native body: takes the marshaled argument values in declaration order, returns the value
    // to write back (ignored when there is no output).
    std::function<std::uint64_t(const std::uint64_t*)> fn;

    bool declaredLongReturn = false;  // carried only so registration can refuse it — the escape's
                                      // address carries the return

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(fn); }
};

// What runs when control escapes the guest. `at` is the address the escape was declared at — the same
// value the declaration carried, so one handler serves several escapes and still knows which fired.
using EscapeHandler = std::function<void(Vm& machine, std::uint32_t at)>;

// One declared escape. Exactly one of `handler` and `replaces` is declared — an escape that would do
// nothing, or that cannot decide which of two things to do, is refused at registration by its key.
//
//   key      — how the game names this escape afterward, and what a registration failure reports.
//              Required: omitting it is a compile error.
//   at       — where in the machine's address space control escapes. Bank-qualified where the console
//              needs it (gb::banked); opaque to the platform, decoded by the backend. An escape declared
//              in a banked window fires only while that bank is the live one.
//   handler  — the OBSERVING kind: your code runs, then the guest's instruction executes as it was
//              going to. The routine there still runs.
//   replaces — the ANSWERING kind: your code runs INSTEAD of the routine there, speaking its calling
//              convention through the declared binding. Built by routine(binding, fn) — see vm.h.
//   armed    — whether it is live. Declared escapes are live by default; an escape switched off keeps
//              its declaration and costs the machine nothing while it runs. Switching off an answering
//              escape puts the routine back exactly as it was.
struct GuestEscape {
    ObjectKey     key;
    std::uint32_t at = 0;
    EscapeHandler handler{};
    NativeRoutine replaces{};
    bool          armed = true;
};

// The declared batch for one machine.
struct EscapeMap {
    std::vector<GuestEscape> declarations;
};

// Collect one or more GuestEscape declarations into the batch registerEscapes takes.
template <class... Rest>
[[nodiscard]] EscapeMap escapes(GuestEscape first, Rest... rest) {
    EscapeMap out;
    out.declarations.reserve(1 + sizeof...(rest));
    out.declarations.push_back(std::move(first));
    (out.declarations.push_back(std::move(rest)), ...);
    return out;
}

// One declared escape on a machine, named by its key. Obtained from Vm::escapes() and used where it is
// obtained — it borrows its machine for the length of the expression and is not something to keep.
// SWITCHING ONE WHILE THE MACHINE RUNS IS FINE, and lands where every other verb does. Arming an
// escape changes the machine's own code — the backend watches an armed address, and an answering
// escape holds a return at it — so a switch issued from the game thread crosses to the thread that
// owns the machine and takes effect at the next step boundary, in the order issued, exactly as a
// write to a declared place does. It is therefore not visible to the very next read: `armed(false)`
// followed at once by `armed()` still answers true until that boundary. Issued from inside a handler
// it applies immediately instead — that code is already running on the machine's own thread.
class EscapeRef {
public:
    // Whether this escape is live, and switching it. A switched-off escape stays declared, so
    // switching it back on needs no re-registration and no address re-validation.
    [[nodiscard]] bool armed() const;
    void armed(bool on);

    // Drop the declaration entirely. Switching an escape off is the ordinary way to stop it; this is
    // for a game that wants the entry gone. Naming it again afterward throws until it is re-declared.
    //
    // A handler that removes its OWN escape destroys the code it is running in. Remove another one,
    // or switch this one off and drop it once the call has returned.
    void remove();

private:
    friend class EscapeTable;
    EscapeRef(Vm& machine, ObjectKey key) : machine_(&machine), key_(std::move(key)) {}

    Vm*       machine_;
    ObjectKey key_;
};

// The escapes declared on one machine. Obtained from Vm::escapes().
class EscapeTable {
public:
    // Name a declared escape. Throws std::out_of_range naming the key if this machine has no escape by
    // that name — a mistyped key doing nothing quietly is the failure this refuses.
    [[nodiscard]] EscapeRef operator[](std::string_view key) const;

    // Whether this machine declares an escape by that name, and how many it declares.
    [[nodiscard]] bool        contains(std::string_view key) const;
    [[nodiscard]] std::size_t size() const;

private:
    friend class Vm;
    explicit EscapeTable(Vm& machine) : machine_(&machine) {}

    Vm* machine_;
};

}  // namespace retropp
