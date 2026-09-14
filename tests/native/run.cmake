# Runs the native harness and compares its stdout to the pinned bytes — the
# tests/shared_load/run.cmake shape. A whole-output comparison, because the
# second module's entry prints if it is ever called and only a full diff
# notices a line that should not be there.

if(NOT DEFINED EXE OR NOT DEFINED MODULE OR NOT DEFINED MISSING
   OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DMODULE=, -DMISSING=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" "${MODULE}" "${MISSING}"
                OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "native harness exited ${_status}; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "native harness output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
