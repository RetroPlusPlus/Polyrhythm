// Names a voice from a value known only as the program runs. A voice is a compile-time literal, so this
// does not build — the outcome tests/compile_outcome.cmake asserts.
#include "retropp/audio_system.h"

retropp::AudioSystem::SNES::Voice fromArgument(int n) {
    return retropp::AudioSystem::SNES::Voice{n};
}
