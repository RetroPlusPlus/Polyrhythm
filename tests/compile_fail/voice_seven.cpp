// Names voice seven, one the SNES has. This builds — the control that proves the outcome harness compiles
// a well-formed voice, so a negative that fails to build for another reason is caught.
#include "retropp/audio_system.h"

constexpr retropp::AudioSystem::SNES::Voice v{7};
static_assert(v.index() == 7);
