// In-engine SM83 (Sharp LR35902 / Game Boy CPU) assembler — converts SM83 assembly
// source text into machine-code bytes. NO external toolchain: this is ordinary engine C++, not a
// shell-out to an external assembler. It exists so a routine can be authored as readable
// SM83 assembly instead of a hand-typed hex array, and assembled in-process — at registration time
// (the VmBackend::assemble seam, runtime) OR by the compiler at build time.
//
// `constexpr` and header-only: the whole encoder is a constant expression, so a routine's
// bytecode can be assembled BY THE COMPILER and baked into the binary as a literal array (an Embed
// routine costs nothing at runtime, needs no .asm file on disk), while the SAME function still runs at
// runtime for a load-from-path routine. One assembler, two evaluation times, one source of truth.
//
// INTERNAL — under src/vm/, never include/retropp/. SM83-specific (a future console's backend brings
// its own ISA encoder); the generic VM host knows nothing of it.
#ifndef RETROPP_SRC_VM_SM83_ASSEMBLER_H
#define RETROPP_SRC_VM_SM83_ASSEMBLER_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/vm/assembler.h"  // SymbolTable + AssembledRoutine (platform-neutral)

namespace retropp::vm {

namespace sm83detail {

// ── constexpr char / text helpers ───────────────────────────────────────────────────────────────
// Hand-rolled so the whole assembler is a constant expression (<cctype> is not constexpr).
constexpr bool isSpaceCh(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
constexpr bool isDigitCh(char c) { return c >= '0' && c <= '9'; }
constexpr bool isAlphaCh(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr char toLowerCh(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(toLowerCh(c));
    return out;
}

constexpr std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isSpaceCh(s[b])) ++b;
    while (e > b && isSpaceCh(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

// Remove all whitespace (operands are matched in a space-insensitive normalized form, so
// `sp + 5` and `[ hl ]` parse identically to `sp+5` / `[hl]`).
constexpr std::string despace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (!isSpaceCh(c)) out.push_back(c);
    }
    return out;
}

// constexpr unsigned/signed → decimal string, for error-message construction (only ever evaluated on
// an error path; at compile time a hit error path makes the assembly ill-formed, i.e. a build error).
constexpr std::string intToStr(std::int64_t v) {
    if (v == 0) return "0";
    const bool          neg = v < 0;
    std::uint64_t       u   = neg ? (static_cast<std::uint64_t>(-(v + 1)) + 1)
                                  : static_cast<std::uint64_t>(v);
    std::string s;
    while (u > 0) {
        s.push_back(static_cast<char>('0' + static_cast<char>(u % 10)));
        u /= 10;
    }
    if (neg) s.push_back('-');
    for (std::size_t i = 0, j = s.size(); i + 1 < j; ++i, --j) {
        const char t = s[i];
        s[i]         = s[j - 1];
        s[j - 1]     = t;
    }
    return s;
}

// NOT constexpr: it ALWAYS throws, so it can never itself be a constant expression. A constexpr
// caller may still call it on an error path — for valid input that path is not taken, so the caller
// stays a constant expression; for invalid input at COMPILE time, reaching this call makes the
// assembly ill-formed (a build error, which is the intent), and at RUNTIME it throws normally.
[[noreturn]] inline void fail(std::size_t line, std::string_view msg) {
    throw std::runtime_error("SM83 assembler, line " + intToStr(static_cast<std::int64_t>(line)) +
                             ": " + std::string(msg));
}

// Parse `s` as a base-`base` magnitude (no sign — leading sign is handled by evalExpr).
constexpr std::int64_t parseDigits(std::string_view s, int base, std::size_t line) {
    if (s.empty()) fail(line, "empty number");
    std::int64_t v = 0;
    for (char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        }
        if (d < 0 || d >= base) fail(line, "invalid digit in numeric literal");
        v = v * base + d;
    }
    return v;
}

// ── operand classification ────────────────────────────────────────────────────────────────────
// The 3-bit r field of the standard register-operand tables: B C D E H L [HL] A.
constexpr int reg8(const std::string& t) {
    if (t == "b") return 0;
    if (t == "c") return 1;
    if (t == "d") return 2;
    if (t == "e") return 3;
    if (t == "h") return 4;
    if (t == "l") return 5;
    if (t == "[hl]") return 6;
    if (t == "a") return 7;
    return -1;
}

// The 2-bit register-pair field. `af` replaces `sp` only for push/pop (selected by allowAf).
constexpr int reg16(const std::string& t, bool allowAf) {
    if (t == "bc") return 0;
    if (t == "de") return 1;
    if (t == "hl") return 2;
    if (!allowAf && t == "sp") return 3;
    if (allowAf && t == "af") return 3;
    return -1;
}

// The 2-bit condition field: NZ Z NC C.
constexpr int cond(const std::string& t) {
    if (t == "nz") return 0;
    if (t == "z") return 1;
    if (t == "nc") return 2;
    if (t == "c") return 3;
    return -1;
}

constexpr bool isMem(const std::string& t) {
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}
constexpr std::string memInner(const std::string& t) { return t.substr(1, t.size() - 2); }

// ── expression evaluation ─────────────────────────────────────────────────────────────────────
// Pass 1 uses a lenient lookup (unknown → 0) so instruction SIZES — which never depend on a value —
// are computed even before labels are defined; pass 2 uses a strict lookup that errors on unknown.
constexpr std::int64_t evalTerm(const std::string& term, std::size_t line, const SymbolTable& syms,
                                bool lenient) {
    if (term.empty()) fail(line, "empty expression term");
    if (term[0] == '$') return parseDigits(std::string_view(term).substr(1), 16, line);
    if (term[0] == '%') return parseDigits(std::string_view(term).substr(1), 2, line);
    if (isDigitCh(term[0])) return parseDigits(term, 10, line);
    // A symbol (label or predefined constant).
    if (const std::uint32_t* v = syms.find(term)) return static_cast<std::int64_t>(*v);
    if (lenient) return 0;
    fail(line, "unknown symbol '" + term + "'");
}

// Evaluate an operand expression: a term, optionally with one trailing +/- term (covers `label+2`,
// `$FF00-1`). Leading sign is honored (`-2`).
constexpr std::int64_t evalExpr(const std::string& expr, std::size_t line, const SymbolTable& syms,
                                bool lenient) {
    std::string e = despace(expr);
    if (e.empty()) fail(line, "empty expression");
    bool        neg = false;
    std::size_t i   = 0;
    if (e[0] == '+' || e[0] == '-') {
        neg = e[0] == '-';
        i   = 1;
    }
    // Find a top-level +/- separating two terms (search from index 1 so a leading sign isn't it).
    std::size_t split = std::string::npos;
    for (std::size_t j = std::max<std::size_t>(i, 1); j < e.size(); ++j) {
        if (e[j] == '+' || e[j] == '-') {
            split = j;
            break;
        }
    }
    if (split == std::string::npos) {
        const std::int64_t v = evalTerm(e.substr(i), line, syms, lenient);
        return neg ? -v : v;
    }
    const std::int64_t left  = evalTerm(e.substr(i, split - i), line, syms, lenient);
    const std::int64_t right = evalTerm(e.substr(split + 1), line, syms, lenient);
    const std::int64_t l     = neg ? -left : left;
    return e[split] == '+' ? l + right : l - right;
}

constexpr std::uint8_t imm8(std::int64_t v, std::size_t line) {
    if (v < -128 || v > 255) fail(line, "8-bit value out of range: " + intToStr(v));
    return static_cast<std::uint8_t>(v & 0xFF);
}

constexpr std::uint8_t rel8(std::int64_t target, std::int64_t afterInstr, std::size_t line) {
    const std::int64_t d = target - afterInstr;
    if (d < -128 || d > 127) fail(line, "jr target out of range (" + intToStr(d) + " bytes)");
    return static_cast<std::uint8_t>(d & 0xFF);
}

// ── one instruction → bytes ───────────────────────────────────────────────────────────────────
// addr is the byte offset of this instruction (for jr relative resolution). Emits into `out`.
constexpr void encode(const std::string& mnem, const std::vector<std::string>& ops, std::size_t addr,
                      std::size_t line, const SymbolTable& syms, bool lenient,
                      std::vector<std::uint8_t>& out) {
    auto eval = [&](const std::string& s) { return evalExpr(s, line, syms, lenient); };
    auto emit = [&](std::initializer_list<int> bytes) {
        for (int b : bytes) out.push_back(static_cast<std::uint8_t>(b & 0xFF));
    };
    auto need = [&](std::size_t n) {
        if (ops.size() != n) fail(line, mnem + ": expected " + intToStr(static_cast<std::int64_t>(n)) +
                                            " operand(s)");
    };

    // ALU A-implicit ops: `op r` / `op a,r` / `op [hl]` / `op n` / `op a,n`.
    auto alu = [&](int baseReg, int immOp) {
        std::string rhs = ops.size() == 2 ? ops[1] : ops[0];
        if (ops.size() == 2 && ops[0] != "a") fail(line, mnem + ": first operand must be A");
        if (ops.size() != 1 && ops.size() != 2) fail(line, mnem + ": wrong operand count");
        if (const int r = reg8(rhs); r >= 0) {
            emit({baseReg | r});
        } else {
            emit({immOp, imm8(eval(rhs), line)});
        }
    };

    // ── 8-bit / 16-bit loads ──
    if (mnem == "ld") {
        need(2);
        const std::string& d = ops[0];
        const std::string& s = ops[1];
        // ld r,r' | ld r,[hl] | ld [hl],r | ld r,n | ld [hl],n  (via the unified r-table, [hl]=6)
        const int dr = reg8(d), sr = reg8(s);
        if (dr >= 0 && sr >= 0) {
            if (dr == 6 && sr == 6) fail(line, "ld [hl],[hl] is not an instruction (that opcode is halt)");
            emit({0x40 | (dr << 3) | sr});
            return;
        }
        if (dr >= 0 && sr < 0 && !isMem(s) && reg16(s, false) < 0) {
            emit({0x06 | (dr << 3), imm8(eval(s), line)});  // ld r,n
            return;
        }
        // ld a,[bc]/[de]/[hl+]/[hl-]/[nn] ; ld [bc]/[de]/[hl+]/[hl-]/[nn],a
        if (d == "a" && isMem(s)) {
            const std::string m = memInner(s);
            if (m == "bc") { emit({0x0A}); return; }
            if (m == "de") { emit({0x1A}); return; }
            if (m == "hl+" || m == "hli") { emit({0x2A}); return; }
            if (m == "hl-" || m == "hld") { emit({0x3A}); return; }
            if (m == "c" || m == "$ff00+c") { emit({0xF2}); return; }  // ld a,[$FF00+C]
            { const std::int64_t v = eval(m);
              emit({0xFA, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return; }
        }
        if (s == "a" && isMem(d)) {
            const std::string m = memInner(d);
            if (m == "bc") { emit({0x02}); return; }
            if (m == "de") { emit({0x12}); return; }
            if (m == "hl+" || m == "hli") { emit({0x22}); return; }
            if (m == "hl-" || m == "hld") { emit({0x32}); return; }
            if (m == "c" || m == "$ff00+c") { emit({0xE2}); return; }  // ld [$FF00+C],a
            { const std::int64_t v = eval(m);
              emit({0xEA, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return; }
        }
        if (d == "sp" && s == "hl") { emit({0xF9}); return; }               // ld sp,hl
        // ld rp,nn  (and the ld hl,sp+e special)
        if (const int rp = reg16(d, false); rp >= 0 && !isMem(s)) {
            if (d == "hl" && (s.rfind("sp+", 0) == 0 || s.rfind("sp-", 0) == 0)) {
                const std::int64_t e = eval(s.substr(2));                     // ld hl,sp+e
                emit({0xF8, imm8(e, line)}); return;
            }
            const std::int64_t v = eval(s);
            emit({0x01 | (rp << 4), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        // ld [nn],sp
        if (isMem(d) && s == "sp") {
            const std::int64_t v = eval(memInner(d));
            emit({0x08, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        fail(line, "ld: unsupported operands '" + d + "', '" + s + "'");
    }

    if (mnem == "ldh") {
        need(2);
        const std::string& d = ops[0];
        const std::string& s = ops[1];
        auto highByte = [&](const std::string& m) -> std::uint8_t {
            const std::int64_t v  = eval(m);
            const std::int64_t lo = (v >= 0xFF00 && v <= 0xFFFF) ? (v - 0xFF00) : v;
            if (lo < 0 || lo > 0xFF) fail(line, "ldh address out of high page: " + m);
            return static_cast<std::uint8_t>(lo);
        };
        if (d == "a" && isMem(s)) {
            const std::string m = memInner(s);
            if (m == "c") { emit({0xF2}); return; }
            emit({0xF0, highByte(m)}); return;
        }
        if (s == "a" && isMem(d)) {
            const std::string m = memInner(d);
            if (m == "c") { emit({0xE2}); return; }
            emit({0xE0, highByte(m)}); return;
        }
        fail(line, "ldh: operands must be a,[n] or [n],a");
    }

    // ── 8-bit ALU ──
    if (mnem == "add") {
        // add hl,rp | add sp,e | add a,r | add r | add n
        if (ops.size() == 2 && ops[0] == "hl") {
            const int rp = reg16(ops[1], false);
            if (rp < 0) fail(line, "add hl,rp: bad register pair");
            emit({0x09 | (rp << 4)}); return;
        }
        if (ops.size() == 2 && ops[0] == "sp") { emit({0xE8, imm8(eval(ops[1]), line)}); return; }
        alu(0x80, 0xC6); return;
    }
    if (mnem == "adc") { alu(0x88, 0xCE); return; }
    if (mnem == "sub") { alu(0x90, 0xD6); return; }
    if (mnem == "sbc") { alu(0x98, 0xDE); return; }
    if (mnem == "and") { alu(0xA0, 0xE6); return; }
    if (mnem == "xor") { alu(0xA8, 0xEE); return; }
    if (mnem == "or")  { alu(0xB0, 0xF6); return; }
    if (mnem == "cp")  { alu(0xB8, 0xFE); return; }

    // ── inc / dec ──
    if (mnem == "inc" || mnem == "dec") {
        need(1);
        if (const int r = reg8(ops[0]); r >= 0) {
            emit({(mnem == "inc" ? 0x04 : 0x05) | (r << 3)}); return;
        }
        if (const int rp = reg16(ops[0], false); rp >= 0) {
            emit({(mnem == "inc" ? 0x03 : 0x0B) | (rp << 4)}); return;
        }
        fail(line, mnem + ": bad operand '" + ops[0] + "'");
    }

    // ── rotates (A) / misc single-byte ──
    constexpr std::array<std::pair<std::string_view, int>, 13> kSimple{{
        {"nop", 0x00}, {"halt", 0x76}, {"di", 0xF3}, {"ei", 0xFB},
        {"ccf", 0x3F}, {"scf", 0x37}, {"cpl", 0x2F}, {"daa", 0x27},
        {"rlca", 0x07}, {"rrca", 0x0F}, {"rla", 0x17}, {"rra", 0x1F},
        {"reti", 0xD9}}};
    for (const auto& [name, op] : kSimple) {
        if (mnem == name) { need(0); emit({op}); return; }
    }
    if (mnem == "stop") { need(0); emit({0x10, 0x00}); return; }

    // ── jumps / calls / returns ──
    if (mnem == "jp") {
        if (ops.size() == 1 && (ops[0] == "hl" || ops[0] == "[hl]")) { emit({0xE9}); return; }
        if (ops.size() == 1) {
            const std::int64_t v = eval(ops[0]);
            emit({0xC3, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        if (ops.size() == 2) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "jp: bad condition '" + ops[0] + "'");
            const std::int64_t v = eval(ops[1]);
            emit({0xC2 | (cc << 3), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        fail(line, "jp: wrong operand count");
    }
    if (mnem == "jr") {
        std::string targetExpr;
        int         op0 = 0x18, cc = -1;
        if (ops.size() == 1) { targetExpr = ops[0]; }
        else if (ops.size() == 2) {
            cc = cond(ops[0]);
            if (cc < 0) fail(line, "jr: bad condition '" + ops[0] + "'");
            op0        = 0x20 | (cc << 3);
            targetExpr = ops[1];
        } else { fail(line, "jr: wrong operand count"); }
        const std::int64_t target = eval(targetExpr);
        emit({op0, rel8(target, static_cast<std::int64_t>(addr) + 2, line)});
        return;
    }
    if (mnem == "call") {
        if (ops.size() == 1) {
            const std::int64_t v = eval(ops[0]);
            emit({0xCD, static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)}); return;
        }
        if (ops.size() == 2) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "call: bad condition '" + ops[0] + "'");
            const std::int64_t v = eval(ops[1]);
            emit({0xC4 | (cc << 3), static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF)});
            return;
        }
        fail(line, "call: wrong operand count");
    }
    if (mnem == "ret") {
        if (ops.empty()) { emit({0xC9}); return; }
        if (ops.size() == 1) {
            const int cc = cond(ops[0]);
            if (cc < 0) fail(line, "ret: bad condition '" + ops[0] + "'");
            emit({0xC0 | (cc << 3)}); return;
        }
        fail(line, "ret: wrong operand count");
    }
    if (mnem == "rst") {
        need(1);
        const std::int64_t v = eval(ops[0]);
        if (v < 0 || v > 0x38 || (v & 0x07) != 0) fail(line, "rst: vector must be one of $00..$38 step 8");
        emit({0xC7 | static_cast<int>(v)}); return;
    }

    // ── push / pop ──
    if (mnem == "push" || mnem == "pop") {
        need(1);
        const int rp = reg16(ops[0], /*allowAf=*/true);
        if (rp < 0) fail(line, mnem + ": bad register pair '" + ops[0] + "'");
        emit({(mnem == "push" ? 0xC5 : 0xC1) | (rp << 4)}); return;
    }

    // ── CB-prefixed: rotates/shifts and bit/res/set ──
    constexpr std::array<std::pair<std::string_view, int>, 8> kCbShift{{
        {"rlc", 0x00}, {"rrc", 0x08}, {"rl", 0x10}, {"rr", 0x18},
        {"sla", 0x20}, {"sra", 0x28}, {"swap", 0x30}, {"srl", 0x38}}};
    for (const auto& [name, base] : kCbShift) {
        if (mnem == name) {
            need(1);
            const int r = reg8(ops[0]);
            if (r < 0) fail(line, mnem + ": bad register '" + ops[0] + "'");
            emit({0xCB, base | r}); return;
        }
    }
    if (mnem == "bit" || mnem == "res" || mnem == "set") {
        need(2);
        const std::int64_t b = eval(ops[0]);
        if (b < 0 || b > 7) fail(line, mnem + ": bit index must be 0..7");
        const int r = reg8(ops[1]);
        if (r < 0) fail(line, mnem + ": bad register '" + ops[1] + "'");
        const int base = mnem == "bit" ? 0x40 : (mnem == "res" ? 0x80 : 0xC0);
        emit({0xCB, base | (static_cast<int>(b) << 3) | r}); return;
    }

    fail(line, "unknown mnemonic '" + mnem + "'");
}

// A parsed source line: any labels it defines + an optional instruction (mnemonic + operands).
struct Line {
    std::size_t              number = 0;
    std::vector<std::string> labels;
    std::string              mnemonic;  // empty = label-only / blank line
    std::vector<std::string> operands;
};

constexpr std::vector<Line> parseLines(std::string_view source) {
    std::vector<Line> lines;
    std::size_t       lineNo = 0;
    std::size_t       pos    = 0;
    while (pos <= source.size()) {
        const std::size_t nl  = source.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? source.size() : nl;
        ++lineNo;
        std::string raw(source.substr(pos, end - pos));
        pos = end + 1;

        if (const std::size_t c = raw.find(';'); c != std::string::npos) raw.erase(c);  // strip comment
        std::string text = trim(raw);

        Line ln;
        ln.number = lineNo;
        // Leading labels: `name:` (possibly several, possibly sharing a line with an instruction).
        for (;;) {
            const std::size_t colon = text.find(':');
            if (colon == std::string::npos) break;
            std::string label = trim(text.substr(0, colon));
            // A colon that's part of an operand (none in our grammar) would break here; labels are
            // simple identifiers, so require the head to be label-shaped.
            if (label.empty() || !(isAlphaCh(label[0]) || label[0] == '_' || label[0] == '.')) {
                break;
            }
            ln.labels.push_back(lower(label));
            text = trim(text.substr(colon + 1));
        }
        if (!text.empty()) {
            // mnemonic = first whitespace-delimited token; the rest splits on commas.
            std::size_t sp = 0;
            while (sp < text.size() && !isSpaceCh(text[sp])) ++sp;
            ln.mnemonic      = lower(text.substr(0, sp));
            std::string rest = trim(text.substr(sp));
            std::size_t start = 0;
            while (start <= rest.size() && !rest.empty()) {
                const std::size_t comma = rest.find(',', start);
                const std::size_t e     = comma == std::string::npos ? rest.size() : comma;
                ln.operands.push_back(lower(despace(rest.substr(start, e - start))));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (!ln.labels.empty() || !ln.mnemonic.empty()) lines.push_back(std::move(ln));
    }
    return lines;
}

}  // namespace sm83detail

// Assemble SM83 assembly source text into machine code. The conventional Game Boy dialect — the format the
// Game Boy disassembly is already in: `;` line comments, `label:` definitions, `$hex` / `%bin` /
// decimal literals, `[hl]` / `[$FF04]` memory operands, condition codes (z / nz / c / nc). One
// instruction per line (a label may share a line with an instruction). `predefined` seeds the
// symbol table with platform constants; labels in the source extend it.
//
// `constexpr`: evaluable by the compiler (build-time bytecode embedding) AND at runtime (the
// VmBackend::assemble seam). Throws std::runtime_error — carrying the 1-based line number and the
// offending text — on an unknown mnemonic, a malformed operand, an unknown symbol, or an out-of-range
// value; at compile time a thrown error makes the assembly ill-formed (a build error).
constexpr AssembledRoutine assembleSm83(std::string_view source, const SymbolTable& predefined = {}) {
    using namespace sm83detail;
    const std::vector<Line> lines = parseLines(source);

    // Pass 1 — assign label offsets. Instruction length depends only on operand shape, so encoding
    // with a lenient (unknown → 0) resolver yields the correct size even before labels are known.
    SymbolTable syms = predefined;
    SymbolTable labels;
    {
        std::size_t               addr = 0;
        std::vector<std::uint8_t> scratch;
        for (const Line& ln : lines) {
            for (const std::string& lbl : ln.labels) {
                if (labels.count(lbl) || predefined.count(lbl)) {
                    fail(ln.number, "duplicate label '" + lbl + "'");
                }
                labels.set(lbl, static_cast<std::uint32_t>(addr));
                syms.set(lbl, static_cast<std::uint32_t>(addr));
            }
            if (ln.mnemonic.empty()) continue;
            scratch.clear();
            encode(ln.mnemonic, ln.operands, addr, ln.number, syms, /*lenient=*/true, scratch);
            addr += scratch.size();
        }
    }

    // Pass 2 — encode for real, every label now resolvable.
    AssembledRoutine result;
    result.labels    = labels;
    std::size_t addr = 0;
    for (const Line& ln : lines) {
        if (ln.mnemonic.empty()) continue;
        const std::size_t before = result.bytes.size();
        encode(ln.mnemonic, ln.operands, addr, ln.number, syms, /*lenient=*/false, result.bytes);
        addr += result.bytes.size() - before;
    }
    return result;
}

}  // namespace retropp::vm

#endif  // RETROPP_SRC_VM_SM83_ASSEMBLER_H
