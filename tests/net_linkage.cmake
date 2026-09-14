# The transport's linkage measurement — a binary that names the transport carries its symbols, and a
# binary that does not carries none.
#
# The transport is its own static library, so an archive member reaches a link only to satisfy an
# undefined symbol. That makes "a game that does not network carries no socket" a property of the link
# rather than of a build flag, and a property is worth exactly what measures it. This script reads the
# symbols of two binaries built from the same tree: the test binary, which names the seam, and
# net_linkage_control, which names only the platform's version string.
#
# Both directions are asserted. Absence on its own can pass for the wrong reason — a renamed symbol, a
# reader that printed nothing, a pattern that never could have matched — so the referencing binary must
# show the symbol before the control's silence means anything.
#
# PROBE guards the reader itself: it is a fragment every engine symbol carries, so a binary whose
# output lacks it was never read successfully, and the script says so instead of reporting an absence
# it did not observe. A Release PE can carry no symbol table at all, which is why MAP accompanies the
# tool on Windows and answers when the tool stays silent.
#
# Run as: cmake -DSYMBOL=<identifier> -DPROBE=<fragment> -DREFERENCING=<binary> -DCONTROL=<binary>
#              -DTOOL=<symbol reader> [-DTOOL_ARGS=<args>]
#              [-DREFERENCING_MAP=<linker map>] [-DCONTROL_MAP=<linker map>]
#              -P tests/net_linkage.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required SYMBOL PROBE REFERENCING CONTROL TOOL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "net_linkage.cmake: -D${_required}=<value> is required")
    endif()
endforeach()

# Read one binary's symbols, and say which source answered. A tool that exits non-zero or prints
# nothing recognisable hands over to the linker map; when neither carries the probe the measurement has
# not been made, and that is a failure rather than an absence.
function(retropp_net_read_symbols _binary _map _label _out_text)
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "net_linkage.cmake: ${_label} binary is missing: ${_binary}")
    endif()

    separate_arguments(_tool_args UNIX_COMMAND "${TOOL_ARGS}")
    execute_process(COMMAND "${TOOL}" ${_tool_args} "${_binary}"
                    OUTPUT_VARIABLE _text
                    ERROR_VARIABLE  _tool_error
                    RESULT_VARIABLE _tool_status)

    if(NOT _tool_status EQUAL 0 OR NOT _text MATCHES "${PROBE}")
        if(_map AND EXISTS "${_map}")
            file(READ "${_map}" _text)
            set(_source "the linker map ${_map}")
        else()
            message(FATAL_ERROR
                "net_linkage.cmake: ${TOOL} produced no symbol information for the ${_label} binary "
                "${_binary} (status ${_tool_status}), and no linker map stands in for it. The "
                "measurement was not made — an absent symbol here would prove nothing.\n"
                "  tool output: ${_text}\n"
                "  tool error:  ${_tool_error}")
        endif()
    else()
        set(_source "${TOOL}")
    endif()

    if(NOT _text MATCHES "${PROBE}")
        message(FATAL_ERROR
            "net_linkage.cmake: ${_source} read the ${_label} binary but its output carries no '${PROBE}', "
            "so the reader is looking at something other than this build's symbols.")
    endif()

    message(STATUS "net linkage: read the ${_label} binary's symbols from ${_source}")
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
