// The default SNES sound driver's registration: its three images from the bytes the build assembled, placed
// where their sources say, the init that uploads the sound program, the tick, the two mailboxes as its
// player verbs and the two bytes it reports as its slots.
#include "src/audio/snes/default_driver.h"

#include <cstdint>
#include <span>

#include "retropp/audio_library.h"
#include "retropp/driver_binding.h"
#include "retropp/generated/snes_sound_driver_init.h"
#include "retropp/generated/snes_sound_driver_sound.h"
#include "retropp/generated/snes_sound_driver_tick.h"
#include "retropp/isa.h"
#include "retropp/snes.h"  // snes::A — the register the init call rides

namespace retropp::audio::snesdriver {

DriverId<Slots> defaultDriver() {
    static const DriverId<Slots> id = [] {
        DriverBinding binding;
        binding.images = {
            DriverImage{.bytes = std::span<const std::uint8_t>(generated::snesSoundDriver_init),
                        .base  = generated::snesSoundDriver_init_origin},
            DriverImage{.bytes = std::span<const std::uint8_t>(generated::snesSoundDriver_tick),
                        .base  = generated::snesSoundDriver_tick_origin},
            // The sound program sits in the console image at $00:A000, where the init reads it from to
            // upload it; its own origin, $0200, is where it runs on the sound CPU.
            DriverImage{.bytes = std::span<const std::uint8_t>(generated::snesSoundDriver_sound),
                        .base  = 0x00A000},
        };
        binding.tickEntry = generated::snesSoundDriver_tick_origin;
        binding.init      = Instruction::call(generated::snesSoundDriver_init_origin, snes::A, /*fixedValue=*/0);
        binding.isa       = Isa::Wdc65816;
        const DriverVerbs verbs{
            .play = {.music = Instruction::write(Location::memory(kPlayMailbox), 1)},
            .stop = Instruction::write(Location::memory(kStopMailbox), 1, /*fixedValue=*/1),
        };
        return AudioLibrary::instance().uploadDriver(
            binding, verbs,
            slots(slot(&Slots::ended, kEnded, SlotDirection::Read),
                  slot(&Slots::frames, kFrames, SlotDirection::Read)));
    }();
    return id;
}

}  // namespace retropp::audio::snesdriver
