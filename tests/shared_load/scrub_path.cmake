# What "no developer environment" means, in one place.
#
# Included by both CMakeLists.txt (to decide whether the scrubbed run is even
# possible on this machine) and run_no_vcvars.cmake (to perform it). One
# definition, because a test that scrubs differently from the check that gates
# it is a test that can silently stop scrubbing.
#
# The list is the Visual Studio installation and the Windows SDK, and the reason
# each entry is here is that lld-link will find the CRT through it:
#
#   VC/Tools/MSVC/    LLVM's findVCToolChainViaEnvironment walks %PATH% looking
#                     for exactly this shape and derives the library directory
#                     from it. Unsetting %LIB% alone leaves this working, which
#                     is how the first version of the no-vcvars test passed
#                     against a bronze that still had the bug.
#   VC/Tools/Llvm/    Visual Studio's own bundled lld-link. It sits inside the
#                     VS installation and finds the toolchain relative to
#                     itself, so leaving it on PATH also leaves the bug hidden.
#                     Dropping it is what forces a standalone LLVM — a normal
#                     user's linker.
#   Windows Kits/     the SDK's binaries, same reasoning.
#   Common7/, MSBuild/  the rest of the developer shell, dropped for company
#                     rather than because either is known to matter.
#
# Everything else is kept deliberately: standalone LLVM, the system directories,
# vcpkg, git. The point is a plain shell, not an empty one.
#
# On non-Windows PATH is one `:`-separated string, so the loop sees a single
# element, nothing matches, and the caller gets its PATH back unchanged.
function(bronze_scrub_dev_path out_var)
    set(_entries "$ENV{PATH}")
    set(_kept "")
    foreach(_entry IN LISTS _entries)
        file(TO_CMAKE_PATH "${_entry}" _norm)
        if(_norm MATCHES "/VC/Tools/MSVC/" OR _norm MATCHES "/VC/Tools/Llvm/"
           OR _norm MATCHES "/Windows Kits/" OR _norm MATCHES "/MSBuild/"
           OR _norm MATCHES "/Common7/")
            continue()
        endif()
        list(APPEND _kept "${_entry}")
    endforeach()
    set(${out_var} "${_kept}" PARENT_SCOPE)
endfunction()
