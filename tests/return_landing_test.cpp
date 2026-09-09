// The run-to-return landing borrows two bytes of high RAM for the length of one run and puts the
// bytes it found back afterwards. These cases pin that borrow: a call and an idle advance both leave
// the landing holding whatever the machine held before them, and neither leaves the marker behind.
//
// High RAM is the landing's home because it is never banked and always mapped, so the address means
// the same thing whatever cartridge is loaded and whatever the guest has selected.
#include "src/vm/gameboy/sameboy_backend.h"

#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

namespace retropp::vm {
namespace {

constexpr std::uint32_t kLanding = 0xFFFD;  // the landing's first byte
constexpr std::uint8_t  kJrSelf  = 0x18;    // JR $ — the marker the run parks on

// A routine that returns immediately: the call's RET pops the landing address and the run stops.
std::uint32_t placeReturnOnly(SameBoyBackend& backend) {
    static constexpr std::array<std::uint8_t, 1> kRet{0xC9};
    return backend.placeRoutine(std::span<const std::uint8_t>(kRet));
}

TEST(ReturnLanding, CallRestoresTheBytesItFound) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    const std::uint32_t entry = placeReturnOnly(backend);

    const std::uint64_t before0 = backend.readMemory(kLanding, 1);
    const std::uint64_t before1 = backend.readMemory(kLanding + 1, 1);

    backend.beginCall(entry);
    backend.run();

    EXPECT_EQ(backend.readMemory(kLanding, 1), before0);
    EXPECT_EQ(backend.readMemory(kLanding + 1, 1), before1);
}

TEST(ReturnLanding, CallDoesNotLeaveTheMarkerBehind) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    const std::uint32_t entry = placeReturnOnly(backend);

    // Seeded, because power-on high RAM is PRNG-seeded: a landing that restores exactly what it found
    // still holds the marker afterwards whenever the machine powered on holding one, and the case
    // below asks about the landing's own doing rather than the byte it inherited.
    backend.writeMemory(kLanding, 0x00, 1);

    backend.beginCall(entry);
    backend.run();

    // The marker is a stop condition for the length of one run, not a resident instruction.
    EXPECT_NE(backend.readMemory(kLanding, 1), kJrSelf);
}

TEST(ReturnLanding, CallPreservesAValueTheMachineAlreadyHeldThere) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};
    const std::uint32_t entry = placeReturnOnly(backend);

    backend.writeMemory(kLanding, 0x5A, 1);
    backend.writeMemory(kLanding + 1, 0xA5, 1);

    backend.beginCall(entry);
    backend.run();

    EXPECT_EQ(backend.readMemory(kLanding, 1), 0x5Au);
    EXPECT_EQ(backend.readMemory(kLanding + 1, 1), 0xA5u);
}

TEST(ReturnLanding, IdleAdvanceRestoresTheBytesItFound) {
    SameBoyBackend backend{ConsoleModel::GameBoyColor};

    backend.writeMemory(kLanding, 0x3C, 1);
    backend.writeMemory(kLanding + 1, 0xC3, 1);

    backend.advanceClock(70224);  // one frame of divider ticking, parked on the marker

    EXPECT_EQ(backend.readMemory(kLanding, 1), 0x3Cu);
    EXPECT_EQ(backend.readMemory(kLanding + 1, 1), 0xC3u);
}

}  // namespace
}  // namespace retropp::vm
