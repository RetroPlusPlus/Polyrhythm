#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "retropp/isa.h"  // Isa — the instruction set a placed routine's file name states

// Routine delivery runtime state, the routine analog of asset_registry.h's EMBED table. Backs
// path-based registration (registerRoutine(LiteralPath, …) / AudioLibrary::registerAudio(LiteralPath, …));
// passing a raw byte span (uploadRoutine / uploadAudio) bypasses it entirely.
//
//   Embed (default) — the .asm is assembled to BYTECODE AT COMPILE TIME (the constexpr SM83 assembler)
//                     and ONLY the bytecode ships; the .asm is never stored in the binary nor read at
//                     runtime. The build scan bakes each Embed routine's bytes and
//                     records them here, keyed by logical path; path-based registration looks them up and hands
//                     the span to the RAW path.
//   LoadFromPath    — the .asm ships beside the binary and is read + assembled ONCE at startup, in
//                     memory, never baked — for a copyright-derived routine or a developer's load-by-
//                     choice. There is NO separate routine root: a LoadFromPath path is a full project-
//                     root-relative LITERAL resolved against the platform's single assetRoot() (asset_registry.h),
//                     exactly like loadAtlas / loadMapPng.
//
// LEAN BINARY (locked): pure data — a path → bytes map with NO dependency on Vm or the VM backend, so it
// never force-links the VM. Only routines with a scanned call site are baked and recorded; an unused
// routine is never assembled, baked, or linked in. The platform's own presets pass raw bytes directly
// (compile-time-baked arrays odr-used only by their preset fn → dropped if unused) and bypass this.

namespace retropp {

namespace detail {

// Where a routine assembled by an absolute assembler goes: the ISA its file name states, and the address
// its bytes were assembled for.
struct EmbeddedPlacement {
    Isa           isa;
    std::uint32_t origin;
};

// Record that a logical routine path resolves to embedded bytecode (a constexpr array valid for the
// program lifetime). Called before main() from the auto-generated per-target registry TU
// (retropp_autoembed, extended to routines) — emitted ONLY for routines with a scanned
// call site, so an unused routine is never recorded here. A `.asm` whose name states an ISA
// (`<name>.65816.asm`, `<name>.spc700.asm`) is recorded with its placement; a bare `.asm` is SM83,
// lands wherever the arena has room, and carries none.
void registerEmbeddedRoutine(std::string_view path, const std::uint8_t* bytes, std::size_t size,
                             std::optional<EmbeddedPlacement> placement = std::nullopt);

// The embedded bytecode registered for `path`, or an empty span if none was baked for it. Path-based
// registration under an Embed policy reports an empty span through asset_registry.h's warnEmbedNotBaked
// and then reads the .asm from assetRoot(), so a literal path still resolves while the bake is missing.
[[nodiscard]] std::span<const std::uint8_t> findEmbeddedRoutine(std::string_view path);

// The placement recorded for `path`, or nothing when the routine was recorded without one (or not at all).
[[nodiscard]] std::optional<EmbeddedPlacement> findEmbeddedRoutinePlacement(std::string_view path);

}  // namespace detail
}  // namespace retropp
