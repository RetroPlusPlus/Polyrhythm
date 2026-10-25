#pragma once

// Game Boy / SameBoy routine presets — the GB-family catalog of standard original-
// hardware routines with a fixed I/O convention, so a consumer registers one BY MEANING with zero
// binding boilerplate:
//
//     retropp::Vm vm{retropp::VMPlatform::GameBoyColor};
//     auto rng = retropp::sameboy::divRng(vm);   // no bytes, no address, no register — just the Vm
//     std::uint8_t roll = rng();
//
// A preset earns its place here only when the routine is a HARDWARE TECHNIQUE rather than anyone's
// algorithm — an operation whose SM83 form is dictated by the instruction set and the hardware
// register it touches, so any independent implementation writes the same instructions. Each is
// authored as a real assembly FILE under src/vm/gameboy/routines/ and assembled BY THE COMPILER (the
// constexpr SM83 assembler bakes it into the binary — see gb_routine_bytecode.h), so a preset
// registers from a baked byte span and the consumer supplies nothing but the Vm&.
//
// AN ALGORITHM IS THE GAME'S, NOT THE PLATFORM'S. Anything with design choices in it — a mixing
// scheme, a seed layout, a compression format — belongs in the game that authors it, whatever its
// provenance. Point Vm::registerRoutine at your own .asm file (or hand pre-assembled bytes to
// uploadRoutine); examples/vm_routines does exactly that with an RNG of its own.
//
// Software RNGs that read no hardware register are intentionally not here either, for a different
// reason: they are byte-reproducible in native C++, so they are a porting job rather than a VM case.

#include <cstdint>

#include "retropp/vm.h"

namespace retropp::sameboy {

// divRng — the Game Boy entropy technique: a direct read of the free-running DIV register (rDIV,
// $FF04) as the random byte. Two instructions, and the instruction set admits no other way to write
// them. Stateless, so the stream is only as varied as the divider — a game wanting a mixed stream
// folds this into a seed of its own design (examples/vm_routines).
// No inputs; returns the rDIV byte in A. Source: src/vm/gameboy/routines/div_rng.asm.
Routine<std::uint8_t()> divRng(Vm& vm);

// NOTE: there is intentionally no audio routine preset here. A game configures audio through the
// AudioSystem (retropp/audio_system.h): it registers its sound driver and sounds THERE, in audio terms,
// and the AudioSystem hosts them on its own internal VM. What the AudioSystem hides is the VM/Routine/
// throttle layer — not the act of registering audio (that is the game's job and fully exposed). The
// diagnostic square-wave driver (src/vm/gameboy/routines/tone.asm) is registered inside the
// AudioSystem, not exposed as a VM preset.

}  // namespace retropp::sameboy
