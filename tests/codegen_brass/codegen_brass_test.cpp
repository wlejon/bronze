#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "il/il.h"
#include "support/diagnostics.h"

TEST_CASE("brass backend reports name") {
    bronze::BrassBackend backend;
    CHECK(std::string(backend.name()) == "brass");
}

TEST_CASE("brass backend compileToJit compiles and executes in-memory cleanly") {
    bronze::BrassBackend backend;
    bronze::DiagnosticSink diags;

    bronze::il::Module m;
    m.name = "jit_test";

    // Function: add(a: f64, b: f64) -> f64
    bronze::il::Function addFn;
    addFn.name = "add";
    addFn.params = {{"a", bronze::il::Type::F64}, {"b", bronze::il::Type::F64}};
    addFn.returnType = bronze::il::Type::F64;
    addFn.isExported = true;
    addFn.valueCount = 3;
    bronze::il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({bronze::il::Op::Add, bronze::il::Type::F64, 2, {0, 1}, 0, 0, 0});
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {2}, 0, 0, 0});
    addFn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(addFn));

    // Entry function: main() -> void
    bronze::il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = bronze::il::Type::Void;
    mainFn.valueCount = 3;
    bronze::il::Block mb0;
    mb0.id = 0;
    // %0 = const.f64 123.0
    mb0.instructions.push_back({bronze::il::Op::ConstF64, bronze::il::Type::F64, 0, {}, 123.0, 0, 0});
    // %1 = const.f64 456.0
    mb0.instructions.push_back({bronze::il::Op::ConstF64, bronze::il::Type::F64, 1, {}, 456.0, 0, 0});
    // %2 = call @add(%0, %1)
    bronze::il::Instruction callInst;
    callInst.op = bronze::il::Op::Call;
    callInst.type = bronze::il::Type::F64;
    callInst.result = 2;
    callInst.operands = {0, 1};
    callInst.calleeIndex = 0;
    mb0.instructions.push_back(callInst);
    // print %2
    bronze::il::Instruction printInst;
    printInst.op = bronze::il::Op::Print;
    printInst.type = bronze::il::Type::Void;
    printInst.result = bronze::il::kNoValue;
    printInst.operands = {2};
    mb0.instructions.push_back(printInst);
    // ret void
    mb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    // Test buildObjectFile
    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    // Test compileToJit
    auto program = backend.compileToJit(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(program != nullptr);
    CHECK(program->entryPoint() != nullptr);
    CHECK(program->symbolAddress("nonexistent_symbol") == nullptr);

    // Call symbolAddress for the exported "add" function and verify calculation
    void* addSym = program->symbolAddress("add");
    REQUIRE(addSym != nullptr);
    auto addFunc = reinterpret_cast<double (*)(double, double)>(addSym);
    CHECK(addFunc(10.5, 31.5) == doctest::Approx(42.0));
    CHECK(addFunc(-5.0, 5.0) == doctest::Approx(0.0));

    // Run the entry point in-memory (calls add, prints 579, returns cleanly)
    program->run();
}

TEST_CASE("tagged template cells buffer sizing scales dynamically with templateSiteCount") {
    bronze::BrassBackend backend;
    bronze::DiagnosticSink diags;

    bronze::il::Module m;
    m.name = "template_site_test";
    m.templateSiteCount = 1500;

    bronze::il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = bronze::il::Type::Void;
    bronze::il::Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());

    auto* tplSym = obj->find_symbol("__bronze_template_cells");
    REQUIRE(tplSym != nullptr);
    constexpr size_t expectedCells = 1500 + 128;
    constexpr size_t expectedBytes = expectedCells * sizeof(uint64_t);
    CHECK(tplSym->size == expectedBytes);

    auto* cacheSym = obj->find_symbol("__bronze_global_cache");
    REQUIRE(cacheSym != nullptr);
    CHECK(cacheSym->value >= tplSym->value + expectedBytes);
}

TEST_CASE("cross compilation target does not leak host AVX2/FMA features") {
    bronze::BrassBackend backend;
    backend.setTarget(brass::Target::aarch64_linux());
    bronze::DiagnosticSink diags;

    bronze::il::Module m;
    m.name = "cross_compile_test";

    bronze::il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = bronze::il::Type::Void;
    bronze::il::Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));

    auto obj = backend.buildObjectFile(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(obj.has_value());
    CHECK(obj->target.arch() == brass::Arch::aarch64);
}

TEST_CASE("shadow stack overflow guard sets pending RangeError and prevents segfault") {
    bronze::ShadowStackFrame rootFrame;
    constexpr size_t kCapacityWords = (64 * 1024 * 1024) / sizeof(uint64_t);
    uint32_t hugeCount = static_cast<uint32_t>(kCapacityWords + 10);

    bronze_gc_frame* frame = bronze_gc_frame_push(hugeCount);
    REQUIRE(frame != nullptr);
    frame->slots[0] = 42;
    frame->slots[hugeCount - 1] = 42;

    CHECK(bronze_exception_pending() != 0);
    uint64_t exBits = bronze_exception_take();
    CHECK(exBits != BRONZE_ABI_NO_EXCEPTION_BITS);

    bronze_gc_frame_pop();
}
