# Runs the shared-load harness and compares its stdout to the pinned bytes —
# the same ratchet the oracle suite applies, and the same script shape
# tests/two_module/run.cmake uses, with the two module paths passed through.
#
# Its own script rather than PASS_REGULAR_EXPRESSION for the reason that one
# gives, and one more that matters here: the fake module's entry prints if it is
# ever called, and only a whole-output comparison notices a LINE THAT SHOULD NOT
# BE THERE.

if(NOT DEFINED EXE OR NOT DEFINED MODULE OR NOT DEFINED FAKE
   OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DMODULE=, -DFAKE=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" "${MODULE}" "${FAKE}"
                OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "shared-load harness exited ${_status}; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "shared-load output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
