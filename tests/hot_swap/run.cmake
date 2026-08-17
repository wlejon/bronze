# Runs the hot-swap harness and compares its stdout to the pinned bytes — the
# same ratchet the oracle suite applies, the same script shape
# tests/shared_load/run.cmake uses, with the two version paths passed through.

if(NOT DEFINED EXE OR NOT DEFINED V1 OR NOT DEFINED V2
   OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DV1=, -DV2=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" "${V1}" "${V2}"
                OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "hot-swap harness exited ${_status}; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "hot-swap output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
