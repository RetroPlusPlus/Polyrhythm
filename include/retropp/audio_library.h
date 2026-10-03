#pragma once

// The AudioLibrary: the one place a project's registered audio lives.
//
// SINGLE INSTANCE BY CONSTRUCTION. There is exactly one AudioLibrary per program, reached through
// AudioLibrary::instance(). The constructor is private and copying is deleted, so a second one cannot be
// declared — "more than one library" is a compile error, not a runtime check (the Swift-singleton shape).
//
// OPTIONAL + LEAN. instance() is a function-local static: it is materialized only when first referenced,
// so a project that registers no audio at all — a pure-render demoscene / digital-art program with no
// AudioSystem — never links it in. You pay for the catalog only if you use it.
//
// ITS OWN STORAGE — no VM, no Routine, no machine type. The library holds portable audio DEFINITIONS (the
// driver bytecode for a chiptune, or the .asm path to assemble; or a PCM audio-file path the PCM backend
// decodes and streams) plus each one's tags, and hands out an AudioId per registration. How a
// definition becomes sound is the
// AudioSystem's job — it places the definition into the VM it owns, on play — so the library never touches
// a VM, which also keeps it from force-linking one. This header therefore pulls no vm.h.
//
// Two ways to register, each returning an AudioId:
//   * uploadAudio   — pass already-assembled chiptune bytecode; the library copies and owns it. No policy
//                     (you brought the bytes). Always a chiptune.
//   * registerAudio — pass a file path; the Embed / LoadFromPath policy decides whether the build bakes the
//                     bytes into the binary or ships the file beside it. The kind is inferred from the
//                     extension: `.asm` is a chiptune, `.wav` / `.ogg` / `.flac` / `.mp3` is PCM.
// The kind is frozen into the entry at registration.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "retropp/asset_policy.h"    // AssetPolicy (a register* path entry carries its per-call policy)
#include "retropp/driver_binding.h"  // DriverBinding / DriverImage / SlotSpec / Instruction / Mapper — the
                                     // untyped machine substrate a hosted driver registration stores
#include "retropp/isa.h"             // Isa (the developer selects the ISA a chiptune is written for, here)
#include "retropp/literal_path.h"    // LiteralPath (registerAudio takes a compile-time literal path)

namespace retropp {

// How a registered audio is used — the routing tag, also the mixer bus (retropp/audio_mixer.h) a source
// is scaled by. Stored on every entry; a track is on whichever bus its type names. With a single instance,
// playback routing is the original hardware's natural channel-stealing. Music and Vocals are sustained —
// the game opens and closes them and they are never auto-closed; Sfx is fire-and-forget and auto-closes
// when its output goes silent. Vocals is simply a third bus alongside Music and Sfx — a separate volume
// channel a game can tag voice/dialogue-style audio with; it is not tied to any particular kind (chiptune
// or PCM). Like Music, it is sustained.
//
// VMDriver is the hosted-driver bus: a resident sound driver (below) rides it as a straight amplifier over
// its own authentic internal mixing. It is AudioType::VMDriver BY CONSTRUCTION for every driver
// registration — never passed as a routing argument — and, like Music, sustained (never auto-closed; the
// driver closes only through its handle or system destruction). It exists so a game can set the hosted
// driver's overall level independently of the cue buses.
enum class AudioType { Music, Sfx, Vocals, VMDriver };

// WHAT a registered audio is, inferred once at registration and frozen into its entry: a chiptune driver
// (runs on the VM) or PCM (a WAV/OGG audio file the PCM backend decodes and streams; no VM). PCM defaults
// to LoadFromPath — the file ships beside the binary rather than being baked in; embedding PCM is an
// opt-in override (a multi-MB track is not baked automatically). Chiptune defaults the other way — Embed —
// because a driver is hundreds of bytes. Orthogonal to AudioType (kind = what it is; type = how it routes).
//
// Driver is a hosted RESIDENT sound driver (the game's own engine, run as a long-lived addressable machine
// on the VM — retropp/driver_binding.h). Registered through uploadDriver / registerDriver (below), it is
// AudioType::VMDriver by construction and hosted through AudioSystem::host(), never play()'d.
//
// HostDriven is an audio system's kind, never a registration's: the console's audio unit alone, with no
// console CPU built around it, driven by the game. The SNES has one — its sound CPU and sound chip are a
// unit of their own beside the console — so `AudioSystem::SNES{AudioKind::HostDriven}` hosts that unit.
enum class AudioKind { Chiptune, Pcm, Driver, HostDriven };

// An opaque handle to a registered audio, minted by the AudioLibrary and cued with AudioSystem::play().
// Its lifetime is the library's (the whole program) — the AtlasId / PaletteId value-handle contract.
enum class AudioId : std::uint32_t {};

namespace detail {
// Case-sensitive suffix match (logical asset paths are authored lowercase). constexpr so the kind
// inference below — and the compile-time check on ChiptunePath — run the same code.
[[nodiscard]] constexpr bool endsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace detail

// Infer the audio KIND from a path's extension: `.asm` (ISA assembly source) is a Chiptune; an audio
// container (`.wav` / `.ogg` / `.flac` / `.mp3`), or a `.brr` sample in the SNES sound chip's own format,
// is PCM. Anything else is taken as Chiptune. Used when registering by path; uploadAudio (bytecode) is
// always a chiptune. constexpr: the one predicate drives both the runtime inference and the compile-time
// ChiptunePath check.
[[nodiscard]] constexpr AudioKind audioKindForExtension(std::string_view path) noexcept {
    if (detail::endsWith(path, ".wav") || detail::endsWith(path, ".ogg") ||
        detail::endsWith(path, ".flac") || detail::endsWith(path, ".mp3") ||
        detail::endsWith(path, ".brr")) {
        return AudioKind::Pcm;
    }
    return AudioKind::Chiptune;  // `.asm` and anything else
}

// A compile-time literal path for the ISA (chiptune) registerAudio overload. Its consteval constructor
// rejects any literal that resolves to PCM — a `.wav` / `.ogg` / `.mp3` / `.flac` audio file plays without
// the VM and has no ISA, so passing one to the ISA overload is a category error. Reaching the throw during
// the required constant evaluation is ill-formed, so such a literal does not compile; a non-PCM literal
// (`.asm` and anything else) constructs normally and keeps the natural `f("song.asm")` call syntax — the
// wrong overload is a compile error, not a runtime one. (Built on LiteralPath, so the build-time asset
// scan still sees the literal text verbatim.)
class ChiptunePath {
public:
    template <std::size_t N>
    consteval ChiptunePath(const char (&literal)[N])  // NOLINT(google-explicit-constructor)
        : path_(literal) {
        if (audioKindForExtension(path_.view()) == AudioKind::Pcm) {
            throw "registerAudio with an Isa registers a chiptune; a .wav/.ogg/.mp3/.flac/.brr audio file "
                  "must use the no-Isa registerAudio overload";
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return path_.view(); }

private:
    LiteralPath path_;
};

// ── Driver hosting: typed registration vocabulary ─────────────────────────────────────────────────
//
// A game hosts its own resident sound driver — the machine-layer facts (placed images, mapper, tick entry,
// state slots, gestures) live in retropp/driver_binding.h. Registration on the library mints a
// DriverId<SlotsStruct>: an AudioId that remembers, at compile time, the game's SLOTS STRUCT — a plain
// value type whose std::optional<T> fields name the driver's readable/writable state. The typed handle
// recovered at host() (retropp/audio_system.h) speaks that struct, so a slot typo is a compile error and
// nothing is re-declared after registration.

// The slots struct for a driver that declares no state slots (the argument-family shape) — the default S.
struct NoSlots {};

// A registered driver's handle: an AudioId that carries its game's slots struct type. Minted by
// uploadDriver / registerDriver, consumed by AudioSystem::host(). NOT implicitly an AudioId — a driver is
// hosted, never play()'d — so the wrapped id is reached only through .id().
template <class SlotsStruct>
class DriverId {
public:
    constexpr explicit DriverId(AudioId id) noexcept : id_(id) {}
    [[nodiscard]] constexpr AudioId id() const noexcept { return id_; }

private:
    AudioId id_;
};

// A type-erased read/write pair over a game slots struct, captured by slot() with the field's type known,
// stored index-aligned with a driver's SlotSpecs for the typed handle. `s` is always a pointer to the
// SlotsStruct the DriverId carries — every slot in one batch shares that struct — so the casts are sound.
struct SlotAccessor {
    std::function<bool(const void* s, std::uint64_t& out)> read;   // engaged? → set out to the field value
    std::function<void(void* s, std::uint64_t value)>      write;  // set the field to a slot value
};

// One declared slot bound to a game struct field: the SlotSpec (address / width / direction) plus the
// typed accessors. Templated on the slots struct so every slot in one slots(...) batch belongs to the SAME
// struct — a field of another type does not compile, which is what makes the handle's void* casts sound.
template <class SlotsStruct>
struct SlotBinding {
    SlotSpec     spec;
    SlotAccessor accessor;
};

// Bind a game slots-struct field to a driver state slot. The field's optional value type carries the slot
// WIDTH (std::optional<std::uint8_t> → 1 byte, std::optional<std::uint16_t> → 2); `address` is the console
// address the driver reads/writes; `direction` gates reads vs writes. Returns a binding that lowers to a
// SlotSpec and captures the typed accessors the handle needs.
template <class S, class T>
[[nodiscard]] SlotBinding<S> slot(std::optional<T> S::* member, std::uint32_t address,
                                  SlotDirection direction = SlotDirection::ReadWrite) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "a driver slot field must be an integral or enum std::optional (a console flag or word)");
    static_assert(sizeof(T) <= 8, "a driver slot is at most 8 bytes wide");
    SlotBinding<S> b;
    b.spec = SlotSpec{.address = address, .width = static_cast<int>(sizeof(T)), .direction = direction};
    b.accessor.read = [member](const void* sv, std::uint64_t& out) -> bool {
        const std::optional<T>& opt = static_cast<const S*>(sv)->*member;
        if (!opt.has_value()) {
            return false;
        }
        out = static_cast<std::uint64_t>(*opt);
        return true;
    };
    b.accessor.write = [member](void* sv, std::uint64_t value) {
        static_cast<S*>(sv)->*member = static_cast<T>(value);
    };
    return b;
}

// The declared slot batch for a driver — slot() bindings, all of the same slots struct S. A driver with no
// slots passes none (uploadDriver<NoSlots>(binding)); S then defaults to NoSlots.
template <class S>
struct DriverSlots {
    std::vector<SlotBinding<S>> bindings;
};

// Collect one or more slot() bindings into a DriverSlots<S>. S is deduced from the first binding; every
// other must be the same slots struct (a slot of a different struct will not compile — the point of the
// typed vocabulary).
template <class S, class... Rest>
[[nodiscard]] DriverSlots<S> slots(SlotBinding<S> first, Rest... rest) {
    DriverSlots<S> out;
    out.bindings.reserve(1 + sizeof...(rest));
    out.bindings.push_back(std::move(first));
    (out.bindings.push_back(std::move(rest)), ...);
    return out;
}

// ── Driver hosting: the player verbs ────────────────────────────────────────────────────────────
//
// A hosted driver declares, once at registration beside slots(...), HOW its player verbs realize on this
// specific machine — so the handle's play(id[, lane]) / stop() call sites carry no machine idiom (the same
// verbs as AudioSystem). Each realization is an Instruction (retropp/driver_binding.h): a write (the
// RAM-flag mailbox family — the id lands in memory the driver polls) or a call (the argument family — the
// id rides a CPU register into an entry). The handle bakes play(id)'s id into the chosen realization's
// value and the platform performs it at the tick boundary.

// The per-lane play realizations — the routing table play(id, lane) keys into, one Instruction per lane.
// Music is REQUIRED (a driver you cannot cue music on is not playable — validated loud at registration);
// Sfx and Vocals are OPTIONAL lanes (a driver with no separate SFX / cry entry leaves them unset, and
// play(id, AudioType::Sfx) on such a driver is a loud error). The AudioType lane keys map a driver's tables
// exactly — a typical trio is Music → a play-music entry, Sfx → a play-effect entry, Vocals → a play-cry
// entry. (The lane key
// is an AudioType because driver_binding.h — below the audio layer — cannot name one; the verbs are the
// audio layer's typing over that untyped Instruction substrate.)
struct PlayVerbs {
    std::optional<Instruction> music;
    std::optional<Instruction> sfx;
    std::optional<Instruction> vocals;
};

// A hosted driver's player verbs. `.play` is the per-lane routing table (above); `.stop` is the stop()
// realization — usually a mailbox write of a fixed "silence" value — and is OPTIONAL, since not every
// driver exposes a single-gesture stop (handle.stop() is a loud error when none is declared). Passed as a
// third argument to both registration functions, beside slots(...); anti-channel-stealing's future per-lane
// instance routing is one more column in this same table, so call sites never change.
struct DriverVerbs {
    PlayVerbs                  play;
    std::optional<Instruction> stop;
};

// A driver image sourced from a per-image path (registerDriver — the legal-posture path). A
// `.asm` path assembles in the driver's ISA at host(); any other extension is read as raw image bytes.
// `policy` selects Embed (baked) or LoadFromPath (ships beside the binary, read at runtime — the posture
// for copyright-derived driver content, which is never embedded). Unset defaults to Embed (a small image),
// overridden per image by naming a policy. `isa` is the same shape for the instruction set: unset, the
// image is written in the binding's own `isa`; named, it is written in that one — the SPC700 program a
// 65816 driver uploads to the SNES's audio unit, beside the 65816 code that uploads it — and the build
// bakes an Embed image with that instruction set's assembler.
struct DriverImagePath {
    std::uint32_t              base = 0;
    LiteralPath                path;
    std::optional<AssetPolicy> policy{};
    std::optional<Isa>         isa{};
};

// One image of a hosted-driver registration, given either way: a per-image PATH (above — the build resolves
// it under that image's own policy) or inline BYTES (DriverImage, retropp/driver_binding.h — the
// registration copies them). A binding mixes the two image by image, which is how a driver pairs
// port-authored startup code baked from a path with a section read at runtime out of content the game may
// not ship inside its binary.
//
// A policy on a byte image is unrepresentable: DriverImage carries no policy field, so an image cannot name
// a policy that would mean nothing. A byte image's span need only outlive the registration call — the
// library copies it, so whatever produced the bytes may be destroyed before host() runs.
using DriverImageSource = std::variant<DriverImagePath, DriverImage>;

// The registerDriver input: the machine facts with images given as declared SOURCES — each one a path or
// inline bytes. Slots are declared separately (the slots(...) argument); this carries only the placement +
// tick + mapper + init + isa the untyped substrate needs.
//
// DriverBinding (retropp/driver_binding.h) is the layer below and is a different type on purpose: the
// untyped machine substrate every hosted driver lowers into, its images already resolved to bytes. This is
// what a game declares; that is what the machine consumes.
struct HostedDriverBinding {
    std::vector<DriverImageSource> images;
    Mapper                         mapper{};
    std::uint32_t                  tickEntry = 0;
    std::optional<std::uint32_t>   stackTop{};
    std::optional<Instruction>     init{};
    Isa                            isa = Isa::Sm83;
};

// A stored driver image: exactly one source is populated — `bytes` (uploadDriver: an owned copy of the
// image span) or `path` + `policy` (registerDriver: resolved to bytes at host()). `base` is the placement.
struct StoredDriverImage {
    std::uint32_t              base = 0;
    std::vector<std::uint8_t>  bytes;    // uploadDriver — owned; empty for a path image
    std::string                path;     // registerDriver — per-image source path; empty for a byte image
    std::optional<AssetPolicy> policy{}; // registerDriver — per-image embed/load policy
};

// What the library stores for a hosted-driver registration: the untyped machine facts (the DriverBinding
// substrate, minus the images which become StoredDriverImages so the bytes are owned) plus the type-erased
// slot accessors the typed handle recovers via the DriverId's slots struct. isa lives on the Entry.
struct DriverDefinition {
    std::vector<StoredDriverImage> images;
    Mapper                         mapper{};
    std::uint32_t                  tickEntry = 0;
    std::optional<std::uint32_t>   stackTop{};
    std::vector<SlotSpec>          slots;      // lowered untyped specs, declaration order
    std::optional<Instruction>     init{};
    std::vector<SlotAccessor>      accessors;  // index-aligned with slots (typed on the DriverId's struct)
    DriverVerbs                    verbs;      // the per-lane play + stop realizations (music required)
};

class AudioLibrary {
public:
    // The one library — see the header note (single by construction, lean, optional). A function-local
    // static; returns the same object on every call.
    static AudioLibrary& instance();

    AudioLibrary(const AudioLibrary&)            = delete;
    AudioLibrary& operator=(const AudioLibrary&) = delete;

    // What the library stores per AudioId: the kind / type / ISA tags, the per-call embed/load policy
    // (path entries), and the portable chiptune definition — EXACTLY ONE of `bytecode` (the raw / Embed
    // form, owned by the library) or `asmPath` (the path / LoadFromPath form) is populated for a Chiptune
    // entry. `isa` is the ISA a Chiptune's bytes/source are written for, SELECTED BY THE DEVELOPER at
    // registration (ignored for PCM): AudioSystem::play() rejects an AudioId cued on a VM of a different
    // ISA. The AudioSystem reads this to place the driver into the VM it owns; a developer never inspects it.
    struct Entry {
        AudioKind                  kind;
        AudioType                  type;
        Isa                        isa;       // the ISA a Chiptune is written for (ignored for PCM)
        std::optional<AssetPolicy> policy;    // path entries: per-call embed/load policy (nullopt = default)
        std::vector<std::uint8_t>  bytecode;  // chiptune ready bytes (uploadAudio / baked Embed) — owned
        std::string                asmPath;   // chiptune source path (registerAudio / LoadFromPath)
        std::optional<DriverDefinition> driver{};  // AudioKind::Driver only — the hosted-driver definition
    };

    // Register a chiptune from pre-assembled `bytecode` (written for ISA `isa`): copy it into the library's
    // own storage, tag it, and return a handle. The bytes are owned by the library from here on (the span
    // need not outlive the call). No embed/load policy — you brought the bytes.
    AudioId uploadAudio(std::span<const std::uint8_t> bytecode, AudioType type, Isa isa);

    // Register a chiptune from a `.asm` path, assembled on first play for the ISA `isa` it targets, with
    // its per-call embed/load `policy`; returns a handle. ISA is a chiptune-only concept — `isa` is the
    // developer-selected compatibility unit that gates the chiptune at play() (a cue on a VM of a different
    // ISA throws). Only chiptune registration takes an ISA. The path is a ChiptunePath: a PCM-extension
    // literal (`.wav` / `.ogg` / `.mp3` / `.flac` — an audio file that runs without the VM and has no ISA)
    // does not compile here; use the no-ISA overload below for those.
    //
    // Templated on the ISA argument purely to gate candidacy: a no-ISA call deduces a non-Isa third
    // argument, fails the constraint, and is dropped from the candidate set BEFORE the ChiptunePath
    // conversion is formed — so a legitimate `.wav` / `.ogg` registration resolves to the no-ISA overload
    // instead of failing this one's compile-time path check. With an actual Isa, this is the only match.
    template <class IsaT>
        requires std::is_same_v<IsaT, Isa>
    AudioId registerAudio(ChiptunePath resourcePath, AudioType type, IsaT isa,
                          std::optional<AssetPolicy> policy = {}) {
        entries_.push_back(Entry{
            .kind     = AudioKind::Chiptune,  // ChiptunePath rejected any PCM extension at compile time
            .type     = type,
            .isa      = isa,
            .policy   = policy,
            .bytecode = {},
            .asmPath  = std::string(resourcePath.view()),
        });
        return static_cast<AudioId>(entries_.size() - 1);
    }

    // Register a PCM audio file (`.wav` / `.ogg` / `.flac` / `.mp3`) from a path, decoded and streamed on
    // first play, with its per-call embed/load `policy`; returns a handle. No ISA — a decoded audio file
    // runs no code, so ISA (a chiptune-only concept) is meaningless here. The kind is inferred from the
    // extension as Pcm; the entry plays on an AudioKind::Pcm system.
    AudioId registerAudio(LiteralPath resourcePath, AudioType type,
                          std::optional<AssetPolicy> policy = {});

    // Register a hosted sound driver from INLINE image bytes (the uploadAudio analog): copy each image's
    // bytes into the library, record the player `verbs`, lower the declared slots to the untyped substrate,
    // capture the typed accessors, and mint a DriverId that remembers the game's slots struct S. A driver
    // is AudioType::VMDriver by construction — no routing type is passed; the ISA is a field of the
    // binding, verified at host(). `verbs.play.music` is required (a driver you cannot cue music on is not
    // playable — throws otherwise). S is deduced from the slots batch; a driver with no slots omits it
    // (uploadDriver<NoSlots>(binding, verbs)). The image spans need not outlive the call. Declaring slots on
    // the binding itself (DriverBinding.slots) at this registration function is an error — use the slots(...) argument.
    template <class S = NoSlots>
    DriverId<S> uploadDriver(const DriverBinding& binding, const DriverVerbs& verbs,
                             const DriverSlots<S>& slots = {}) {
        if (!binding.slots.empty()) {
            throw std::invalid_argument(
                "declare a hosted driver's slots through the slots(...) argument, not DriverBinding.slots");
        }
        requireMusicVerb(verbs);
        DriverDefinition def = lowerMachine(binding.mapper, binding.tickEntry, binding.stackTop, binding.init);
        def.verbs = verbs;
        def.images.reserve(binding.images.size());
        for (const DriverImage& img : binding.images) {
            def.images.push_back(storeBytes(img));
        }
        lowerSlots(slots, def);
        return DriverId<S>(storeDriver(std::move(def), binding.isa));
    }

    // Register a hosted sound driver whose images ship as per-image PATHS (the registerAudio analog — the
    // legal-posture registration): store each image's path + policy (resolved to bytes at host()), record the player
    // `verbs`, lower the slots, capture the typed accessors, and mint the typed DriverId. Copyright-derived
    // driver content uses AssetPolicy::LoadFromPath and is never embedded. `verbs.play.music` is required.
    // Same S-deduction as uploadDriver.
    template <class S = NoSlots>
    DriverId<S> registerDriver(const HostedDriverBinding& binding, const DriverVerbs& verbs,
                               const DriverSlots<S>& slots = {}) {
        requireMusicVerb(verbs);
        DriverDefinition def = lowerMachine(binding.mapper, binding.tickEntry, binding.stackTop, binding.init);
        def.verbs = verbs;
        def.images.reserve(binding.images.size());
        for (const DriverImageSource& img : binding.images) {
            def.images.push_back(std::visit(
                [](const auto& source) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(source)>, DriverImagePath>) {
                        return StoredDriverImage{
                            .base   = source.base,
                            .bytes  = {},
                            .path   = std::string(source.path.view()),
                            .policy = source.policy,
                        };
                    } else {
                        return storeBytes(source);
                    }
                },
                img));
        }
        lowerSlots(slots, def);
        return DriverId<S>(storeDriver(std::move(def), binding.isa));
    }

    // How many audio resources are registered — also the next AudioId to be minted. (The library
    // accumulates for the life of the program; ids are dense and ascending from 0.)
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // The stored definition for `id`. Precondition: static_cast<std::size_t>(id) < size().
    [[nodiscard]] const Entry& entry(AudioId id) const;

private:
    AudioLibrary() = default;

    // Build a DriverDefinition holding the shared machine facts (everything but images + slots) — the
    // common lowering both driver registration functions start from before they add their own image source.
    static DriverDefinition lowerMachine(Mapper mapper, std::uint32_t tickEntry,
                                         std::optional<std::uint32_t> stackTop,
                                         std::optional<Instruction> init);

    // Lower one byte image into its stored form, copying the span into the library's own storage — the copy
    // is what lets the caller destroy whatever produced the bytes before host() runs.
    //
    // An empty span is refused here, on the thread that registers, naming the image's base. A driver image
    // with no bytes is a source that did not load, and the stored form discriminates on exactly this
    // emptiness — an empty one reaches host() as a path image with no path and fails on the audio
    // production thread, where a game has no seam to catch it.
    static StoredDriverImage storeBytes(const DriverImage& img) {
        if (img.bytes.empty()) {
            throw std::invalid_argument("a hosted driver's byte image is empty (base " + hexAddress(img.base) +
                                        ") — the image's bytes did not load");
        }
        return StoredDriverImage{
            .base   = img.base,
            .bytes  = std::vector<std::uint8_t>(img.bytes.begin(), img.bytes.end()),
            .path   = {},
            .policy = {},
        };
    }

    // An address as it is written at a call site (`0x6000`), for an error message that names one.
    static std::string hexAddress(std::uint32_t address) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        std::string out = "0x";
        bool significant = false;
        for (int shift = 28; shift >= 0; shift -= 4) {
            const unsigned nibble = (address >> shift) & 0xFu;
            if (nibble != 0 || significant || shift == 0) {
                out.push_back(kDigits[nibble]);
                significant = true;
            }
        }
        return out;
    }

    // The one verb-shape validation both registration functions run: a driver must declare a Music play realization (a
    // driver with no way to cue music is not playable). Sfx / Vocals / stop stay optional.
    static void requireMusicVerb(const DriverVerbs& verbs) {
        if (!verbs.play.music.has_value()) {
            throw std::invalid_argument(
                "a hosted driver must declare its Music play verb (DriverVerbs{.play = {.music = ...}})");
        }
    }

    // Lower a typed slot batch into the definition: untyped SlotSpecs and the index-aligned accessors.
    template <class S>
    static void lowerSlots(const DriverSlots<S>& slots, DriverDefinition& def) {
        def.slots.reserve(slots.bindings.size());
        def.accessors.reserve(slots.bindings.size());
        for (const SlotBinding<S>& b : slots.bindings) {
            def.slots.push_back(b.spec);
            def.accessors.push_back(b.accessor);
        }
    }

    // Push a Driver entry (AudioType::VMDriver by construction; the routing bus arrives with the mixer's
    // VMDriver channel) and return its dense AudioId.
    AudioId storeDriver(DriverDefinition def, Isa isa);

    std::vector<Entry> entries_;
};

}  // namespace retropp
