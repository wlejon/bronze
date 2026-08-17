# `--emit-shared` with the MSVC developer environment SCRUBBED, then the module
# it produced run through the same harness and compared to the same pinned
# bytes.
#
# This exists because of a defect the rest of the suite could not see. ctest
# runs under dev.cmd, where %LIB% names the CRT and the Windows SDK, so every
# link in this tree had an environment to lean on; a user's shell does not.
# `bronze build -o app.exe` never needed one — bronze_rt.lib's MSVC-compiled
# objects carry /DEFAULTLIB: directives and lld-link resolves those through its
# own toolchain detection — and `--emit-shared` briefly did, because it named
# `msvcrt.lib` as an input file, which is a PATH the linker opens rather than a
# request it resolves. From a shell with an empty %LIB% it could not be opened.
#
# So the test unsets the variables rather than trusting a comment. `cmake -E
# env --unset=` is the only portable way to run one command with a scrubbed
# environment, which is why this is a script and not another add_test line.
#
# %LIB% is NOT the whole of the developer environment, and finding that out is
# what made this script worth writing: with only the variables unset, a bronze
# with the bug deliberately put back still passed. PATH has to be scrubbed as
# well, and scrub_path.cmake holds the list and the reason for each entry.
#
# It links AND runs: a linker exiting 0 is not the claim. The claim is that the
# module it wrote loads into the shared runtime and behaves exactly like the one
# built by the build system.

if(NOT DEFINED BRONZE OR NOT DEFINED MODJS OR NOT DEFINED MANIFEST OR NOT DEFINED RTLIB
   OR NOT DEFINED MODULE OR NOT DEFINED EXE OR NOT DEFINED FAKE
   OR NOT DEFINED EXPECTED OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR "run_no_vcvars.cmake needs -DBRONZE=, -DMODJS=, -DMANIFEST=, "
                        "-DRTLIB=, -DMODULE=, -DEXE=, -DFAKE=, -DEXPECTED= and -DACTUAL=")
endif()

# A stale artefact from a previous run must not be able to make this pass.
file(REMOVE "${MODULE}")

include("${CMAKE_CURRENT_LIST_DIR}/scrub_path.cmake")
bronze_scrub_dev_path(_kept)
if(WIN32)
    list(JOIN _kept ";" _scrubbed_path)
else()
    list(JOIN _kept ":" _scrubbed_path)
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            --unset=LIB --unset=INCLUDE --unset=LIBPATH --unset=VCToolsInstallDir
            --unset=VCINSTALLDIR --unset=WindowsSdkDir --unset=UCRTVersion
            --unset=WindowsSdkBinPath --unset=WindowsSDKLibVersion
            --unset=VCToolsVersion --unset=VSINSTALLDIR
            "PATH=${_scrubbed_path}"
            BRONZE_SHARED_RT_LIB=${RTLIB}
            "${BRONZE}" build "${MODJS}"
            --emit-shared
            --entry-symbol=bronze_shared_demo
            --host-globals "${MANIFEST}"
            -o "${MODULE}"
    RESULT_VARIABLE _build_status
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_status EQUAL 0)
    message(FATAL_ERROR
        "bronze --emit-shared failed with the developer environment scrubbed "
        "(exit ${_build_status}). This is the environment a user's shell has.\n"
        "--- stdout ---\n${_build_out}--- stderr ---\n${_build_err}")
endif()
if(NOT EXISTS "${MODULE}")
    message(FATAL_ERROR "bronze --emit-shared reported success but wrote no ${MODULE}")
endif()

execute_process(COMMAND "${EXE}" "${MODULE}" "${FAKE}"
                OUTPUT_FILE "${ACTUAL}" RESULT_VARIABLE _run_status)
if(NOT _run_status EQUAL 0)
    file(READ "${ACTUAL}" _partial)
    message(FATAL_ERROR "harness exited ${_run_status} on the module linked without a "
                        "developer environment; output so far:\n${_partial}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ACTUAL}" "${EXPECTED}"
                RESULT_VARIABLE _differs)
if(NOT _differs EQUAL 0)
    file(READ "${ACTUAL}" _got)
    file(READ "${EXPECTED}" _want)
    message(FATAL_ERROR "shared-load output differs for the module linked without a "
                        "developer environment.\n--- expected ---\n${_want}--- actual ---\n${_got}")
endif()
