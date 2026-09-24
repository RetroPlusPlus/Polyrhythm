# The cores' linkage measurement — a binary that constructs a machine carries the core its constructor
# names, and a binary that names no core carries none.
#
# A Vm constructor resolves its platform's core where the game writes it, and each core is its own member
# of the engine archive, so a core reaches a link only when the game's own code names it. This script
# reads two binaries built from the same tree: REFERENCING must show NEEDLE, a string literal that is in
# an image exactly when that core is, and CONTROL must not.
#
# Both directions are asserted. Absence on its own can pass for the wrong reason — an edited literal, a
# reader that printed nothing, a pattern that never could have matched — so the referencing binary must
# show its evidence before the control's silence means anything. PROBE is a fragment both binaries must
# carry, so one whose content lacks it was never read successfully and the script says so rather than
# reporting an absence it did not observe.
#
# The evidence is the image's own printable content, read the same way on every platform; a Release image
# keeps no symbol table, and a string literal survives in it. Matching is case-folded.
#
# A constant platform folds to its one core only when the compiler optimizes. FOLDED_CONTROL=ON marks a
# row whose control depends on that fold; under a Debug or unnamed configuration such a row reports itself
# skipped, with the reason, instead of asserting.
#
# Run as: cmake -DNEEDLE=<text> -DPROBE=<text> -DREFERENCING=<binary> -DCONTROL=<binary>
#              [-DCONFIG=<configuration>] [-DFOLDED_CONTROL=ON] -P tests/vm_linkage.cmake

cmake_minimum_required(VERSION 3.28)

foreach(_required NEEDLE PROBE REFERENCING CONTROL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "vm_linkage.cmake: -D${_required}=<value> is required")
    endif()
endforeach()

if(FOLDED_CONTROL AND ("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug"))
    message(STATUS "vm linkage SKIPPED — the raw form folds only in an optimized build")
    return()
endif()

# Reads one binary's evidence.
function(retropp_vm_read _binary _label _out_text)
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "vm_linkage.cmake: the ${_label} binary is missing: ${_binary}")
    endif()

    file(STRINGS "${_binary}" _found LENGTH_MINIMUM 4)
    string(REPLACE ";" "\n" _text "${_found}")
    string(TOLOWER "${_text}" _text)

    if(NOT _text MATCHES "${PROBE}")
        message(FATAL_ERROR
            "vm_linkage.cmake: the ${_label} binary ${_binary} was read but its content carries no "
            "'${PROBE}', so it is not reporting this build. The measurement was not made — an absent "
            "literal here would prove nothing.")
    endif()

    message(STATUS "vm linkage: read the ${_label} binary from the image's own strings")
    set(${_out_text} "${_text}" PARENT_SCOPE)
endfunction()

retropp_vm_read("${REFERENCING}" "referencing" _referencing)
retropp_vm_read("${CONTROL}"     "control"     _control)

if(NOT _referencing MATCHES "${NEEDLE}")
    message(FATAL_ERROR
        "vm linkage FAILED — the referencing binary ${REFERENCING} shows none of '${NEEDLE}'. Either the "
        "core is absent from a link whose constructor names it, or the literal this test asserts on has "
        "changed. Until this direction holds, the control's silence measures nothing.")
endif()

if(_control MATCHES "${NEEDLE}")
    message(FATAL_ERROR
        "vm linkage FAILED — ${CONTROL} names no such core yet the control shows it ('${NEEDLE}'). "
        "Something reaches the core unconditionally, so every game that links the platform carries a "
        "core it never named.")
endif()

message(STATUS "vm linkage: '${NEEDLE}' is present where its core is named and absent where it is not — OK")
