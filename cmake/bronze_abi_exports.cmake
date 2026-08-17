# The shared runtime's C ABI export surface, derived from the registry in
# src/abi/bronze_abi.h and from nothing else.
#
# Windows exports NOTHING from a DLL that is not named, so a shared runtime
# needs a list. The two ways to write that list are both worse than generating
# it: a hand-maintained .def is a second copy of the ABI that drifts the first
# time a chunk adds a helper — the exact failure the registry exists to make
# impossible — and a dllexport macro spread across the 95 runtime sources puts
# a build concern in every file that happens to define a helper, and still
# cannot express the `DATA` tag a data symbol needs.
#
# So the list is SCANNED out of the registry macros at configure time. Adding
# an `X(...)` line to BRONZE_ABI_FUNCTIONS or BRONZE_ABI_GLOBALS remains the
# only edit a new ABI symbol needs: the header is on CMAKE_CONFIGURE_DEPENDS
# (src/abi puts it there for the fingerprint; this file repeats it so the
# property does not depend on that directory being configured first), so the
# edit re-runs configure and every export file is rewritten from the new text.
#
# The scan is CHECKED rather than trusted. Every line in the header that opens
# with `X(` must have been claimed by one of the two blocks; a third registry
# macro, or a reformatting that breaks the block detection, is a configure-time
# error naming the count instead of a symbol that silently stops being
# exported and surfaces as an unresolved import in someone else's build.
#
# The C++ half of the surface — the embed API — is NOT here. It is annotated at
# its declarations with BRONZE_EMBED_API (src/embed/embed.h says why that one
# header may be annotated and the runtime sources may not), so on Windows it
# arrives through dllexport and on ELF/Mach-O through a mangled-name wildcard
# in the two files below.

# The text of the macro block that `define_line` opens, and everything after
# it. A block runs from its `#define` to the first blank line, which is what
# ends a multi-line macro in C and therefore what ends one here.
function(bronze_abi_cut_block text define_line block_var after_var)
    string(FIND "${text}" "${define_line}" _start)
    if(_start LESS 0)
        message(FATAL_ERROR
            "bronze_abi_export_files: ${define_line} is not in the ABI header. The registry "
            "is the source of the shared runtime's export list; if it was renamed, this "
            "scan has to be renamed with it.")
    endif()
    string(SUBSTRING "${text}" ${_start} -1 _rest)
    string(FIND "${_rest}" "\n\n" _end)
    if(_end LESS 0)
        message(FATAL_ERROR
            "bronze_abi_export_files: ${define_line} is not followed by a blank line, so "
            "its extent cannot be determined.")
    endif()
    string(SUBSTRING "${_rest}" 0 ${_end} _block)
    string(SUBSTRING "${_rest}" ${_end} -1 _after)
    set(${block_var} "${_block}" PARENT_SCOPE)
    set(${after_var} "${_after}" PARENT_SCOPE)
endfunction()

# The names of the `X(name, ...)` entries in a block, in the order the block
# gives them - which is the order the export files list them in, so the
# generated files are a function of the header and nothing else.
function(bronze_abi_entry_names block names_var)
    set(_names "")
    string(REGEX MATCHALL "\n[ \t]*X[(][A-Za-z_][A-Za-z0-9_]*[ \t]*," _entries "${block}")
    foreach(_entry IN LISTS _entries)
        # MATCH plus CMAKE_MATCH_1 rather than REGEX REPLACE with a
        # backreference: a backreference is spelled with a backslash, and what
        # CMake's argument parser does to a backslash in a quoted string is the
        # one thing this file must not depend on.
        string(REGEX MATCH "X[(]([A-Za-z_][A-Za-z0-9_]*)" _matched "${_entry}")
        list(APPEND _names "${CMAKE_MATCH_1}")
    endforeach()
    set(${names_var} "${_names}" PARENT_SCOPE)
endfunction()

# bronze_abi_export_files(<header> <outdir> <def_var> <ver_var> <exp_var>)
#
# Writes <outdir>/bronze_abi.def (Windows), bronze_abi.ver (ELF version
# script) and bronze_abi.exp (Mach-O exported-symbols list), and sets the
# three named variables to their paths in the caller's scope.
function(bronze_abi_export_files header outdir def_var ver_var exp_var)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")

    file(READ "${header}" _text)
    string(REPLACE "\r" "" _text "${_text}")

    # The scan works on the TEXT, never on a list of lines. Splitting a C
    # header on newline into CMake's list separator does not survive contact
    # with this header: every line of a registry macro ends in a backslash,
    # which is CMake's escape for that separator, so the whole registry
    # collapses into one element and nothing inside it is ever seen.
    #
    # So each block is cut out by position instead — from its `#define` to the
    # blank line that ends it — and the entries are matched out of the cut with
    # one regex. Both blocks are contiguous by construction: a registry entry
    # and the prose explaining it are one macro, and a blank line inside a
    # macro would end the macro.
    bronze_abi_cut_block("${_text}" "#define BRONZE_ABI_FUNCTIONS(X)" _fns_text _after_fns)
    bronze_abi_cut_block("${_text}" "#define BRONZE_ABI_GLOBALS(X)" _globals_text _after_globals)

    bronze_abi_entry_names("${_fns_text}" _fns)
    bronze_abi_entry_names("${_globals_text}" _globals)

    if(_fns STREQUAL "")
        message(FATAL_ERROR
            "bronze_abi_export_files: found no X(...) lines in BRONZE_ABI_FUNCTIONS in "
            "${header}. The scan that produces the shared runtime's export list is broken; "
            "a DLL built from an empty list exports nothing and every compiled module "
            "fails to link against it.")
    endif()
    if(_globals STREQUAL "")
        message(FATAL_ERROR
            "bronze_abi_export_files: found no X(...) lines in BRONZE_ABI_GLOBALS in "
            "${header}. See above - the data symbols are the half a module cannot reach "
            "through an import thunk, so a missing one is a wrong address, not a link error.")
    endif()

    # Nothing that LOOKS like a registry entry may sit outside the two blocks.
    # That is the check that keeps this scan honest as the header grows: a
    # third registry macro, or an entry accidentally left after a block's
    # blank line, is a configure-time error here instead of a symbol that
    # quietly stops being exported and surfaces as an unresolved import in
    # someone else's build.
    bronze_abi_entry_names("${_after_globals}" _stray)
    if(NOT _stray STREQUAL "")
        message(FATAL_ERROR
            "bronze_abi_export_files: ${_stray} in ${header} look like registry entries but "
            "fall outside BRONZE_ABI_FUNCTIONS and BRONZE_ABI_GLOBALS. Either a third "
            "registry macro was added - teach this scan about it - or a block grew a blank "
            "line and now ends early.")
    endif()

    # ---- Windows: the module-definition file -------------------------------
    set(_def_text "; Generated from ${header} by cmake/bronze_abi_exports.cmake.\n")
    string(APPEND _def_text "; DO NOT EDIT: add an X(...) line to the registry instead.\n")
    string(APPEND _def_text "EXPORTS\n")
    foreach(_name IN LISTS _fns)
        string(APPEND _def_text "    ${_name}\n")
    endforeach()
    # DATA is not decoration: without it the linker exports the address of a
    # thunk, and a module that took the import through one would read its
    # exception cell out of code bytes.
    foreach(_name IN LISTS _globals)
        string(APPEND _def_text "    ${_name} DATA\n")
    endforeach()

    # ---- ELF: the version script -------------------------------------------
    #
    # `local: *` is what makes the list a surface rather than a suggestion: an
    # ELF DSO exports every global symbol by default, so without it the runtime
    # would publish its own internals and a host could bind to one by accident.
    #
    # The embed API rides in as a wildcard over its MANGLED names rather than
    # through an `extern "C++"` block: a plain glob is matched by every linker
    # against the symbol table as it stands, with no demangler in the loop.
    set(_ver_text "# Generated from ${header} by cmake/bronze_abi_exports.cmake.\n")
    string(APPEND _ver_text "# DO NOT EDIT: add an X(...) line to the registry instead.\n")
    string(APPEND _ver_text "{\n  global:\n")
    foreach(_name IN LISTS _fns _globals)
        string(APPEND _ver_text "    ${_name};\n")
    endforeach()
    string(APPEND _ver_text "    _ZN6bronze5embed*;\n")
    string(APPEND _ver_text "    _ZNK6bronze5embed*;\n")
    string(APPEND _ver_text "  local:\n    *;\n};\n")

    # ---- Mach-O: the exported-symbols list ---------------------------------
    #
    # Same surface, ld64's spelling: one symbol per line, with the leading
    # underscore Mach-O gives every C symbol (and the second one an Itanium
    # mangled name picks up on top of it).
    set(_exp_text "# Generated from ${header} by cmake/bronze_abi_exports.cmake.\n")
    string(APPEND _exp_text "# DO NOT EDIT: add an X(...) line to the registry instead.\n")
    foreach(_name IN LISTS _fns _globals)
        string(APPEND _exp_text "_${_name}\n")
    endforeach()
    string(APPEND _exp_text "__ZN6bronze5embed*\n")
    string(APPEND _exp_text "__ZNK6bronze5embed*\n")

    # file(GENERATE) rather than file(WRITE): it leaves an unchanged file
    # untouched, so a configure re-run for an unrelated reason does not restamp
    # the mtime and relink the runtime.
    file(GENERATE OUTPUT "${outdir}/bronze_abi.def" CONTENT "${_def_text}")
    file(GENERATE OUTPUT "${outdir}/bronze_abi.ver" CONTENT "${_ver_text}")
    file(GENERATE OUTPUT "${outdir}/bronze_abi.exp" CONTENT "${_exp_text}")

    set(${def_var} "${outdir}/bronze_abi.def" PARENT_SCOPE)
    set(${ver_var} "${outdir}/bronze_abi.ver" PARENT_SCOPE)
    set(${exp_var} "${outdir}/bronze_abi.exp" PARENT_SCOPE)
endfunction()
