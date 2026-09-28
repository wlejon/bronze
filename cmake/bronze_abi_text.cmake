# The text of the generated-code ABI header, as one piece.
#
# src/abi/bronze_abi.h is kept in three files so that none of them runs past
# the repository's size limit: the header itself, bronze_abi_functions.h (the
# helper registry and the brass symbols beside it) and bronze_abi_layout.h (the
# inline-cache contracts and the object layouts generated code reads). The two
# parts are TEXTUAL pieces of the one header, not headers of their own: each is
# included exactly once, at the point its text belongs, and neither has a guard
# or an include of its own.
#
# Both readers of the header's text — the ABI fingerprint (src/abi) and the
# shared runtime's export scan (bronze_abi_exports.cmake) — read it through
# this function, which puts each part back in place of its `#include` line.
# The fingerprint is therefore a function of the ABI's text and not of how that
# text is split: the split changed no byte of it, and moving a line from one
# part to another does not move the fingerprint either. Only a part named in
# `_BRONZE_ABI_TEXT_PARTS` is expanded, so bronze_abi_tls.h (hashed on its own,
# beside this text) keeps its ordinary include.

set(_BRONZE_ABI_TEXT_PARTS bronze_abi_functions.h bronze_abi_layout.h)

# bronze_abi_read_text(<header> <out_var>)
#
# <out_var> receives the header's text with every part expanded in place, and
# each file read goes on CMAKE_CONFIGURE_DEPENDS of the calling directory, so
# an edit to any of them re-runs configure.
function(bronze_abi_read_text header out_var)
    get_filename_component(_dir "${header}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")
    file(READ "${header}" _text)
    foreach(_part IN LISTS _BRONZE_ABI_TEXT_PARTS)
        set(_line "#include \"abi/${_part}\"\n")
        string(FIND "${_text}" "${_line}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR
                "bronze_abi_read_text: ${header} does not include abi/${_part} on a line of its "
                "own. The ABI fingerprint and the export scan read the header with its parts "
                "put back in place; a part that is no longer included that way would drop out "
                "of both.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dir}/${_part}")
        file(READ "${_dir}/${_part}" _part_text)
        string(REPLACE "${_line}" "${_part_text}" _text "${_text}")
    endforeach()
    set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()
