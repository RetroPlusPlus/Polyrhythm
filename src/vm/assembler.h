// Generic VM assembler output types — platform-NEUTRAL. Every per-platform assembler (the
// Game Boy's SM83 assembler, a future SNES 65816 assembler, …) produces the same shape: machine-code
// bytes plus the byte offset of each label. These types live here, not in any one platform's
// assembler header, so the generic backend seam (vm_backend.h) names them without depending on a
// specific ISA's assembler.
//
// INTERNAL — under src/vm/, never include/retropp/.
#ifndef RETROPP_SRC_VM_ASSEMBLER_H
#define RETROPP_SRC_VM_ASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace retropp::vm {

// A name → value symbol table: constants an assembler resolves by name (a platform's hardware-register
// addresses, plus labels defined within a routine's source).
//
// A flat (linear-scan) map rather than std::unordered_map, because the SM83 assembler is `constexpr`
// so a routine's bytecode can be assembled BY THE COMPILER at build time — and the standard
// hashed containers are not usable in a constant expression. The tables are tiny (a few dozen hardware
// names + a routine's handful of labels), so a linear scan is the right structure here regardless. The
// public surface mirrors the slice of std::map this code uses (initializer-list construction, count,
// at), so existing call sites and tests are unchanged.
class SymbolTable {
public:
    constexpr SymbolTable() = default;
    constexpr SymbolTable(
        std::initializer_list<std::pair<std::string_view, std::uint32_t>> init) {
        for (const auto& [name, value] : init) set(name, value);
    }

    // Insert `name → value`, overwriting an existing entry for `name`.
    constexpr void set(std::string_view name, std::uint32_t value) {
        for (Entry& e : entries_) {
            if (e.name == name) {
                e.value = value;
                return;
            }
        }
        entries_.push_back(Entry{std::string(name), value});
    }

    // A pointer to the stored value for `name`, or nullptr if absent. The pointer is valid until the
    // next mutation (sufficient for the single-evaluation assemble passes).
    [[nodiscard]] constexpr const std::uint32_t* find(std::string_view name) const {
        for (const Entry& e : entries_) {
            if (e.name == name) return &e.value;
        }
        return nullptr;
    }

    [[nodiscard]] constexpr std::size_t count(std::string_view name) const {
        return find(name) != nullptr ? 1u : 0u;
    }

    // The value for `name`; throws std::out_of_range if absent (mirrors std::map::at).
    [[nodiscard]] constexpr std::uint32_t at(std::string_view name) const {
        const std::uint32_t* v = find(name);
        if (v == nullptr) throw std::out_of_range("SymbolTable::at — unknown symbol");
        return *v;
    }

private:
    struct Entry {
        std::string   name;
        std::uint32_t value;
    };
    std::vector<Entry> entries_;
};

// The result of assembling routine source: the machine-code bytes, plus every label's byte offset
// within them (so a routine's entry, or a named cell, is referenced by name rather than a magic
// offset), plus the address the bytes were assembled for when the ISA's assembler is absolute. An
// absolute routine is correct at `origin` and nowhere else, so it is placed there; one with no origin
// is placed wherever the arena has room.
struct AssembledRoutine {
    std::vector<std::uint8_t>    bytes;
    SymbolTable                  labels;    // label name → byte offset within `bytes`
    std::optional<std::uint32_t> origin{};  // the address of bytes[0], for an absolute assembler
};

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_ASSEMBLER_H
