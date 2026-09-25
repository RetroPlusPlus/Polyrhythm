# Bakes one routine written for a Snaggletooth assembler into a generated header the routine registry
# reads: the assembled bytes, the address they were assembled for, and the ISA. Run by the build scan
# (retropp_autoembed_routines) as `cmake -P` for every Embed routine whose registration names an ISA
# other than SM83.
#
#   -DASSEMBLER=<the CLI's path>      cpu65816_asm or spc700_asm, built by Snaggletooth's tools
#   -DSOURCE=<the .asm's path>
#   -DLOGICAL=<the logical path>      the registration's literal, for the header's comment
#   -DSYMBOL=<the array's C identifier>
#   -DISA=<the retropp::Isa enumerator>   Wdc65816 or Spc700
#   -DHEADER=<the header to write>
#   -DLAYOUT_IN=<the previous routine's layout, or empty>
#   -DLAYOUT_OUT=<this routine's layout>
#
# A source that says ORG is assembled where it says. One that does not is laid out where it fits: the
# first gap from the arena's start that holds it, past every routine this target baked before it, the
# layout carried from one bake to the next through LAYOUT_IN / LAYOUT_OUT so a target's routines never
# overlap. The runtime lays a routine it assembles itself out the same way, past everything already
# placed, so a baked routine and a runtime one never collide either.
cmake_minimum_required(VERSION 3.20)

foreach(_v ASSEMBLER SOURCE LOGICAL SYMBOL ISA HEADER LAYOUT_OUT)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "bake_snaggletooth_routine: ${_v} is not set")
    endif()
endforeach()

# The arena as the runtime lays it: code windows from $8000 to $FFFF of each bank, the first bank's
# last 80 bytes reserved for the idle loop and the header, and no routine straddling a window's end.
set(_arena_start 32768)         # $00:8000
set(_bank0_code_end 65456)      # $00:FFB0 — the idle loop begins here
set(_bank_bytes 65536)

# Runs the assembler on `source`, writing `out`, and answers the first range's start and the last range's
# end (as decimal image addresses) from the ranges the CLI prints.
function(_assemble source out start_var end_var)
    execute_process(COMMAND "${ASSEMBLER}" "${source}" -o "${out}"
                    OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${ASSEMBLER} failed on ${source}:\n${_stderr}")
    endif()
    string(REGEX MATCHALL "\\$[0-9A-Fa-f]+-\\$[0-9A-Fa-f]+" _ranges "${_stdout}")
    if(NOT _ranges)
        message(FATAL_ERROR "${ASSEMBLER} printed no range for ${source}:\n${_stdout}")
    endif()
    list(GET _ranges 0 _first)
    list(GET _ranges -1 _last)
    string(REGEX REPLACE "^\\$([0-9A-Fa-f]+)-.*$" "\\1" _first_hex "${_first}")
    string(REGEX REPLACE "^.*-\\$([0-9A-Fa-f]+)$" "\\1" _last_hex "${_last}")
    math(EXPR _start "0x${_first_hex}")
    math(EXPR _end "0x${_last_hex} + 1")
    set(${start_var} "${_start}" PARENT_SCOPE)
    set(${end_var} "${_end}" PARENT_SCOPE)
endfunction()

# The occupied ranges so far, one "start end" pair per line.
set(_layout "")
if(LAYOUT_IN AND EXISTS "${LAYOUT_IN}")
    file(STRINGS "${LAYOUT_IN}" _layout)
endif()

file(READ "${SOURCE}" _text)
set(_bin "${HEADER}.bin")
if(_text MATCHES "(^|\n)[ \t]*[Oo][Rr][Gg][ \t]")
    _assemble("${SOURCE}" "${_bin}" _start _end)
else()
    # Assemble once at $000000 to learn the size, then lay it out and assemble again where it lands.
    _assemble("${SOURCE}" "${_bin}" _start _end)
    math(EXPR _size "${_end} - ${_start}")
    set(_at ${_arena_start})
    set(_moved TRUE)
    while(_moved)
        set(_moved FALSE)
        math(EXPR _stop "${_at} + ${_size}")
        math(EXPR _window_end "(${_at} / ${_bank_bytes} + 1) * ${_bank_bytes}")
        if(_at LESS ${_bank_bytes})
            set(_window_end ${_bank0_code_end})
        endif()
        if(_stop GREATER ${_window_end})
            math(EXPR _at "(${_at} / ${_bank_bytes} + 1) * ${_bank_bytes} + ${_arena_start}")
            set(_moved TRUE)
            continue()
        endif()
        foreach(_range IN LISTS _layout)
            string(REPLACE " " ";" _pair "${_range}")
            list(GET _pair 0 _their_start)
            list(GET _pair 1 _their_end)
            if(_at LESS ${_their_end} AND _their_start LESS ${_stop})
                set(_at ${_their_end})
                set(_moved TRUE)
            endif()
        endforeach()
    endwhile()
    math(EXPR _org_hex "${_at}" OUTPUT_FORMAT HEXADECIMAL)
    string(REGEX REPLACE "^0x" "" _org_hex "${_org_hex}")
    set(_placed "${HEADER}.asm")
    file(WRITE "${_placed}" "        ORG $${_org_hex}\n${_text}")
    _assemble("${_placed}" "${_bin}" _start _end)
endif()

list(APPEND _layout "${_start} ${_end}")
string(REPLACE ";" "\n" _layout_text "${_layout}")
file(WRITE "${LAYOUT_OUT}" "${_layout_text}\n")

file(READ "${_bin}" _hex HEX)
string(LENGTH "${_hex}" _hex_length)
math(EXPR _count "${_hex_length} / 2")
string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
math(EXPR _origin_hex "${_start}" OUTPUT_FORMAT HEXADECIMAL)
file(WRITE "${HEADER}"
    "// AUTO-GENERATED by retropp_autoembed_routines for '${LOGICAL}'. DO NOT EDIT.\n"
    "#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n"
    "#include \"retropp/isa.h\"\n"
    "namespace retropp::generated {\n"
    "inline constexpr std::array<std::uint8_t, ${_count}> ${SYMBOL} = { ${_bytes} };\n"
    "inline constexpr std::uint32_t ${SYMBOL}_origin = ${_origin_hex};\n"
    "inline constexpr retropp::Isa ${SYMBOL}_isa = retropp::Isa::${ISA};\n"
    "}  // namespace retropp::generated\n")
