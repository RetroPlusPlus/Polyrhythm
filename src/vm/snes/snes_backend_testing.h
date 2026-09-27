// Internal test access to SnesBackend's live machine — reads the SnesState and the DSP queue the suites assert against
// without a public accessor. VmTestAccess's shape: a friend struct under src/vm/, never in
// include/retropp/. TEST observation only.
#ifndef RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H
#define RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H

#include <cstdint>
#include <optional>

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
    // The image the backend holds — a game's cartridge, or the one it wrote for its routines.
    [[nodiscard]] static const std::vector<std::uint8_t>& image(const SnesBackend& backend) noexcept {
        return backend.rom_;
    }
    // The DSP frames the machine has produced that nobody has taken. Zero after every step, with a sink
    // installed or without — the step drains them either way; the read itself takes them.
    [[nodiscard]] static std::size_t pendingAudioFrames(SnesBackend& backend) {
        return backend.snes_->takeFrames().size();
    }
    // Whether the machine tells the backend about an armed instruction, and about an armed access: set only
    // while something is armed and a sink is there to ask.
    [[nodiscard]] static bool instructionWatcherInstalled(const SnesBackend& backend) noexcept {
        return backend.snes_ && backend.snes_->instructionWatcher() != nullptr;
    }
    [[nodiscard]] static bool accessWatcherInstalled(const SnesBackend& backend) noexcept {
        return backend.snes_ && backend.snes_->accessWatcher() != nullptr;
    }
    // The byte the machine's own copy of the image holds at a bus address, read without a fetch.
    [[nodiscard]] static std::optional<std::uint8_t> peek(const SnesBackend& backend, std::uint32_t address) {
        return backend.snes_->peek(address);
    }
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SNES_SNES_BACKEND_TESTING_H
