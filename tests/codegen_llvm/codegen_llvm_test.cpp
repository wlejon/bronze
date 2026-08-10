#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>

#include "codegen-llvm/llvm_backend.h"
#include "il/il.h"
#include "support/diagnostics.h"

using namespace bronze;

TEST_CASE("LLVM backend metadata") {
    LLVMBackend backend;
    CHECK(std::string(backend.name()) == "llvm");
}

TEST_CASE("LLVM backend emits object file for arithmetic IL module") {
    il::Module module;
    module.name = "test_arithmetic";

    il::Function addFunc;
    addFunc.name = "add";
    addFunc.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    addFunc.returnType = il::Type::F64;
    addFunc.isExported = false;
    addFunc.valueCount = 3;
    addFunc.body = {
        {il::Op::Add, il::Type::F64, 2, {0, 1}, 0.0, 0, 0},
        {il::Op::Ret, il::Type::F64, il::kNoValue, {2}, 0.0, 0, 0}
    };
    module.functions.push_back(addFunc);

    il::Function mainFunc;
    mainFunc.name = "main";
    mainFunc.params = {};
    mainFunc.returnType = il::Type::F64;
    mainFunc.isExported = true;
    mainFunc.valueCount = 8;
    mainFunc.body = {
        {il::Op::ConstF64, il::Type::F64, 0, {}, 10.0, 0, 0},
        {il::Op::ConstF64, il::Type::F64, 1, {}, 20.0, 0, 0},
        {il::Op::Mul, il::Type::F64, 2, {0, 1}, 0.0, 0, 0},
        {il::Op::ConstF64, il::Type::F64, 3, {}, 5.0, 0, 0},
        {il::Op::Sub, il::Type::F64, 4, {2, 3}, 0.0, 0, 0},
        {il::Op::ConstF64, il::Type::F64, 5, {}, 2.0, 0, 0},
        {il::Op::Div, il::Type::F64, 6, {4, 5}, 0.0, 0, 0},
        {il::Op::Call, il::Type::F64, 7, {6, 0}, 0.0, 0, 0},
        {il::Op::Ret, il::Type::F64, il::kNoValue, {7}, 0.0, 0, 0}
    };
    module.functions.push_back(mainFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_arith.obj";
    if (std::filesystem::exists(outPath)) {
        std::filesystem::remove(outPath);
    }

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}

TEST_CASE("LLVM backend emits object file for comparison IL module") {
    il::Module module;
    module.name = "test_compare";

    il::Function cmpFunc;
    cmpFunc.name = "compare";
    cmpFunc.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    cmpFunc.returnType = il::Type::Bool;
    cmpFunc.isExported = true;
    cmpFunc.valueCount = 5;
    cmpFunc.body = {
        {il::Op::CmpLt, il::Type::Bool, 2, {0, 1}, 0.0, 0, 0},
        {il::Op::CmpGt, il::Type::Bool, 3, {0, 1}, 0.0, 0, 0},
        {il::Op::CmpEq, il::Type::Bool, 4, {0, 1}, 0.0, 0, 0},
        {il::Op::Ret, il::Type::Bool, il::kNoValue, {2}, 0.0, 0, 0}
    };
    module.functions.push_back(cmpFunc);

    std::filesystem::path outPath = std::filesystem::temp_directory_path() / "bronze_test_cmp.obj";
    if (std::filesystem::exists(outPath)) {
        std::filesystem::remove(outPath);
    }

    DiagnosticSink diags;
    LLVMBackend backend;
    bool success = backend.emitObject(module, outPath.string(), diags);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(success);
    REQUIRE(std::filesystem::exists(outPath));
    CHECK(std::filesystem::file_size(outPath) > 0);

    std::filesystem::remove(outPath);
}
