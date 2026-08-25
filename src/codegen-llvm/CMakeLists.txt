find_package(LLVM CONFIG REQUIRED)
if(LLVM_VERSION_MAJOR LESS 20)
    message(FATAL_ERROR "Bronze requires LLVM 20 or higher, found LLVM ${LLVM_PACKAGE_VERSION}")
endif()

llvm_map_components_to_libnames(llvm_libs Core ExecutionEngine Target native nativecodegen asmparser passes object bitreader bitwriter transformutils)

bronze_add_module(codegen_llvm
    SOURCES
        llvm_abi.cpp
        llvm_alias.cpp
        llvm_prop_ic.cpp
        llvm_static_slot.cpp
        llvm_prop_get.cpp
        llvm_prop_set.cpp
        llvm_cache.cpp
        llvm_call.cpp
        llvm_method_call.cpp
        llvm_construct.cpp
        llvm_convert.cpp
        llvm_elem.cpp
        llvm_elem_cache.cpp
        llvm_iter.cpp
        llvm_env.cpp
        llvm_frame.cpp
        llvm_func.cpp
        llvm_math.cpp
        llvm_ops.cpp
        llvm_ops_call.cpp
        llvm_private.cpp
        llvm_arith.cpp
        llvm_partition.cpp
        llvm_pin.cpp
        llvm_backend.cpp
    DEPS
        abi
        codegen
        il
        support
    TEST_SOURCES
        ../../tests/codegen_llvm/codegen_llvm_test.cpp
)

target_include_directories(bronze_codegen_llvm SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(bronze_codegen_llvm PUBLIC ${LLVM_DEFINITIONS})
target_link_libraries(bronze_codegen_llvm PUBLIC ${llvm_libs})

# Instantiating templates inside LLVM headers (e.g. Endian.h, bit.h) under
# /W4 /WX trips C4702 (unreachable code) — a code-generation warning that
# neither /external:W0 nor an in-file pragma reaches on MSVC.
if(MSVC)
    target_compile_options(bronze_codegen_llvm PRIVATE /wd4702)
    if(TARGET bronze_codegen_llvm_tests)
        target_compile_options(bronze_codegen_llvm_tests PRIVATE /wd4702)
    endif()
endif()
