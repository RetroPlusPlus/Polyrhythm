#pragma once

#include <memory>

#include "retropp/engine_config.h"
#include "retropp/sdl_platform.h"

namespace retropp::detail {

// The internal seam for exercising SdlPlatform's input on a machine with no display to draw to: an
// SdlPlatform with its window, its event pump and its gamepads, and no GPU device and no audio.
// device() is null, so nothing can be drawn through it; pumpEvents, input, actions, the slot routing
// and the capture behave as on a full platform. Throws std::runtime_error when SDL cannot start or
// the window cannot be created.
struct SdlPlatformTestAccess {
    [[nodiscard]] static std::unique_ptr<SdlPlatform> inputOnly(const EngineConfig& config);
};

}  // namespace retropp::detail
