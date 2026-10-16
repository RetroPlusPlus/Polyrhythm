// Names a voice past the SNES's eight. The voice literal is checked as the program compiles, so this does
// not build — the outcome tests/compile_outcome.cmake asserts.
#include "retropp/audio_system.h"

constexpr retropp::AudioSystem::SNES::Voice v{8};
