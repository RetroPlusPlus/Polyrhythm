// The SM83 assembler's addresses, exercised in isolation. A label is a position in its source, and an
// `ORG` or a caller's origin gives it an address; a `jr` measures the same distance at any address; a
// label named as an address with no address to give it is refused at its line; and the sizing pass
// checks no value, so a near forward `jr` assembles from anywhere in a source.
#include "src/vm/gameboy/sm83_assembler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace retropp::vm {
namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes asm_(std::string_view src, std::optional<std::uint32_t> origin = std::nullopt,
           const SymbolTable& syms = {}) {
    return assembleSm83(src, syms, origin).bytes;
}

// An `ORG` gives every use of a label the address the bytes land at: an absolute jump in each form, a
// call in each form, a 16-bit load, a memory operand in each direction, and a label with an offset.
TEST(Sm83AssemblerOrigin, OrgGivesEveryLabelItsAddress) {
    EXPECT_EQ(asm_("ORG $0150\nloop:\n  nop\n  jp loop\n"), (Bytes{0x00, 0xC3, 0x50, 0x01}));

    const char* src =
        "ORG $0150\n"
        "  nop\n"
        "tail:\n"
        "  jp nz, tail\n"
        "  call tail\n"
        "  call c, tail\n"
        "  ld hl, tail\n"
        "  ld a, [tail]\n"
        "  ld [tail], a\n"
        "  ld [tail], sp\n"
        "  jp tail+2\n";
    EXPECT_EQ(asm_(src), (Bytes{0x00,              // nop
                                0xC2, 0x51, 0x01,  // jp nz, tail
                                0xCD, 0x51, 0x01,  // call tail
                                0xDC, 0x51, 0x01,  // call c, tail
                                0x21, 0x51, 0x01,  // ld hl, tail
                                0xFA, 0x51, 0x01,  // ld a, [tail]
                                0xEA, 0x51, 0x01,  // ld [tail], a
                                0x08, 0x51, 0x01,  // ld [tail], sp
                                0xC3, 0x53, 0x01}));  // jp tail+2
}

// A caller's origin does what an `ORG` does, and a source's own `ORG` wins over it.
TEST(Sm83AssemblerOrigin, ACallerOriginStandsInForOrg) {
    const AssembledRoutine fromCaller = assembleSm83("loop:\n  nop\n  jp loop\n", {}, 0x0160);
    EXPECT_EQ(fromCaller.bytes, (Bytes{0x00, 0xC3, 0x60, 0x01}));
    EXPECT_EQ(fromCaller.origin, 0x0160u);

    const AssembledRoutine fromSource = assembleSm83("ORG $0150\nloop:\n  nop\n  jp loop\n", {}, 0x0160);
    EXPECT_EQ(fromSource.bytes, (Bytes{0x00, 0xC3, 0x50, 0x01}));
    EXPECT_EQ(fromSource.origin, 0x0150u);
}

// `labels` are offsets within the bytes whether or not the source has an address.
TEST(Sm83AssemblerOrigin, LabelsStayOffsetsWithinTheBytes) {
    const AssembledRoutine placed = assembleSm83("ORG $0150\nentry:\n  nop\n  nop\ntail:\n  ret\n");
    EXPECT_EQ(placed.labels.at("entry"), 0u);
    EXPECT_EQ(placed.labels.at("tail"), 2u);
    EXPECT_EQ(placed.origin, 0x0150u);

    const AssembledRoutine unplaced = assembleSm83("entry:\n  nop\n  nop\ntail:\n  ret\n");
    EXPECT_EQ(unplaced.labels.at("entry"), 0u);
    EXPECT_EQ(unplaced.labels.at("tail"), 2u);
    EXPECT_FALSE(unplaced.origin.has_value());
}

// A `jr` encodes the distance to its label, which is the same wherever the bytes land.
TEST(Sm83AssemblerOrigin, JrMeasuresTheSameDistanceAtAnyOrigin) {
    const Bytes backward{0x00, 0x18, 0xFD};
    EXPECT_EQ(asm_("loop:\n  nop\n  jr loop\n"), backward);
    EXPECT_EQ(asm_("ORG $0150\nloop:\n  nop\n  jr loop\n"), backward);
    EXPECT_EQ(asm_("loop:\n  nop\n  jr loop\n", 0x01C0), backward);

    const Bytes forward{0x20, 0x01, 0x00};
    EXPECT_EQ(asm_("  jr nz, skip\n  nop\nskip:\n"), forward);
    EXPECT_EQ(asm_("ORG $0150\n  jr nz, skip\n  nop\nskip:\n"), forward);
    EXPECT_EQ(asm_("  jr nz, skip\n  nop\nskip:\n", 0x01C0), forward);
}

// With neither an `ORG` nor a caller's origin, a label named as an address is refused at its line, with
// the label and the fix in the message. A `jr` and a predefined symbol need no address of the source's.
TEST(Sm83AssemblerOrigin, ALabelNamedAsAnAddressWithNoAddressIsRefused) {
    const std::vector<std::string> uses{"jp loop",      "jp z, loop",    "call loop",
                                        "ld hl, loop",  "ld a, [loop]",  "ld [loop], a",
                                        "ld [loop], sp", "ld hl, loop+1"};
    for (const std::string& use : uses) {
        const std::string source = "loop:\n  " + use + "\n";
        try {
            (void)assembleSm83(source);
            ADD_FAILURE() << "assembled: " << use;
        } catch (const std::runtime_error& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("line 2"), std::string::npos) << message;
            EXPECT_NE(message.find("loop"), std::string::npos) << message;
            EXPECT_NE(message.find("ORG"), std::string::npos) << message;
        }
    }

    EXPECT_EQ(asm_("loop:\n  jr loop\n"), (Bytes{0x18, 0xFE}));
    EXPECT_EQ(asm_("  ld a, [rdiv]\n", std::nullopt, SymbolTable{{"rdiv", 0xFF04}}),
              (Bytes{0xFA, 0x04, 0xFF}));
}

// `ORG` is one line, first, in the 16-bit address space, in any case.
TEST(Sm83AssemblerOrigin, OrgComesFirstOnceAndInRange) {
    EXPECT_THROW(assembleSm83("  nop\n  ORG $0150\n"), std::runtime_error);
    EXPECT_THROW(assembleSm83("start:\n  ORG $0150\n"), std::runtime_error);
    EXPECT_THROW(assembleSm83("start: ORG $0150\n"), std::runtime_error);
    EXPECT_THROW(assembleSm83("  ORG $0150\n  ORG $0160\n"), std::runtime_error);
    EXPECT_THROW(assembleSm83("  ORG $10000\n"), std::runtime_error);

    EXPECT_EQ(assembleSm83("  org $0150\n  nop\n").origin, 0x0150u);
    EXPECT_EQ(sm83DeclaredOrigin("  org $0150\n  nop\n"), 0x0150u);
    EXPECT_FALSE(sm83DeclaredOrigin("  nop\n").has_value());
}

// A forward `jr` deep in a source is measured against where its label really is.
TEST(Sm83AssemblerOrigin, AForwardJrFromDeepInTheSourceAssembles) {
    std::string src;
    for (int i = 0; i < 200; ++i) src += "  nop\n";
    src += "  jr done\n  nop\ndone:\n";

    for (const std::optional<std::uint32_t> origin :
         {std::optional<std::uint32_t>{}, std::optional<std::uint32_t>{0x0160}}) {
        const Bytes bytes = asm_(src, origin);
        ASSERT_EQ(bytes.size(), 203u);
        EXPECT_EQ(bytes[200], 0x18);
        EXPECT_EQ(bytes[201], 0x01);
        EXPECT_EQ(bytes[202], 0x00);
    }
}

// A `jr` whose target is out of reach is refused at any origin.
TEST(Sm83AssemblerOrigin, AnOutOfRangeJrIsStillRefusedWhereItIsReal) {
    std::string src = "top:\n";
    for (int i = 0; i < 200; ++i) src += "  nop\n";
    src += "  jr top\n";
    EXPECT_THROW(assembleSm83(src, {}, 0x0160), std::runtime_error);
}

// An `ORG` and the addresses it gives assemble in the compiler, which is what a baked routine relies on.
constexpr bool orgAssemblesAtCompileTime() {
    const AssembledRoutine r = assembleSm83("ORG $0150\nloop:\n  nop\n  jp loop\n");
    const std::uint8_t     expected[] = {0x00, 0xC3, 0x50, 0x01};
    if (r.bytes.size() != sizeof(expected)) return false;
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        if (r.bytes[i] != expected[i]) return false;
    }
    return r.origin == 0x0150u;
}
static_assert(orgAssemblesAtCompileTime(), "an ORG must assemble at compile time");

TEST(Sm83AssemblerOrigin, OrgAssemblesAtCompileTime) { EXPECT_TRUE(orgAssemblesAtCompileTime()); }

}  // namespace
}  // namespace retropp::vm
