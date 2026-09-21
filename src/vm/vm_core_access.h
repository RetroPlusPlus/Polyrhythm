#pragma once

#include "retropp/vm.h"

// Builds a Vm from a core that is already resolved. The audio system keeps the core its game named and
// builds each voice's machine from it. INTERNAL — under src/vm/; never in include/retropp/.
namespace retropp::vm {
struct VmCoreAccess {
    [[nodiscard]] static Vm make(detail::CoreFactory core, VMPlatform platform, TimingProfile timing) {
        return Vm(core, platform, timing, VmConfig{});
    }
};
}  // namespace retropp::vm
