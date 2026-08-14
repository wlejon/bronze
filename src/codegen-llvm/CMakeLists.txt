find_package(LLVM CONFIG REQUIRED)

llvm_map_components_to_libnames(llvm_libs Core ExecutionEngine Target x86codegen x86asmparser passes object)

bronze_add_module(codegen_llvm
    SOURCES
        llvm_abi.cpp
        llvm_prop.cpp
        llvm_elem.cpp
        llvm_env.cpp
        llvm_func.cpp
        llvm_ops.cpp
        llvm_arith.cpp
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

# The test TU parses the emitted COFF back with llvm::object to pin symbol
# linkage. Instantiating those templates under /W4 /WX trips C4702
# (unreachable code) inside LLVM's own Endian.h — a code-generation warning
# that neither /external:W0 nor an in-file pragma reaches on MSVC.
if(MSVC)
    set_source_files_properties(../../tests/codegen_llvm/codegen_llvm_test.cpp
                                PROPERTIES COMPILE_OPTIONS /wd4702)
endif()
