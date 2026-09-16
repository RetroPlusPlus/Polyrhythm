# The transport's linkage measurement — a binary that names the transport shows it, and a binary that
# does not shows nothing of it.
#
# The transport is its own static library, so an archive member reaches a link only to satisfy an
# undefined symbol. That makes "a game that does not network carries no socket" a property of the link
# rather than of a build flag, and a property is worth exactly what measures it. This script reads two
# binaries built from the same tree: the test binary, which names the seam, and net_linkage_control,
# which names only the platform's version string.
#
# Both directions are asserted. Absence on its own can pass for the wrong reason — a renamed symbol, a
# reader that printed nothing, a pattern that never could have matched — so the referencing binary must
# show its evidence before the control's silence means anything. PROBE is the second half of that guard:
# a fragment both binaries must carry, so one whose output lacks it was never read successfully and the
# script says so rather than reporting an absence it did not observe.
#
# Two instruments, one per platform, because the two platforms keep different evidence:
#
#   tool      a symbol reader over the linked binary — nm on Apple and Unix, where it ships with the
#             toolchain and an executable keeps its symbol table. SYMBOL is a function name from
#             retropp::net, which reads the same through any mangling.
#
#   strings   the image's own printable content. A Release PE keeps no symbol table for any reader to
#             find, but it does keep its import table, and an imported library's name sits there as
#             plain ASCII. SYMBOL is that library; the claim becomes "this binary does not depend on the
#             OS socket library", which is the dependency the archive exists to keep out. Matching is
#             case-folded here, since an import name's case belongs to whoever wrote the import.
#
# Run as: cmake -DINSTRUMENT=tool|strings -DSYMBOL=<text> -DPROBE=<text>
#              -DREFERENCING=<binary> -DCONTROL=<binary> [-DTOOL=<symbol reader>]
#              -P tests/net_linkage.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required INSTRUMENT SYMBOL PROBE REFERENCING CONTROL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "net_linkage.cmake: -D${_required}=<value> is required")
    endif()
endforeach()

if(NOT INSTRUMENT MATCHES "^(tool|strings)$")
    message(FATAL_ERROR "net_linkage.cmake: -DINSTRUMENT must be tool or strings, not '${INSTRUMENT}'")
endif()
if(INSTRUMENT STREQUAL "tool" AND (NOT DEFINED TOOL OR "${TOOL}" STREQUAL ""))
    message(FATAL_ERROR "net_linkage.cmake: -DINSTRUMENT=tool also needs -DTOOL=<symbol reader>")
endif()

# Reads one binary's evidence and says where it came from.
function(retropp_net_read _binary _label _out_text)
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "net_linkage.cmake: the ${_label} binary is missing: ${_binary}")
    endif()

    if(INSTRUMENT STREQUAL "tool")
        execute_process(COMMAND "${TOOL}" "${_binary}"
                        OUTPUT_VARIABLE _text
                        ERROR_VARIABLE  _tool_error
                        RESULT_VARIABLE _tool_status)
        if(NOT _tool_status EQUAL 0)
            message(FATAL_ERROR
                "net_linkage.cmake: ${TOOL} failed on the ${_label} binary ${_binary} "
                "(status ${_tool_status}).\n  ${_tool_error}")
        endif()
        set(_source "${TOOL}")
    else()
        file(STRINGS "${_binary}" _found LENGTH_MINIMUM 4)
        string(REPLACE ";" "\n" _text "${_found}")
        string(TOLOWER "${_text}" _text)
        set(_source "the image's own strings")
    endif()

    if(NOT _text MATCHES "${PROBE}")
        message(FATAL_ERROR
            "net_linkage.cmake: ${_source} read the ${_label} binary ${_binary} but its content carries "
            "no '${PROBE}', so it is not reporting this build. The measurement was not made — an absent "
            "symbol here would prove nothing.")
    endif()

    message(STATUS "net linkage: read the ${_label} binary from ${_source}")
    set(${_out_text} "${_text}" PARENT_SCOPE)
endfunction()

retropp_net_read("${REFERENCING}" "referencing" _referencing)
retropp_net_read("${CONTROL}"     "control"     _control)

# SYMBOL is comma-separated rather than a CMake list, so it survives being handed through add_test as one
# argument. Each entry is asserted in both directions.
string(REPLACE "," ";" _wanted "${SYMBOL}")

foreach(_needle IN LISTS _wanted)
    if(NOT _referencing MATCHES "${_needle}")
        message(FATAL_ERROR
            "net linkage FAILED — the referencing binary ${REFERENCING} shows no '${_needle}'. Either "
            "that part of the transport is absent from a link that names it, or what this test asserts "
            "on has been renamed. Until this direction holds, the control's silence measures nothing.")
    endif()

    if(_control MATCHES "${_needle}")
        message(FATAL_ERROR
            "net linkage FAILED — ${CONTROL} declares no transport yet shows '${_needle}'. Something "
            "reaches it unconditionally, so every game that links the platform now carries a dependency "
            "it never asked for. For an OS library this is usually a link line that records it whether "
            "or not anything calls it.")
    endif()

    message(STATUS "net linkage: '${_needle}' is present where the transport is named and absent where it is not — OK")
endforeach()
