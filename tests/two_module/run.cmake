# Runs the two-module host and compares its stdout to the pinned bytes, the
# same ratchet the oracle suite applies: the expectation is a committed file and
# the comparison is byte-for-byte.
#
# Its own script rather than PASS_REGULAR_EXPRESSION because a regex would let
# an output drift past as long as one line still matched.

if(NOT DEFINED EXE OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "two-module host exited ${_status}; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "two-module output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
