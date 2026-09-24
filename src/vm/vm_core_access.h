#pragma once

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

    // The clock of the machine this core builds for `platform`. The audio system sizes a voice's
    // step from it once, before any voice exists.
    [[nodiscard]] static std::optional<MachineClock> clockOf(detail::CoreFactory core,
                                                             VMPlatform platform) {
        return core(platform)->clock();
    }
};
}  // namespace retropp::vm
