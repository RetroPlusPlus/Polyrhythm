// The engine reaches Snaggletooth's SNES machine and both of its assemblers from a
// test: a program written in 65816 assembles to bytes, lays into a LoROM image, and
// runs on a snaggletooth::Snes; and a line of SPC700 assembles to the bytes the audio
// CPU runs. A fixture cartridge carries no committed binary — its code is assembled
// here, so it is source a reader can follow.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "cpu65816/cpu65816_asm.h"
#include "examples/common.h"
#include "snaggletooth/snes/snes.h"
#include "spc700/spc700_asm.h"

namespace {

using snaggletooth::assembler::Assembly;

// A one-bank LoROM cartridge whose reset code is `source`, laid over the canonical
// header. `source` places its first byte at $00:8000, where reset points.
std::vector<std::uint8_t> cartridgeFrom(std::string_view source) {
  const Assembly program = snaggletooth::assembler::assembleCpu65816(source, "fixture.asm");
  EXPECT_TRUE(program.ok());
  std::vector<std::uint8_t> rom = snaggletooth::examples::loRomImage(1);
  for (const auto& range : program.ranges) {
    const std::size_t at = range.start - 0x8000u;
    std::copy(range.bytes.begin(), range.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return rom;
}

TEST(SnaggletoothLink, Cpu65816AssemblesToBytes) {
  const Assembly one = snaggletooth::assembler::assembleCpu65816(
      "        ORG $00:8000\n        A8\n        X8\n        LDA #$12\n", "t.asm");
  ASSERT_TRUE(one.ok());
  ASSERT_FALSE(one.ranges.empty());
  EXPECT_EQ(one.ranges.front().bytes, (std::vector<std::uint8_t>{0xA9, 0x12}));

  // An immediate assembled under an unknown register width is an error, never a guess.
  const Assembly bad = snaggletooth::assembler::assembleCpu65816(
      "        ORG $00:8000\n        LDA #$12\n", "t.asm");
  EXPECT_FALSE(bad.ok());
}

TEST(SnaggletoothLink, Spc700AssemblesToBytes) {
  const Assembly one = snaggletooth::assembler::assembleSpc700(
      "        ORG $0400\n        MOV A,#$01\n", "t.asm");
  ASSERT_TRUE(one.ok());
  ASSERT_FALSE(one.ranges.empty());
  EXPECT_EQ(one.ranges.front().bytes, (std::vector<std::uint8_t>{0xE8, 0x01}));

  const Assembly bad = snaggletooth::assembler::assembleSpc700(
      "        ORG $0400\n        MOV A,#\n", "t.asm");
  EXPECT_FALSE(bad.ok());
}

TEST(SnaggletoothLink, HostedCartridgeRunsAssembledCode) {
  const std::vector<std::uint8_t> rom = cartridgeFrom(
      "        ORG $00:8000\n"
      "        A8\n"
      "        X8\n"
      "        LDA #$42\n"    // A <- $42
      "        STA $00\n"     // work RAM $00 <- A
      "here:   BRA here\n");  // rest on the store

  snaggletooth::Snes machine(snaggletooth::SnesConfig{.rom = rom});
  EXPECT_EQ(machine.state().master, 0u);

  for (int i = 0; i < 5; ++i) machine.step();

  EXPECT_GT(machine.state().master, 0u);
  EXPECT_EQ(machine.state().wram[0], 0x42u);
}

}  // namespace
