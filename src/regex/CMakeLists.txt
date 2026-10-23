if(NOT TARGET brass)
    if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
        set(FETCHCONTENT_SOURCE_DIR_BRASS "$ENV{BRASS_ROOT}")
    elseif(BRASS_ROOT AND NOT BRASS_ROOT STREQUAL _BRONZE_BRASS_ROOT_CHOSEN)
        set(FETCHCONTENT_SOURCE_DIR_BRASS "${BRASS_ROOT}")
    endif()
    bro_dependency(brass)
    set(BRASS_ROOT "${brass_SOURCE_DIR}" CACHE PATH "brass source tree" FORCE)
    set(_BRONZE_BRASS_ROOT_CHOSEN "${brass_SOURCE_DIR}" CACHE INTERNAL "")
endif()

bronze_add_module(regex
    SOURCES
        chars.cpp
        parser.cpp
        parser_class.cpp
        matcher.cpp
        regex_jit.cpp
        unicode.cpp
        unicode_data_gc.cpp
        unicode_data_id.cpp
        unicode_data_scf.cpp
        unicode_data_script.cpp
    TEST_SOURCES
        ../../tests/regex/regex_test.cpp
        ../../tests/regex/chars_test.cpp
        ../../tests/regex/class_set_test.cpp
        ../../tests/regex/regex_jit_test.cpp
)

if(DEFINED BRASS_ROOT AND EXISTS "${BRASS_ROOT}/include")
    target_include_directories(bronze_regex PUBLIC "${BRASS_ROOT}/include")
endif()
if(TARGET brass)
    target_link_libraries(bronze_regex PUBLIC brass)
endif()

