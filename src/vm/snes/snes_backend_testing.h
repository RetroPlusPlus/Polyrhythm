// Internal test access to SnesBackend's live machine — reads the SnesState and the DSP queue the suites assert against
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
    // The DSP frames the machine has produced that nobody has taken. Zero after every step, with a sink
    // installed or without — the step drains them either way; the read itself takes them.
    [[nodiscard]] static std::size_t pendingAudioFrames(SnesBackend& backend) {
        return backend.snes_->takeFrames().size();
    }
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H
