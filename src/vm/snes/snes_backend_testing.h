// Internal test access to SnesBackend's live machine — reads the SnesState the four suites assert against
// without a public accessor. VmTestAccess's shape: a friend struct under src/vm/, never in
// include/retropp/. TEST observation only.
#ifndef RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H
#define RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H

#include "snaggletooth/snes/snes.h"
#include "src/vm/snes/snes_backend.h"

namespace retropp::vm {

struct SnesBackendTestAccess {
    // The backend's live machine state, by const reference (a SnesState is a quarter of a megabyte). A
    // held word reaches state().joy; two machines' byte-identical state compares two of these.
    [[nodiscard]] static const snaggletooth::SnesState& state(const SnesBackend& backend) {
        return backend.snes_->state();
    }
    [[nodiscard]] static bool hosting(const SnesBackend& backend) noexcept {
        return backend.snes_.has_value();
    }
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H
