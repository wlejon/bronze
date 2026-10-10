# Runs the static-brass host and compares its stdout to the pinned bytes.

if(NOT DEFINED EXE OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run.cmake needs -DEXE=, -DEXPECTED= and -DACTUAL=")
endif()

execute_process(COMMAND "${EXE}" OUTPUT_FILE "${ACTUAL}" ERROR_VARIABLE _stderr RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "static-brass host exited ${_status}; output so far:\n${_partial}\nstderr:\n${_stderr}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "static-brass host output differs.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
