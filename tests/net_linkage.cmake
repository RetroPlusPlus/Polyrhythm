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
# The evidence is the image's own printable content, on every platform and with no tool to locate. It
# answers both halves of the claim at once: a symbol name from retropp::net is in there as text, and so
# is the name of every OS library the image records a dependency on — which is the cost that matters,
# since a library nothing calls is still opened at every start when the link line recorded it. A symbol
# reader sees only the first half, and a Release PE keeps no symbol table for one to find. Matching is
# case-folded, since a library name's case belongs to whoever wrote the import.
#
# Run as: cmake -DSYMBOL=<text> -DPROBE=<text> -DREFERENCING=<binary> -DCONTROL=<binary>
#              -P tests/net_linkage.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required SYMBOL PROBE REFERENCING CONTROL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "net_linkage.cmake: -D${_required}=<value> is required")
    endif()
endforeach()

# Reads one binary's evidence.
function(retropp_net_read _binary _label _out_text)
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "net_linkage.cmake: the ${_label} binary is missing: ${_binary}")
    endif()

    file(STRINGS "${_binary}" _found LENGTH_MINIMUM 4)
    string(REPLACE ";" "\n" _text "${_found}")
    string(TOLOWER "${_text}" _text)

    if(NOT _text MATCHES "${PROBE}")
        message(FATAL_ERROR
            "net_linkage.cmake: the ${_label} binary ${_binary} was read but its content carries no "
            "'${PROBE}', so it is not reporting this build. The measurement was not made — an absent "
            "symbol here would prove nothing.")
    endif()

    message(STATUS "net linkage: read the ${_label} binary from the image's own strings")
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
