# The transport's linkage measurement — a binary that names the transport carries its symbols, and a
# binary that does not carries none.
#
# The transport is its own static library, so an archive member reaches a link only to satisfy an
# undefined symbol. That makes "a game that does not network carries no socket" a property of the link
# rather than of a build flag, and a property is worth exactly what measures it. This script reads two
# binaries built from the same tree: the test binary, which names the seam, and net_linkage_control,
# which names only the platform's version string.
#
# Both directions are asserted. Absence on its own can pass for the wrong reason — a renamed symbol, a
# reader that printed nothing, a pattern that never could have matched — so the referencing binary must
# show the symbol before the control's silence means anything.
#
# Two instruments, one per platform, and the caller declares which:
#
#   TOOL             a symbol reader over the linked binary. nm on Apple and Unix, where it ships with
#                    the toolchain and reads an executable directly.
#   *_MAP            the linker's own map — its statement of which archive members it pulled into the
#                    link. This is what answers on Windows, where a Release PE carries no symbol table
#                    for any reader to find. It needs nothing beyond the linker already doing the link.
#
# A declared map must exist. Asking the linker for one and finding none is a failure to report, never a
# reason to reach for something else.
#
# PROBE guards whichever instrument is used: it is a fragment every engine symbol carries, so a binary
# whose output lacks it was never read successfully, and the script says so rather than reporting an
# absence it did not observe.
#
# Run as: cmake -DSYMBOL=<identifier> -DPROBE=<fragment> -DREFERENCING=<binary> -DCONTROL=<binary>
#              { -DTOOL=<symbol reader> | -DREFERENCING_MAP=<map> -DCONTROL_MAP=<map> }
#              -P tests/net_linkage.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required SYMBOL PROBE REFERENCING CONTROL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "net_linkage.cmake: -D${_required}=<value> is required")
    endif()
endforeach()

# Reads one binary's symbols and says which instrument answered.
function(retropp_net_read_symbols _binary _map _label _out_text)
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "net_linkage.cmake: the ${_label} binary is missing: ${_binary}")
    endif()

    set(_text "")
    set(_source "")

    if(NOT "${_map}" STREQUAL "")
        if(NOT EXISTS "${_map}")
            message(FATAL_ERROR
                "net_linkage.cmake: the linker was asked for a map of the ${_label} binary at\n"
                "  ${_map}\n"
                "and none is there, so the measurement cannot be made. Check that the /MAP: link option "
                "reached this target's link step.")
        endif()
        file(READ "${_map}" _text)
        set(_source "the linker map ${_map}")
    elseif(DEFINED TOOL AND NOT "${TOOL}" STREQUAL "")
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
        message(FATAL_ERROR
            "net_linkage.cmake: no instrument was declared for the ${_label} binary — pass -DTOOL or a "
            "linker map. Without one the script can measure nothing.")
    endif()

    if(NOT _text MATCHES "${PROBE}")
        message(FATAL_ERROR
            "net_linkage.cmake: ${_source} carries no '${PROBE}', so it is not reporting this build's "
            "symbols. The measurement was not made — an absent symbol here would prove nothing.")
    endif()

    message(STATUS "net linkage: read the ${_label} binary from ${_source}")
    set(${_out_text} "${_text}" PARENT_SCOPE)
endfunction()

retropp_net_read_symbols("${REFERENCING}" "${REFERENCING_MAP}" "referencing" _referencing_symbols)
retropp_net_read_symbols("${CONTROL}"     "${CONTROL_MAP}"     "control"     _control_symbols)

if(NOT _referencing_symbols MATCHES "${SYMBOL}")
    message(FATAL_ERROR
        "net linkage FAILED — the referencing binary ${REFERENCING} carries no '${SYMBOL}'. Either the "
        "transport is absent from a link that names it, or the identifier this test asserts on has been "
        "renamed. Until this direction holds, the control's silence measures nothing.")
endif()

if(_control_symbols MATCHES "${SYMBOL}")
    message(FATAL_ERROR
        "net linkage FAILED — ${CONTROL} declares no transport yet carries '${SYMBOL}'. Something "
        "references the transport unconditionally, so every game that links the platform now carries a "
        "socket it never asked for.")
endif()

message(STATUS "net linkage: '${SYMBOL}' is present where it is named and absent where it is not — OK")
