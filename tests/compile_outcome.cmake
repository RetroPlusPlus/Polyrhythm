# Build one target and assert whether it compiles. A ctest row runs this with -DTARGET=<name>
# -DEXPECT=fails|builds [-DDIAGNOSTIC=<text>], so the voice literal's compile-time refusal is a test like
# any other. EXPECT=fails also checks the compiler's message carries DIAGNOSTIC, so a target that fails to
# build for some other reason is not mistaken for the refusal. --config selects the configuration under a
# multi-configuration generator and is ignored by a single-configuration one.

execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${BINARY_DIR} --target ${TARGET} --config ${CONFIG}
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
set(_log "${_out}${_err}")

if(EXPECT STREQUAL "fails")
    if(_rc EQUAL 0)
        message(FATAL_ERROR "expected ${TARGET} not to compile, but it built:\n${_log}")
    endif()
    if(DEFINED DIAGNOSTIC AND NOT "${_log}" MATCHES "${DIAGNOSTIC}")
        message(FATAL_ERROR "expected ${TARGET} to fail with '${DIAGNOSTIC}', but it failed otherwise:\n${_log}")
    endif()
    message(STATUS "${TARGET} did not compile, as expected")
elseif(EXPECT STREQUAL "builds")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "expected ${TARGET} to compile, but it failed:\n${_log}")
    endif()
    message(STATUS "${TARGET} compiled, as expected")
else()
    message(FATAL_ERROR "EXPECT must be fails or builds, not '${EXPECT}'")
endif()
