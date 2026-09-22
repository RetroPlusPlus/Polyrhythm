#pragma once

// The VM host public API — the platform's runtime virtual-machine surface for the narrow
// set of original routines whose output cannot be faithfully native-ported (gameplay RNG; later a
// sound driver).
//
// This header is PLATFORM-AGNOSTIC. It selects a per-system backend (VMPlatform) and exposes a
// generic, function-like call surface; the per-system vocabulary a routine binding names — the CPU
// register set, the memory map — lives in a platform-specific header (the Game Boy / SM83 family in
// retropp/gb.h; other systems are drop-in). The call surface is identical across systems because each
// routine's convention is sealed in its binding.
//
// A consumer registers a surgically-extracted routine ONCE, declaring where each input and the
// output live (a CPU register or an absolute memory address), and thereafter calls it as an ordinary
// typed C++ function:
//
//     retropp::Vm vm{retropp::VMPlatform::GameBoyColor};
//     auto rng = vm.uploadRoutine<std::uint8_t()>(routineBytes, {.output = retropp::gb::A});
//     std::uint8_t roll = rng();          // no register / memory / address idiom at the call site
//
// This carries the platform's "no hardware-register variables exist in the port" principle to the VM
// boundary: registers, memory addresses, and entry offsets appear ONLY inside a routine's binding —
// never where a routine is called.
//
// A VM hosts ONE of two things, and each refuses the other.
//
// PLACED ROUTINES. Surgically-extracted byte images (embedded at build time, or read from a path when
// they cannot be embedded) placed at declared addresses in the VM's own code space: the narrow set
// whose output cannot be faithfully native-ported — gameplay RNG, and a game's own sound driver. A
// driver image may be bank-qualified, and the VM bank-switches through its own placed code. Nothing
// else runs on such a machine.
//
// A WHOLE CARTRIDGE the game supplies (hostRom). Every byte of the image is addressable through the
// declared-place surface, and run() boots it and runs its own code continuously on a thread of its
// own — at the platform's own speed, or any fraction or multiple of it the game sets while it runs
// (speed(num, den)). Native code and the cartridge's code meet
// through declared places (read / write) and declared escapes (guest_escape.h) — where control leaves
// the guest at an address the game names, runs the game's C++, and resumes. A hosted cartridge has no
// code arena, so uploadRoutine / registerRoutine throw on such a machine.
//
// The header pulls NO backend type (no SameBoy GB_*, no SM83 register enum): the template callable
// converts typed arguments to width-tagged values and delegates the machine work to non-template Vm
// members defined in vm.cpp, which dispatch through the abstract backend seam.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "retropp/asset_policy.h"      // AssetPolicy (registerRoutine's Embed / LoadFromPath choice)
#include "retropp/driver_binding.h"    // DriverBinding / Instruction — the resident-driver surface below
#include "retropp/guest_buttons.h"     // GuestButtons — what buttons() takes
#include "retropp/guest_escape.h"      // GuestEscape / EscapeMap / EscapeTable — the escape surface below
#include "retropp/guest_frame.h"       // GuestFrameContent — what video() answers with
#include "retropp/guest_watch.h"       // GuestWatch / WatchMap / WatchTable — the watch surface below
#include "retropp/isa.h"               // Isa + the VMPlatform → Isa mapping below
#include "retropp/literal_path.h"      // LiteralPath (registerRoutine takes a compile-time literal path)
#include "retropp/location.h"          // Location — the register / memory value-home vocabulary
#include "retropp/memory_region.h"     // MemoryRegion — where a declared place in the guest lives
#include "retropp/timing.h"

namespace retropp {

namespace vm {
struct VmTestAccess;  // src/vm/vm_testing.h — the internal deterministic seam for device-free tests
class VmBackend;  // src/vm/vm_backend.h — the seam a core implements
struct VmCoreAccess;  // src/vm/vm_core_access.h — builds a Vm from a resolved core
}  // namespace vm

// The target system whose VM backend runs the routine. Each enumerator selects a per-system backend;
// the call surface is identical across systems because each routine's convention is sealed in its
// binding. GameBoy / GameBoyColor map to the SM83 / SameBoy backend — the only backend built in v1.
// Any other enumerator throws at Vm construction ("no backend built in v1"); it is a drop-in when a
// consumer exercises it (the ViewportResolution::Snes precedent). Extend this list as systems land.
enum class VMPlatform { GameBoy, GameBoyColor, Snes, Nes, Genesis, MasterSystem };

// The ISA a VM of `platform` runs — the assembler it uses and the byte format it accepts. Several
// platforms can share one ISA (the Game Boy and Game Boy Color both run SM83), which is why a chiptune's
// compatibility is keyed on the ISA, not the exact platform. The audio system uses this to verify, at
// play(), that a catalog entry's (developer-selected) ISA matches the VM it is being cued on. Unbuilt
// platforms have no backend (Vm construction throws), so their mapping is a placeholder for now.
[[nodiscard]] constexpr Isa isaFor(VMPlatform platform) noexcept {
    switch (platform) {
        case VMPlatform::GameBoy:
        case VMPlatform::GameBoyColor:
            return Isa::Sm83;
        case VMPlatform::Snes:
        case VMPlatform::Nes:
        case VMPlatform::Genesis:
        case VMPlatform::MasterSystem:
            break;  // ISA added with the backend; unreachable today (Vm ctor throws for these)
    }
    return Isa::Sm83;
}

namespace detail {

// How a machine's core is built. A Vm constructor resolves the core for its platform where the game
// writes the constructor, so a game's binary carries the cores it names.
using CoreFactory = std::unique_ptr<vm::VmBackend> (*)(VMPlatform);

std::unique_ptr<vm::VmBackend> gameBoyCore(VMPlatform platform);  // src/vm/gameboy/sameboy_backend.cpp

[[nodiscard]] inline CoreFactory coreFor(VMPlatform platform) noexcept {
    switch (platform) {
        case VMPlatform::GameBoy:
        case VMPlatform::GameBoyColor:
            return &gameBoyCore;
        case VMPlatform::Snes:
        case VMPlatform::Nes:
        case VMPlatform::Genesis:
        case VMPlatform::MasterSystem:
            break;
    }
    return nullptr;  // no core is built for this platform; constructing the Vm throws std::runtime_error
}

}  // namespace detail

// How a routine is paced. HostSpeed runs the routine as fast as the host allows — the form for a
// routine you CALL for a return value (RNG). HardwareSpeed throttles to the CPU clock for a real-time
// consumer (a continuously-running audio driver), which is stepped via startDriver / stepDriver rather
// than called for a value.
enum class Throttle {
    HostSpeed,
    HardwareSpeed,
};

// The developer-declared I/O binding: the ONLY place registers / memory / entry offsets are named.
// The WIDTH of each input and the output comes from the callable signature (sizeof), not from here;
// the binding names only WHERE each value lives and HOW the routine is paced.
//   RoutineBinding{ .inputs = {gb::A, gb::B}, .output = gb::A, .throttle = Throttle::HostSpeed }
struct RoutineBinding {
    std::vector<Location>   inputs;                  // argument i marshals to inputs[i]
    std::optional<Location> output{};               // return value reads from output (nullopt = void)
    Throttle                throttle = Throttle::HostSpeed;
    std::uint32_t           entryOffset = 0;         // first instruction's offset WITHIN the supplied
                                                     // routine bytes (usually 0). NOT a ROM address.
};

// Compile-time constraint on the values a routine I/O location can hold: the unsigned-integral
// widths a console CPU register or memory location carries (8 / 16 / 32-bit). The selected backend
// further constrains a value bound to one of ITS registers to that register's actual width.
template <typename T>
inline constexpr bool kIsVmValue =
    std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
    std::is_same_v<T, std::uint32_t>;

namespace detail {

// Deduce a native callable's signature so routine(...) needs no template arguments at the call site:
// the lambda's parameter types set the argument widths and its return type sets the output width.
template <typename F>
struct NativeCallSig : NativeCallSig<decltype(&F::operator())> {};
template <typename C, typename Ret, typename... Args>
struct NativeCallSig<Ret (C::*)(Args...) const> {
    using BuildFn = NativeRoutine (*)(RoutineBinding, void*);
    template <typename G>
    static NativeRoutine build(RoutineBinding binding, G fn) {
        static_assert((kIsVmValue<Args> && ...),
                      "a replacement's arguments must be uint8_t, uint16_t, or uint32_t");
        static_assert(std::is_void_v<Ret> || kIsVmValue<Ret>,
                      "a replacement's return type must be void, uint8_t, uint16_t, or uint32_t");
        NativeRoutine out;
        out.inputs                 = std::move(binding.inputs);
        out.inputWidths            = {static_cast<int>(sizeof(Args))...};
        out.output                 = binding.output;
        out.declaredEntryOffset    = binding.entryOffset;
        out.declaredHardwarePacing = binding.throttle != Throttle::HostSpeed;
        if constexpr (!std::is_void_v<Ret>) {
            out.outputWidth = static_cast<int>(sizeof(Ret));
        }
        out.fn = [f = std::move(fn)](const std::uint64_t* values) mutable -> std::uint64_t {
            return invoke(f, values, std::index_sequence_for<Args...>{});
        };
        return out;
    }
    template <typename G, std::size_t... I>
    static std::uint64_t invoke(G& f, const std::uint64_t* values, std::index_sequence<I...>) {
        if constexpr (std::is_void_v<Ret>) {
            f(static_cast<Args>(values[I])...);
            return 0;
        } else {
            return static_cast<std::uint64_t>(f(static_cast<Args>(values[I])...));
        }
    }
};
template <typename C, typename Ret, typename... Args>
struct NativeCallSig<Ret (C::*)(Args...)> : NativeCallSig<Ret (C::*)(Args...) const> {};

}  // namespace detail

// Build the `.replaces` value of a GuestEscape: a native function answering for a guest routine,
// speaking that routine's own calling convention.
//
// THE DIRECTION THE VALUES FLOW — the binding vocabulary is uploadRoutine's, mirrored, because the
// direction of the CALL is mirrored. There, your C++ is the caller: the platform puts your arguments
// INTO the bound locations and reads the output back OUT for you. Here, the GUEST is the caller: its
// own code loaded the bound input locations before its `call`, the platform reads your arguments OUT of
// them, and your return value is written INTO the bound output — where the routine's callers were
// always going to look for it. The binding is a transcription of the convention the routine already
// has in the cartridge, discovered from its code, not a convention you invent.
//
// The function's signature carries the widths (parameter i ↔ binding.inputs[i]; the return type ↔
// binding.output), so no width is ever written down. `.throttle` and `.entryOffset` have no meaning
// for a replacement and a binding that sets either is refused at registration.
template <typename F>
[[nodiscard]] NativeRoutine routine(RoutineBinding binding, F fn) {
    return detail::NativeCallSig<F>::build(std::move(binding), std::move(fn));
}

class Vm;

// A typed handle to a registered routine: call it like a plain function. Copyable value handle that
// holds a non-owning Vm* + the routine's handle within that Vm — valid only while the owning Vm is
// alive (the same lifetime contract as AtlasId / PaletteId; do not outlive or move the owning Vm).
template <typename Sig>
class Routine;  // primary left undefined — only function-type specializations are valid

template <typename Ret, typename... Args>
class Routine<Ret(Args...)> {
    static_assert((kIsVmValue<Args> && ...),
                  "Routine arguments must be uint8_t, uint16_t, or uint32_t");
    static_assert(std::is_void_v<Ret> || kIsVmValue<Ret>,
                  "Routine return type must be void, uint8_t, uint16_t, or uint32_t");

public:
    Routine() = default;  // empty handle; calling it is undefined (no Vm)

    Ret operator()(Args... args) const;

private:
    friend class Vm;
    Routine(Vm* vm, std::size_t handle) noexcept : vm_(vm), handle_(handle) {}

    Vm* vm_ = nullptr;
    std::size_t handle_ = 0;
};

// A marshalled call value crossing from the typed template into the non-template VM core: the value
// zero-extended to 64 bits + its width in bytes. Internal to the call path; consumers never build one.
struct CallValue {
    std::uint64_t value;
    int width;  // 1, 2, or 4
};

// ── Naming places in the guest's address space ──────────────────────────────────────────────────
//
// A game declares the places it cares about in a machine as ONE batch, and the platform checks every
// entry when the batch is registered — so a table of two hundred places is answered once, not one
// failure at a time deep in gameplay.
//
// The keys are fields of a game-defined struct, named by pointer-to-member, so a typo is a compile
// error rather than a bad address. The struct is a vocabulary, never instantiated: it exists so the
// places have names.
//
//   struct Places {
//       MemoryRegion tileArt;
//       MemoryRegion textTable;
//   };
//
//   const auto places = vm.registerRegions(regions(
//       region(&Places::tileArt,   MemoryRegion{.at = gb::banked(2, 0x4000), .size = 16, .count = 384},
//              "tile art"),
//       region(&Places::textTable, MemoryRegion{.at = 0x3000, .size = 32, .count = 64},
//              "text table")));
//
// The batch is not an asset table — it is the places this game cares about in this machine. One
// declared field is read and written through the same key.

// One declared place bound to a game struct field. Templated on the struct so every entry in one
// regions(...) batch belongs to the SAME struct — a field of another type does not compile.
template <class S>
struct RegionBinding {
    MemoryRegion S::* key;
    MemoryRegion      where;
    std::string_view  name;
};

// Bind a game struct field to a place in the guest's address space. `name` is what a registration
// failure reports: a pointer-to-member carries no name at runtime, so without it a bad entry can only
// be identified by its address — and for a batch generated from a symbol file, that is materially
// worse than the name the generator already had.
template <class S>
[[nodiscard]] RegionBinding<S> region(MemoryRegion S::* member, MemoryRegion where,
                                      std::string_view name) {
    return RegionBinding<S>{.key = member, .where = where, .name = name};
}

// The declared batch for one machine.
template <class S>
struct RegionMap {
    std::vector<RegionBinding<S>> bindings;
};

// Collect one or more region() bindings into a RegionMap<S>. S is deduced from the first binding;
// every other must name a field of that same struct.
template <class S, class... Rest>
[[nodiscard]] RegionMap<S> regions(RegionBinding<S> first, Rest... rest) {
    RegionMap<S> out;
    out.bindings.reserve(1 + sizeof...(rest));
    out.bindings.push_back(std::move(first));
    (out.bindings.push_back(std::move(rest)), ...);
    return out;
}

// A registered batch on one VM, remembering the struct its keys come from. Hold it and name places
// through it; it carries the declaration order the keys resolve against.
template <class S>
class RegionMapId {
public:
    RegionMapId() = default;  // empty handle; naming a place through it is undefined (no Vm)

    // The place a key was declared to name, or nullopt if the key is not in this batch.
    [[nodiscard]] std::optional<MemoryRegion> declared(MemoryRegion S::* member) const {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == member) {
                return declarations_[i];
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

private:
    friend class Vm;
    RegionMapId(std::size_t handle, std::vector<MemoryRegion S::*> keys,
                std::vector<MemoryRegion> declarations) noexcept
        : handle_(handle), keys_(std::move(keys)), declarations_(std::move(declarations)) {}

    std::size_t                     handle_ = 0;
    std::vector<MemoryRegion S::*>  keys_;
    std::vector<MemoryRegion>       declarations_;
};

// One declared place flattened for the non-template registration core: where it is and what to call
// it in an error. Internal to the registration path; consumers never build one.
struct DeclaredRegion {
    MemoryRegion     where;
    std::string_view name;
};

// The VM host. Owns one backend machine (selected by VMPlatform); routines registered on it share
// its memory (so RNG seed state persists across calls). Non-copyable (owns a machine); movable.
// What a machine produces, declared at construction. A machine hosts without producing anything: it
// runs a cartridge, holds its memory, and answers the verbs below, and nothing is drawn or heard
// unless it is asked for. Each output costs the machine something a machine that never uses it should
// not pay — a raster costs cycles — so each is named and each is off until declared.
//
// Every output has a runtime verb of the same name (video(bool)), so a game that knows at construction
// says so here, and one that decides later switches it there. They are the same setting.
struct VmConfig {
    // What this machine's own files are kept under, in the player's data directory. A machine given a
    // key keeps its guest's persistent data — what a cartridge holds on to when the power goes off —
    // and comes up on it again next run, with the game calling nothing: the data is read back when a
    // cartridge is hosted, and written out after the guest changes it. Empty, and the machine keeps
    // nothing, which is every machine that does not ask for this.
    //
    // The key is one path component and is the game's to keep stable: it is what makes the data found
    // again, so a machine keyed differently next run comes up on nothing, and two machines sharing a
    // key share one set of files. The file inside that directory is named by batterySave().
    std::string key;

    bool video = false;  // the machine draws; its finished frames become a layer's content
};

class Vm {
public:
    explicit Vm(VMPlatform platform, TimingProfile timing = TimingProfile::GameBoyColor)
        : Vm(detail::coreFor(platform), platform, timing, VmConfig{}) {}

    // The same, with what the machine is and produces declared up front:
    //
    //     Vm gb{VMPlatform::GameBoyColor, VmConfig{.key = "adventure", .video = true}};
    //
    // Equivalent to constructing without them and calling the matching verb — a machine declared this
    // way is drawing before it hosts anything, which is the difference from switching it on later.
    //
    // Throws std::invalid_argument for a key that is more than one path component (a separator, "."
    // or ".."), and std::logic_error for a keyed machine on a platform whose core keeps nothing.
    Vm(VMPlatform platform, VmConfig config)
        : Vm(detail::coreFor(platform), platform, TimingProfile::GameBoyColor, std::move(config)) {}
    Vm(VMPlatform platform, TimingProfile timing, VmConfig config)
        : Vm(detail::coreFor(platform), platform, timing, std::move(config)) {}

    ~Vm();

    // What to call the file this machine's data is kept in. The whole path is
    //
    //     <the game's own per-user data directory>/VM/<the machine's key>/<this name>.<extension>
    //
    // and this names the file alone. It is "battery" until this says otherwise, which is all a machine
    // that plays one cartridge ever needs.
    //
    // THE EXTENSION IS THE CORE'S, and it always ends the file — the bytes are the console's own format,
    // so what a file of them is called is that console's answer, and a Game Boy machine's data lands as
    // the ".sav" every other program that reads one already expects. Spell it out and it is left alone
    // ("zelda.sav" stays); leave it off and it is added ("zelda" → "zelda.sav"); carry some other
    // extension and it keeps that and gets this after it ("zelda.bak" → "zelda.bak.sav"). Either way the
    // file is one a player can take elsewhere.
    //
    // A machine that plays WHATEVER IT IS HANDED needs more: one machine, many cartridges, each one's
    // data its own. It cannot have said so at construction, because it does not have the image yet —
    // so it says so once it does:
    //
    //     Vm machine{model, VmConfig{.key = "ticked", .video = true}};
    //     machine.batterySave(nameOf(image));   // → VM/ticked/<that image>.sav
    //     machine.hostRom(image);
    //
    // Either order works. Set before a cartridge is hosted, the stored copy is read back as the
    // cartridge arrives; set after, it is read back here. What the machine already owes under the name
    // it is leaving is written out under THAT name first, so changing cartridges never files one
    // image's data under another's.
    //
    // Only while the machine is parked. The name decides which file the data goes to, and moving that
    // out from under a guest writing into it is the one moment it must not change — so this throws
    // std::logic_error on a running machine, as every verb that touches the machine does.
    //
    // Throws std::invalid_argument for a name that is empty or more than one file's worth (a path
    // separator, "." or ".."), and std::logic_error on a machine with no key — a file needs a directory
    // to be in, and VmConfig::key is what names that.
    void batterySave(std::string_view name);

    // Nested platform-bound instantiation types — the CPU-half symmetric spelling of
    // AudioSystem::{GB,GBC} (the all-caps hardware vocabulary of the driver-hosting design). Each fixes the
    // console (VMPlatform + TimingProfile) so a consumer names the hardware once at the type and never
    // repeats it at construction: `retropp::Vm::GBC vm;` is `Vm{VMPlatform::GameBoyColor,
    // TimingProfile::GameBoyColor}`. They ARE Vms (they add no state, only the pre-binding) — use one
    // anywhere a Vm is expected. Platform namespaces (gb::, …) stay HARDWARE vocabulary only; a system type
    // never lives in one.
    class GB;
    class GBC;

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&&) noexcept;
    Vm& operator=(Vm&&) noexcept;

    // The system this VM hosts (the one passed at construction).
    [[nodiscard]] VMPlatform platform() const noexcept;

    // Reset the machine to its post-reset state — clears persistent routine state (e.g. RNG seeds).
    // Registered routines stay registered (their bytes live in the VM's code space, untouched).
    void reset();

    // Advance the machine's free-running clock by `cycles` CPU cycles WITHOUT executing a routine —
    // so hardware registers a routine reads (e.g. the Game Boy's rDIV divider) keep ticking BETWEEN
    // calls, exactly as they do on always-running hardware. This is what makes a hardware-RNG host
    // faithful: rDIV must reflect the time elapsed since the last call, or the RNG degenerates into a
    // counter. Drive it from the host's clock — one tick's worth of cycles per platform tick, which the
    // timing profile already defines: pass TimingProfile::cpuCyclesPerTick() (no hardcoded count).
    // Calling it is optional: a routine that reads no time-based register (pure computation) does not
    // need it.
    void advanceClock(std::uint64_t cycles);

    // Advance the machine by exactly one platform tick's worth of ITS OWN cycles, carrying the
    // sub-cycle remainder from tick to tick. Say "a tick happened" and let the machine work out what
    // that is worth to it, rather than computing a cycle count at the call site.
    //
    // This is what makes a machine hosted at a cadence that is not its own stay honest. The cycles a
    // tick is worth come from the machine's clock rate and the period actually being run — never from
    // the machine's own frame count, which is right only when the two cadences coincide. A guest whose
    // clock does not divide the tick period leaves a fraction of a cycle behind every tick; the
    // fraction is kept and spent later, so the running total is exact over any number of ticks and the
    // instantaneous error never exceeds one cycle.
    //
    // Pass the period the run loop is actually ticking at. The no-argument form spends one frame of
    // the machine's own clock, which is the common case: a machine running at its native rate.
    //
    // On a cartridge running Advance::OnTick this is the step that advances it: queued writes and
    // escape and watch switches land, the machine runs the tick's worth of cycles, and its declared
    // regions publish. The speed factor scales what the tick is worth. Throws std::logic_error on a
    // cartridge running Advance::Continuously — that machine keeps its own clock.
    //
    // A machine whose core keeps no clock of its own advances nothing.
    void advanceTick(std::chrono::nanoseconds enginePeriod);
    void advanceTick();

    // ── Audio chain (the hardware-speed driver path) ────────────────────────────────────────────────
    // The narrow set of original routines that produce sound run as continuously-executing DRIVERS at
    // the hardware CPU clock (Throttle::HardwareSpeed), their APU register writes synthesizing PCM at
    // the original cadence — distinct from a HostSpeed routine that is CALLED for a return value (RNG).
    // These three members are that path: enable the APU + sink once, position the driver, then step it
    // one cycle budget per sim tick. (The cue surface a game drives by meaning is the AudioSystem; this
    // is the raw chain it sits on.)

    // Enable the backend's APU and route each produced stereo PCM frame to `onSample` (called per
    // sample on the thread that steps the driver — for the audio chain, the AudioSystem's production
    // thread). The APU's sample rate is set to
    // `sampleRate` so it resamples to the sink rate internally. Call once before driving a routine.
    void enableAudio(unsigned sampleRate,
                     std::function<void(std::int16_t left, std::int16_t right)> onSample);

    // Position a hardware-speed driver routine to run continuously (PC → its entry). It is not run to a
    // return for a value — stepDriver advances it. `driver` must be registered on THIS Vm with
    // Throttle::HardwareSpeed (throws otherwise).
    void startDriver(const Routine<void()>& driver);

    // Run the started driver for `cpuCycles` CPU cycles (the TimingProfile CPU unit — pass
    // TimingProfile::cpuCyclesPerTick() once per sim tick); the APU produces ~rate/frameRate frames
    // into the enabled sink during the run. Returns the CPU cycles actually run.
    std::uint64_t stepDriver(std::uint64_t cpuCycles);

    // Host a whole cartridge image on THIS VM: hand over the image's BYTES and the machine comes up
    // on them, with every byte of the cartridge addressable. Use it to reach content that already
    // exists inside a game's own cartridge — art, tables, text — and feed it to the ingestion
    // surfaces, converting first if the format needs it.
    //
    // BYTES, NEVER A PATH. A path would force one delivery policy; bytes take either. Register the
    // image with `registerData` and pass `data(id)`, or read it however the game likes.
    //
    // This makes the image READABLE, not running: there is no boot and no entry point. The backend
    // parses the image's own header, and the platform exposes no cartridge metadata — no title, no
    // mapper, no size. Every one of those is console-shaped, and the caller holds the bytes.
    //
    // Hosting a game's cartridge and hosting a platform-built one are EXCLUSIVE, and each refuses the
    // other. hostDriver synthesizes a cartridge — the platform writes its header and places content
    // into the gaps — so the platform owns that image; here the game does. On a hosted cartridge,
    // uploadRoutine / registerRoutine throw as well: there is no arena to inject into. One VM does
    // one or the other.
    //
    // Throws std::invalid_argument for an empty image, or std::logic_error if this VM already hosts
    // a driver.
    void hostRom(std::span<const std::uint8_t> rom);

    // ── The hosted cartridge runs ───────────────────────────────────────────────────────────────
    // A hosted image is readable the moment it is hosted; these run it. The machine boots to the
    // state the platform's own firmware leaves, and one of two clocks drives it from there: a thread
    // of its own, or the game's tick. While it runs, the places declared through registerRegions are
    // the observable set: read answers them from a coherent per-step publish, write lands at a step
    // boundary in the order issued, and a place built on the spot needs the machine stopped. A
    // running machine stays where it is — stop() before moving the Vm.

    // Which clock advances a running cartridge.
    enum class Advance : std::uint8_t {
        // A thread of its own, at the platform's own speed times the current factor — the machine
        // holds the hardware's cadence whether or not the game's loop keeps up.
        Continuously,
        // The game's tick, on the thread that calls advanceTick: one call advances the machine by
        // exactly what that tick is worth. One image, one sequence of ticks and one factor put the
        // machine in one state, byte for byte, on every run and every host.
        OnTick,
    };

    // Boot the hosted image and run it. After stop(), running again resumes from where the machine
    // parked; reset() first for a fresh boot. Throws std::logic_error unless this VM hosts a
    // cartridge (hostRom first), if the machine is already running, or on a machine whose core keeps
    // no clock of its own.
    void run(Advance how = Advance::Continuously);

    // Execution speed as a fraction of the platform's own: {1, 1} is the hardware's speed (the
    // default), {2, 1} double, {1, 2} half, {0, 1} paused. Under Advance::OnTick it scales what one
    // tick is worth, so {2, 1} advances two ticks' worth of cycles per tick and {0, 1} advances
    // none. Adjustable at any time, running or not;
    // the pace is exact — owed cycles carry their sub-cycle remainder, never rounding, at any
    // factor. Throws std::invalid_argument for a zero denominator or a term past 1024, and
    // std::logic_error on a machine whose core keeps no clock of its own.
    void speed(std::uint32_t num, std::uint32_t den);
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> speed() const;

    // Leave the loop: the machine parks where it is, keeping every byte of its state, and the
    // thread is gone by return. Harmless when nothing runs.
    void stop();

    // ── Video ───────────────────────────────────────────────────────────────────────────────────
    // A machine hosts WITHOUT drawing: a raster costs cycles, and a machine hosted for its memory or
    // its routines shows nothing. Turn video on and the frames it finishes become a layer's content,
    // composited with native layers like anything else on screen. Declare it at construction
    // (VmConfig) or switch it here; they are the same setting reached two ways.

    // video(true) starts the machine drawing, video(false) stops it. Idempotent either way.
    //
    // EITHER CLOCK DRAWS. Under Advance::OnTick the machine steps on the thread that calls
    // advanceTick, so the frame the game composes with is written where it is read. Under
    // Advance::Continuously it steps on a thread of its own and hands each finished frame across, so
    // what a game reads is a completed frame either way, never one being drawn.
    //
    // Asked of a RUNNING machine, the change lands at its next step boundary — where every change
    // issued to a running machine lands — so the machine is never asked to install anything while its
    // own thread is using it.
    //
    // Throws std::logic_error if this machine's core produces no video at all.
    void video(bool drawing);

    // The last complete frame the machine drew, as a layer's content — hand it straight to a
    // DrawLayer. The pixels stay valid until the next advanceTick.
    //
    // A tick-advanced machine answers from the TICK BOUNDARY, where every other verb on a hosted
    // machine lands, so a frame finishing part-way through a tick waits there. A machine on a clock of
    // its own has no such boundary and answers with the newest frame it has finished. Either way
    // `generation` says which frame this is — never when it arrived — so both read the same.
    //
    // Throws std::logic_error unless this machine has been asked for video; a request that has not
    // reached a step boundary yet counts, and answers with the empty picture until a frame lands.
    [[nodiscard]] GuestFrameContent video() const;

    // Hold these buttons. The state is a level and the whole set is answered at once, so a button
    // absent from the value is released — hand over what is held now, every tick, and the guest reads
    // whatever its own code reads. A per-platform header names the buttons (gb::Buttons) and converts.
    //
    // Lands at the next step boundary, like every other verb: a machine on the game's tick sees it at
    // its next advanceTick, one running on its own clock at its next step.
    //
    // Throws std::logic_error if this machine's core takes no button state.
    void buttons(GuestButtons held);

    // Declare the places in this machine the game cares about, as one batch, and get back the handle
    // that names them. Every entry is checked here — reachable on this machine, and wholly contained
    // in the memory it starts in — so the batch is answered once instead of one failure at a time
    // during play. A batch with bad entries throws naming ALL of them, each by its declared name; a
    // report that stops at the first is what makes a generated two-hundred-entry table painful.
    //
    // Regions are checked against the machine as it stands, so host the cartridge first — a place
    // inside an image that has not been loaded is not reachable yet.
    //
    // Throws std::invalid_argument (an empty batch, or any entry that does not fit).
    template <class S>
    [[nodiscard]] RegionMapId<S> registerRegions(const RegionMap<S>& map);

    // Read one entry of a declared place, and write one back. The bytes are the caller's — a plain
    // buffer, not a catalogued handle, because minting one for a pile of bytes about to be converted
    // and discarded is ceremony. Hand the result to uploadData if it should be catalogued, or to
    // uploadAtlas after converting it.
    //
    // `index` names which entry of the place to move; a place declared with the default count of 1
    // has only entry 0, which is the whole of it. An index the place does not declare throws.
    // Entries are resolved in the machine's decoded address space, so an array longer than a bank
    // reads correctly across the boundaries rather than running off the end of the first one.
    //
    // WRITING IS ALLOWED EVERYWHERE, including into a hosted cartridge: the image is a buffer this
    // process owns, and patching one is a thing a game extending an existing cartridge legitimately
    // does. The write lands in memory only — the file the bytes came from is untouched, and
    // re-hosting replaces the image.
    //
    // Reading a stopped machine gives the bytes as they are at the moment of the call. While the
    // machine runs (run()), a read answers the latest completed step's publish — every declared
    // place captured at one instant, coherent with the others — and a write crosses to the
    // machine's own thread and lands at the next step boundary, ordered with other writes; both
    // work only on declared places while running.
    //
    // Throws std::invalid_argument if the key is not in `map` or the byte count is not one entry,
    // std::out_of_range for an index the place does not declare.
    template <class S>
    [[nodiscard]] std::vector<std::uint8_t> read(const RegionMapId<S>& map, MemoryRegion S::* key,
                                                 std::uint32_t index = 0);
    template <class S>
    void write(const RegionMapId<S>& map, MemoryRegion S::* key,
               std::span<const std::uint8_t> bytes, std::uint32_t index = 0);

    // The same verbs against a place built on the spot rather than declared. Much real content is
    // not tabular — a pointer table points at variable-length blobs, so reaching one means reading
    // the table, decoding an entry, and building a place from what was just read. These forms are
    // checked at the call instead of at registration.
    [[nodiscard]] std::vector<std::uint8_t> read(const MemoryRegion& where, std::uint32_t index = 0);
    void write(const MemoryRegion& where, std::span<const std::uint8_t> bytes,
               std::uint32_t index = 0);

    // ── Escapes (guest code hands control to native code) ───────────────────────────────────────
    // Declare the places in this machine's own code where control leaves the guest and runs the game's
    // native code, then resumes. See guest_escape.h for what an escape is and what a handler may do.

    // Declare the escapes this game wants, as one batch. Every entry is checked here — the address is
    // reachable on this machine, the key is not already declared, no two entries in the batch name the
    // same key or the same address, and each carries a handler to run — so the batch is answered once
    // instead of one failure at a time during play. A batch with bad entries throws naming ALL of them,
    // each by its declared key.
    //
    // Declare before the machine starts running, or between steps of a machine the game drives itself;
    // both are free of races by construction. Declaring on a machine already running under run() throws
    // — arming a machine mid-flight is not available.
    //
    // Escapes are checked against the machine as it stands, so host the cartridge first: a place inside
    // an image that has not been loaded is not reachable yet.
    //
    // Registration hands back nothing. Keys are unique within the machine, so the machine itself
    // answers every question about its escapes afterwards — see escapes() below.
    //
    // Throws std::invalid_argument (an empty batch, or any entry that does not pass) and
    // std::logic_error (the machine is running).
    void registerEscapes(const EscapeMap& map);

    // The escapes declared on this machine, named by key: `machine.escapes()["rng draw"].armed(false)`.
    // Switching one off keeps its declaration, and a machine whose escapes are all off costs exactly
    // what a machine with none costs. Switching and removing work while the machine runs — the change
    // crosses to its thread and lands at the next step boundary, as a write to a declared place does
    // (see guest_escape.h).
    [[nodiscard]] EscapeTable escapes() noexcept;

    // ── Watches (the guest's own accesses, decided by native code) ──────────────────────────────
    // Declare the places in this machine's memory whose reads and writes the game's native code
    // decides. See guest_watch.h for what a watch is and what a handler may answer with.

    // Declare the watches this game wants, as one batch. Every entry is checked here — the place is
    // reachable on this machine and watchable by it, the key is not already declared, no two entries
    // in the batch name the same key or overlapping places, and each carries at least one handler —
    // so the batch is answered once instead of one failure at a time during play. A batch with bad
    // entries throws naming ALL of them, each by its declared key.
    //
    // Two declarations may not overlap, so the watch that answers for a byte is never in question.
    // A place whose reads AND writes are both wanted is ONE watch declaring both handlers.
    //
    // Declare before the machine starts running, or between steps of a machine the game drives
    // itself; both are free of races by construction. Declaring on a machine already running under
    // run() throws — the terms registerRegions and registerEscapes are on.
    //
    // Watches are checked against the machine as it stands, so host the cartridge first: a place
    // inside an image that has not been loaded is not reachable yet.
    //
    // Registration hands back nothing. Keys are unique within the machine, so the machine itself
    // answers every question about its watches afterwards — see watches() below.
    //
    // Throws std::invalid_argument (an empty batch, or any entry that does not pass) and
    // std::logic_error (the machine is running).
    void registerWatches(const WatchMap& map);

    // The watches declared on this machine, named by key: `machine.watches()["hp"].armed(false)`.
    // Switching one off keeps its declaration, and a machine whose watches are all off costs
    // exactly what a machine with none costs. Switching and removing work while the machine runs —
    // the change crosses to its thread and lands at the next step boundary, as a write to a
    // declared place does (see guest_watch.h).
    [[nodiscard]] WatchTable watches() noexcept;

    // ── Resident driver (the hosted-machine path) ───────────────────────────────────────────────
    // A hosted sound driver is richer than a single startDriver routine: N placed images (optionally
    // banked), a per-frame tick entry, declared state slots, and player verbs realized as Instructions.
    // These members are the machine-layer mechanics the audio surfaces compose (AudioSystem::host →
    // HostedDriver). A game does not name them directly; it declares a DriverBinding and acts through
    // the durable handle.

    // Configure THIS VM's machine as a resident-driver host from `binding`: build a cartridge image
    // sized to hold the highest placed bank, install the mapper, place each image at its (possibly
    // bank-qualified) base, relocate the scratch stack to the declared top, then perform the binding's
    // `init` gesture once (the platform-run .init). The ISA is verified against this VM's platform.
    // After this, tickDriver / readSlot drive the resident machine. Throws (std::invalid_argument /
    // std::runtime_error) on: an ISA mismatch, a banked placement with the none mapper, overlapping
    // placed ranges, placement into the boot-ROM window or the platform-reserved header gap, a stack top
    // outside work RAM, or a cartridge the backend cannot address. Uploaded routines already placed on
    // this VM are preserved (the arena and the placed images share one space).
    void hostDriver(const DriverBinding& binding);

    // Run one resident-driver frame: perform each queued Instruction in submission order (mailbox
    // writes and entry calls), call the tick entry to its return, then idle the machine for the
    // remainder of `cyclesPerFrame` (pass TimingProfile::cpuCyclesPerTick()) so the APU synthesizes at
    // the hardware cadence. Returns the CPU cycles consumed by the performed instructions + the tick
    // call (the idle pads the frame to `cyclesPerFrame`). Read the published slots afterwards with
    // readSlot. Throws std::logic_error if no driver is hosted on this VM.
    std::uint64_t tickDriver(std::span<const Instruction> queued, std::uint64_t cyclesPerFrame);

    // Read a declared slot's current value from the hosted machine (slot `index` is the i-th SlotSpec
    // in the hosted binding, in declaration order). The value's width is the slot's declared width.
    // Throws std::logic_error if no driver is hosted, or std::out_of_range for a bad index.
    [[nodiscard]] std::uint64_t readSlot(std::size_t index);

    // Register a surgically-extracted routine from its EMBEDDED BYTES (a build-time `const` array)
    // + its I/O binding, returning a typed callable. The platform injects the bytes into the VM's code
    // space; there is no ROM. `instances` is a declared seam — only 1 is realized; registering with
    // more throws (multi-instance routing for anti-channel-stealing audio is not built yet). The
    // signature determines I/O widths; the binding determines I/O locations and pacing. Throws on:
    // instances > 1, an inputs/arity mismatch, a width/location mismatch, an unknown register for the
    // backend, or an exhausted code arena.
    template <typename Sig>
    Routine<Sig> uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances = 1);

    // Register a routine from a `.asm` FILE (the mirror of loadAtlas): hand over a
    // compile-time LITERAL logical path (never bytes, never a runtime string), and the platform resolves
    // it by the embed/load `policy`. The literal is what a build-time scan can find to bake an Embed
    // routine; a genuinely runtime path is not supported here — read its bytes yourself and use uploadRoutine.
    //   * Embed (default)    — use the bytes the build baked into the binary for this logical path (the
    //                          routine registry). If none were baked (no scan ran), fall
    //                          through to the on-disk read so the path still works during development.
    //   * LoadFromPath       — read `assetPath(path)` at registration and assemble it in-process
    //                          with this VM's platform assembler (the Game Boy family → SM83, the
    //                          platform's own — NO external toolchain), for a copyright-derived routine.
    // `policy` precedence: per-call > the per-type default (Embed).
    // `binding`/`instances`/the signature mean exactly what they do for uploadRoutine; entry is offset 0
    // (a leaf routine). Throws if the file cannot be opened, on a source error (with line context), or
    // on any of the byte form's validation failures.
    template <typename Sig>
    Routine<Sig> registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy = {}, int instances = 1);

    // Bind a routine that ALREADY EXISTS in this machine — in a hosted cartridge, or in code placed
    // here earlier — at the address `at`, declaring where its inputs and its output live. The third
    // way to obtain a Routine<Sig>: uploadRoutine hands over bytes, registerRoutine hands over a
    // file, bindRoutine names an address in code the machine already holds.
    //
    //     auto random = vm.bindRoutine<std::uint8_t()>(gb::banked(1, 0x5A1C),
    //                                                  RoutineBinding{.output = gb::A});
    //     const std::uint8_t roll = random();
    //
    // Calling one runs the guest's own code IN THE GUEST'S OWN CONTEXT. The registers and the stack
    // the routine finds are the ones the machine already had, its return frame is pushed where the
    // guest's own call would push it, and every register is back the way it was when the routine
    // returns — so the code that was interrupted carries on as if nothing had happened. What the
    // routine changed in MEMORY stands: a seed it advanced, a buffer it decoded. That is the answer
    // it exists to give.
    //
    // Call one from an escape handler or from a replacement's function — the routine may escape into
    // native code of its own, which may call another routine, to any depth. Call one from the game's
    // own thread while the machine is stopped, or has never run, to reach content the cartridge
    // stores compressed without ever running its game loop. Calling one on a running machine from
    // any thread other than the one stepping it throws.
    //
    // What the call costs the guest is exact: the routine's own instructions, entry through its
    // return, plus one instruction fetch for the address the return lands on.
    //
    // `binding` is registerRoutine's and means the same thing — the signature carries the widths,
    // the binding names the homes. `throttle` and `entryOffset` have no meaning for a routine that
    // is already in place, since the address IS the entry, and a binding that sets either is refused.
    //
    // Bind before run(), or between steps of a machine the game drives itself: the address and the
    // binding are checked against the machine as it stands, so host the cartridge first, and binding
    // against a machine already running under run() throws — the terms registerRegions and
    // registerEscapes are on.
    //
    // Throws std::invalid_argument if `at` is not reachable on this machine, or on any of
    // uploadRoutine's binding failures; std::logic_error if the machine is running. Throws
    // std::logic_error at CALL time if the machine is running and the caller is not the thread
    // stepping it, or if the routine is bank-qualified and its bank is not the one currently mapped.
    template <typename Sig>
    Routine<Sig> bindRoutine(std::uint32_t at, const RoutineBinding& binding);

    // Assemble assembly SOURCE into machine-code bytes for THIS VM's ISA (the Game Boy family → SM83) —
    // the VM's platform alone decides the ISA, so the right assembler is always selected. A source →
    // bytes transform, NOT a path or registration call: it is how a consumer holding routine/audio
    // source obtains bytes to hand to uploadRoutine (the "runtime need ⇒ hand raw bytes" path for
    // source). The audio system uses it to materialize a LoadFromPath chiptune. Throws on a source error.
    [[nodiscard]] std::vector<std::uint8_t> assemble(std::string_view source);

private:
    Vm(detail::CoreFactory core, VMPlatform platform, TimingProfile timing, VmConfig config);

    template <typename Sig>
    friend class Routine;
    friend struct vm::VmTestAccess;  // the deterministic seam device-free tests step the run through
    friend struct vm::VmCoreAccess;
    friend class EscapeRef;    // the two escape views below reach the declared table through
    friend class EscapeTable;  // the private by-key core, so it is not public surface
    friend class WatchRef;     // and the two watch views, through their own
    friend class WatchTable;

    // The escape table's by-key core, reached only through EscapeTable / EscapeRef. Each throws
    // std::out_of_range naming the key when this machine declares no escape by that name.
    [[nodiscard]] bool        escapeArmed(std::string_view key) const;
    void                      setEscapeArmed(std::string_view key, bool on);
    void                      removeEscape(std::string_view key);
    [[nodiscard]] bool        hasEscape(std::string_view key) const;
    [[nodiscard]] std::size_t escapeCount() const;

    // The watch table's by-key core, on the same terms.
    [[nodiscard]] bool        watchArmed(std::string_view key) const;
    void                      setWatchArmed(std::string_view key, bool on);
    void                      removeWatch(std::string_view key);
    [[nodiscard]] bool        hasWatch(std::string_view key) const;
    [[nodiscard]] std::size_t watchCount() const;

    // Non-template core (defined in vm.cpp). registerResolved validates + places the bytes through
    // the backend + stores the resolved binding, returning its handle; invoke sets up the call
    // frame, marshals inputs, runs to return, and reads the output — all via the backend seam.
    // registerRegions' non-template core: validate every declared place against the machine and
    // store the batch, returning its handle. Throws naming every entry that failed.
    std::size_t registerRegionsResolved(std::span<const DeclaredRegion> declared);

    std::size_t registerResolved(std::span<const std::uint8_t> routineBytes,
                                 const RoutineBinding& binding,
                                 std::span<const int> inputWidths,
                                 int outputWidth, int instances);
    // registerRoutine's non-template core: resolve the embed/load policy for `logicalPath`, then either
    // place the build-baked bytes (Embed) or read `assetPath(logicalPath)` + assemble it (LoadFromPath
    // or an un-baked Embed), placing + resolving as registerResolved does.
    std::size_t registerRoutineResolvingPolicy(std::string_view logicalPath, const RoutineBinding& binding,
                                               std::optional<AssetPolicy> policy,
                                               std::span<const int> inputWidths, int outputWidth,
                                               int instances);
    // bindRoutine's non-template core: validate the address and the binding against the machine and
    // store the resolved binding against the address it names, returning its handle.
    std::size_t bindRoutineResolved(std::uint32_t at, const RoutineBinding& binding,
                                    std::span<const int> inputWidths, int outputWidth);
    std::uint64_t invoke(std::size_t handle, std::span<const CallValue> inputs);

    // Perform one declared Instruction on the hosted resident driver: a mailbox write (returns 0), or
    // an entry call run to return with the given cycle cap (returns the CPU cycles consumed). The value
    // it carries is the Instruction's fixed value when set, else 0 — the audio handle bakes the play(id)
    // value into a fixed-value clone before queuing, so a queued Instruction is always fully determined.
    std::uint64_t performInstruction(const Instruction& instruction, std::uint64_t cycleCap);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The nested platform-bound Vm types (declared above): a Game Boy and a Game Boy Color VM with their
// platform + timing pre-bound. No new state — construction is the only thing they fix.
class Vm::GB : public Vm {
public:
    GB() : Vm(&detail::gameBoyCore, VMPlatform::GameBoy, TimingProfile::GameBoy, VmConfig{}) {}
    explicit GB(VmConfig config)
        : Vm(&detail::gameBoyCore, VMPlatform::GameBoy, TimingProfile::GameBoy, std::move(config)) {}
};

class Vm::GBC : public Vm {
public:
    GBC() : Vm(&detail::gameBoyCore, VMPlatform::GameBoyColor, TimingProfile::GameBoyColor, VmConfig{}) {}
    explicit GBC(VmConfig config)
        : Vm(&detail::gameBoyCore, VMPlatform::GameBoyColor, TimingProfile::GameBoyColor,
             std::move(config)) {}
};

// ── Template definitions ──────────────────────────────────────────────────────────────────────

template <class S>
RegionMapId<S> Vm::registerRegions(const RegionMap<S>& map) {
    std::vector<DeclaredRegion>    flat;
    std::vector<MemoryRegion S::*> keys;
    std::vector<MemoryRegion>      declarations;
    flat.reserve(map.bindings.size());
    keys.reserve(map.bindings.size());
    declarations.reserve(map.bindings.size());
    for (const RegionBinding<S>& b : map.bindings) {
        flat.push_back(DeclaredRegion{.where = b.where, .name = b.name});
        keys.push_back(b.key);
        declarations.push_back(b.where);
    }
    const std::size_t handle = registerRegionsResolved(flat);
    return RegionMapId<S>{handle, std::move(keys), std::move(declarations)};
}

template <class S>
std::vector<std::uint8_t> Vm::read(const RegionMapId<S>& map, MemoryRegion S::* key,
                                   std::uint32_t index) {
    const std::optional<MemoryRegion> where = map.declared(key);
    if (!where.has_value()) {
        throw std::invalid_argument("read: that field is not one of this batch's declared places");
    }
    return read(*where, index);
}

template <class S>
void Vm::write(const RegionMapId<S>& map, MemoryRegion S::* key,
               std::span<const std::uint8_t> bytes, std::uint32_t index) {
    const std::optional<MemoryRegion> where = map.declared(key);
    if (!where.has_value()) {
        throw std::invalid_argument("write: that field is not one of this batch's declared places");
    }
    write(*where, bytes, index);
}

// Decomposes a function-type Sig into the per-argument widths and the return width the non-template
// core needs. Only Ret(Args...) is valid; the primary is left undefined.
template <typename Sig>
struct RoutineSignature;

template <typename Ret, typename... Args>
struct RoutineSignature<Ret(Args...)> {
    static std::array<int, sizeof...(Args)> inputWidths() {
        return {static_cast<int>(sizeof(Args))...};
    }
    static constexpr int outputWidth() {
        if constexpr (std::is_void_v<Ret>) {
            return 0;
        } else {
            return static_cast<int>(sizeof(Ret));
        }
    }
};

template <typename Sig>
Routine<Sig> Vm::uploadRoutine(std::span<const std::uint8_t> routineBytes,
                               const RoutineBinding& binding, int instances) {
    const auto widths = RoutineSignature<Sig>::inputWidths();
    const std::size_t handle = registerResolved(
        routineBytes, binding, std::span<const int>(widths),
        RoutineSignature<Sig>::outputWidth(), instances);
    return Routine<Sig>{this, handle};
}

template <typename Sig>
Routine<Sig> Vm::registerRoutine(LiteralPath asmFilePath, const RoutineBinding& binding,
                                 std::optional<AssetPolicy> policy, int instances) {
    const auto widths = RoutineSignature<Sig>::inputWidths();
    const std::size_t handle = registerRoutineResolvingPolicy(
        asmFilePath.view(), binding, policy, std::span<const int>(widths),
        RoutineSignature<Sig>::outputWidth(), instances);
    return Routine<Sig>{this, handle};
}

template <typename Sig>
Routine<Sig> Vm::bindRoutine(std::uint32_t at, const RoutineBinding& binding) {
    const auto widths = RoutineSignature<Sig>::inputWidths();
    const std::size_t handle = bindRoutineResolved(at, binding, std::span<const int>(widths),
                                                   RoutineSignature<Sig>::outputWidth());
    return Routine<Sig>{this, handle};
}

template <typename Ret, typename... Args>
Ret Routine<Ret(Args...)>::operator()(Args... args) const {
    const std::array<CallValue, sizeof...(Args)> inputs{
        CallValue{static_cast<std::uint64_t>(args), static_cast<int>(sizeof(Args))}...};
    const std::uint64_t result = vm_->invoke(handle_, std::span<const CallValue>(inputs));
    if constexpr (!std::is_void_v<Ret>) {
        return static_cast<Ret>(result);
    }
}

}  // namespace retropp
