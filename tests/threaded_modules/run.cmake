# Runs the threaded-modules host and compares its stdout to the pinned bytes —
# two_module's ratchet, applied to the two-thread host. The host prints only
# after the join, in a fixed order, which is what makes byte comparison sound
# for a concurrent run.

if(NOT DEFINED EXE OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "threaded-modules host exited ${_status}; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "threaded-modules output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
