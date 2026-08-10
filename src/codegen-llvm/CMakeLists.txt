find_package(LLVM CONFIG REQUIRED)

llvm_map_components_to_libnames(llvm_libs Core ExecutionEngine Target x86codegen x86asmparser passes)

bronze_add_module(codegen_llvm
    SOURCES
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
