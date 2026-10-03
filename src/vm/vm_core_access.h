#pragma once

#include <memory>
#include <optional>

#include "retropp/vm.h"
#include "src/vm/vm_backend.h"

// Builds a Vm from a core that is already resolved. The audio system keeps the core its game named and
// builds each voice's machine, and reads that machine's clock, from it. INTERNAL — under src/vm/; never in include/retropp/.
namespace retropp::vm {
struct VmCoreAccess {
    [[nodiscard]] static Vm make(detail::CoreFactory core, VMPlatform platform, TimingProfile timing) {
        return Vm(core, platform, timing, VmConfig{});
    }

    // The machine as its audio unit alone — what an AudioSystem of the HostDriven kind hosts. A core whose
    // audio is not a unit of its own throws std::logic_error here.
    [[nodiscard]] static Vm makeAudioUnit(detail::CoreFactory core, VMPlatform platform, TimingProfile timing);

    // The clock of the machine this core builds for `platform`. The audio system sizes a voice's
    // step from it once, before any voice exists.
    [[nodiscard]] static std::optional<MachineClock> clockOf(detail::CoreFactory core,
                                                             VMPlatform platform) {
        return core(platform)->clock();
    }
    // The clock of the audio unit alone: the machine's own, which is not the console's.
    [[nodiscard]] static std::optional<MachineClock> audioUnitClockOf(detail::CoreFactory core,
                                                                      VMPlatform platform) {
        std::unique_ptr<VmBackend> backend = core(platform);
        backend->hostAudioUnit();
        return backend->clock();
    }
};
}  // namespace retropp::vm
