#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/il2mir/il_translator.h"
#include "codegen-brass/il_to_brass_ast.h"
#include "il/il.h"
#include <brass/mir/opcodes.hpp>
#include <brass/mir/verifier.hpp>
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

// `layout.epoch` (il_op.h `LayoutEpoch`, docs/aot-specialization.md "S1"): one
// load of the TLS epoch word and an exact conversion, no runtime call, and two
// reads with a call between them stay two reads after the optimizer, since the
// whole point of the second is to see what the call did.
TEST_CASE("layout.epoch lowers to a TLS load that a call between two reads does not merge") {
    bronze::il::Module m;
    m.name = "layout_epoch_test";
    bronze::il::Function fn;
    fn.name = "epoch_still";
    fn.returnType = bronze::il::Type::Bool;
    fn.valueCount = 4;
    bronze::il::Block b0;
    b0.id = 0;
    // %0 = layout.epoch; %1 = math.random; %2 = layout.epoch; %3 = cmp.eq %0, %2; ret %3
    b0.instructions.push_back({bronze::il::Op::LayoutEpoch, bronze::il::Type::F64, 0, {}, 0, 0, 0});
    b0.instructions.push_back({bronze::il::Op::MathRandom, bronze::il::Type::F64, 1, {}, 0, 0, 0});
    b0.instructions.push_back({bronze::il::Op::LayoutEpoch, bronze::il::Type::F64, 2, {}, 0, 0, 0});
    b0.instructions.push_back({bronze::il::Op::CmpEq, bronze::il::Type::Bool, 3, {0, 2}, 0, 0, 0});
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Bool, bronze::il::kNoValue, {3}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    std::vector<std::string> names = {"epoch_still"};
    il2mir::BronzeModuleAST ast = bronze::codegen::lowerToBrassAst(m, names);
    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = true;
    opts.verify_lowered_module = true;
    il2mir::TranslationResult tr = il2mir::translate_bronze_ast(ast, opts);
    REQUIRE(tr.success);
    brass::Function* mirFn = tr.module->get_function("epoch_still");
    REQUIRE(mirFn != nullptr);
    CHECK(brass::verify_function(*mirFn));

    int conversions = 0;
    int runtimeEpochCalls = 0;
    for (brass::BasicBlock* bb : mirFn->blocks()) {
        for (brass::Instruction* inst : *bb) {
            if (inst->opcode() == brass::Opcode::sitofp_f64_i64) ++conversions;
            if (inst->opcode() == brass::Opcode::call && inst->symbol().find("epoch") != std::string::npos) {
                ++runtimeEpochCalls;
            }
        }
    }
    CHECK(conversions == 2);
    CHECK(runtimeEpochCalls == 0);
}

// docs/aot-specialization.md "S4": in an image build a read through a site's
// cache calls the module's read stub, whose hit paths call nothing; its one
// call is the full stub (__bronze_prop_get_slow), from a block whose every
// path returns that call's answer.
TEST_CASE("a read site's stub answers hits without a call and hands the rest to the full stub") {
    bronze::il::Module m;
    m.name = "ic_fast_test";
    m.keyConstants = {"x"};
    m.icSiteCount = 1;
    bronze::il::Function fn;
    fn.name = "read_x";
    fn.params = {{"o", bronze::il::Type::Dynamic}};
    fn.returnType = bronze::il::Type::Dynamic;
    fn.valueCount = 2;
    bronze::il::Block b0;
    b0.id = 0;
    bronze::il::Instruction get;
    get.op = bronze::il::Op::PropGet;
    get.type = bronze::il::Type::Dynamic;
    get.result = 1;
    get.operands = {0};
    get.keyIndex = 0;
    get.icIndex = 0;
    b0.instructions.push_back(get);
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Dynamic, bronze::il::kNoValue, {1}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    std::vector<std::string> names = {"read_x"};
    il2mir::BronzeModuleAST ast = bronze::codegen::lowerToBrassAst(m, names);
    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = false;
    opts.verify_lowered_module = true;
    opts.direct_js_calls = true;
    opts.pin_tls_register = true;
    opts.ic_site_count = 1;
    il2mir::TranslationResult tr = il2mir::translate_bronze_ast(ast, opts);
    REQUIRE(tr.success);
    brass::Function* stub = tr.module->get_function("__bronze_prop_get_ic");
    REQUIRE(stub != nullptr);
    REQUIRE(tr.module->get_function("__bronze_prop_get_slow") != nullptr);
    CHECK(brass::verify_function(*stub));
    int calls = 0;
    for (brass::BasicBlock* bb : stub->blocks()) {
        for (brass::Instruction* inst : *bb) {
            if (!brass::is_call(inst->opcode())) continue;
            ++calls;
            CHECK(inst->opcode() == brass::Opcode::call);
            CHECK(inst->symbol() == "__bronze_prop_get_slow");
        }
    }
    CHECK(calls == 1);

    brass::Function* reader = tr.module->get_function("read_x");
    REQUIRE(reader != nullptr);
    int stub_calls = 0;
    for (brass::BasicBlock* bb : reader->blocks()) {
        for (brass::Instruction* inst : *bb) {
            if (inst->opcode() != brass::Opcode::call) continue;
            if (inst->symbol() == "__bronze_prop_get_ic") ++stub_calls;
            CHECK(inst->symbol() != "__bronze_prop_get_slow");
        }
    }
    CHECK(stub_calls == 1);
}

// `layout.fact` (il_op.h `LayoutFact`, il_static_slot.cpp `layout_fact_of` /
// `layout_fact_word`): the fact is the epoch word's bits in an f64 and a site
// compares those bits, so neither making one nor asking one converts between
// integer and double.
TEST_CASE("a layout fact carries the epoch word's bits and no site converts it") {
    bronze::il::Module m;
    m.name = "layout_fact_test";
    m.keyConstants = {"x"};
    m.icSiteCount = 1;
    m.staticSiteCount = 1;
    bronze::il::Function fn;
    fn.name = "read_x";
    fn.params = {{"o", bronze::il::Type::Dynamic}};
    fn.returnType = bronze::il::Type::Dynamic;
    fn.valueCount = 3;
    bronze::il::Block b0;
    b0.id = 0;
    bronze::il::Instruction fact;
    fact.op = bronze::il::Op::LayoutFact;
    fact.type = bronze::il::Type::F64;
    fact.result = 1;
    fact.operands = {0};
    fact.staticSlot = 0;
    fact.staticCellIndex = 0;
    fact.icIndex = 0;
    bronze::il::Instruction get;
    get.op = bronze::il::Op::PropGet;
    get.type = bronze::il::Type::Dynamic;
    get.result = 2;
    get.operands = {0, 1};
    get.keyIndex = 0;
    get.icIndex = 0;
    get.staticSlot = 0;
    get.staticCellIndex = 0;
    get.shapeGuarded = true;
    b0.instructions.push_back(fact);
    b0.instructions.push_back(get);
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Dynamic, bronze::il::kNoValue, {2}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    std::vector<std::string> names = {"read_x"};
    il2mir::BronzeModuleAST ast = bronze::codegen::lowerToBrassAst(m, names);
    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = false;
    opts.verify_lowered_module = true;
    opts.direct_js_calls = true;
    opts.pin_tls_register = true;
    opts.ic_site_count = 1;
    opts.static_site_count = 1;
    il2mir::TranslationResult tr = il2mir::translate_bronze_ast(ast, opts);
    REQUIRE(tr.success);
    brass::Function* reader = tr.module->get_function("read_x");
    REQUIRE(reader != nullptr);
    CHECK(brass::verify_function(*reader));
    int conversions = 0;
    int toFact = 0;
    int toWord = 0;
    for (brass::BasicBlock* bb : reader->blocks()) {
        for (brass::Instruction* inst : *bb) {
            const brass::Opcode op = inst->opcode();
            if (op == brass::Opcode::sitofp_f64_i64 || op == brass::Opcode::fptosi_i64) ++conversions;
            if (op == brass::Opcode::bitcast_f64_i64) ++toFact;
            if (op == brass::Opcode::bitcast_i64_f64) ++toWord;
        }
    }
    CHECK(conversions == 0);
    CHECK(toFact >= 1);
    CHECK(toWord >= 1);
}

// An element read's receiver (il_abi.h `object_address`): its address is the
// bits xor the object tag, the object test is that address below 2^48, and
// nothing masks the receiver's bits, so brass's untag_reuse can hand the
// address to the next access.
TEST_CASE("an element read untags its receiver by xor and tests the address") {
    bronze::il::Module m;
    m.name = "elem_untag_test";
    bronze::il::Function fn;
    fn.name = "read_elem";
    fn.params = {{"a", bronze::il::Type::Dynamic}, {"i", bronze::il::Type::Dynamic}};
    fn.returnType = bronze::il::Type::Dynamic;
    fn.valueCount = 3;
    bronze::il::Block b0;
    b0.id = 0;
    bronze::il::Instruction get;
    get.op = bronze::il::Op::ElemGet;
    get.type = bronze::il::Type::Dynamic;
    get.result = 2;
    get.operands = {0, 1};
    b0.instructions.push_back(get);
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Dynamic, bronze::il::kNoValue, {2}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    std::vector<std::string> names = {"read_elem"};
    il2mir::BronzeModuleAST ast = bronze::codegen::lowerToBrassAst(m, names);
    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = false;
    opts.verify_lowered_module = true;
    opts.direct_js_calls = true;
    opts.pin_tls_register = true;
    il2mir::TranslationResult tr = il2mir::translate_bronze_ast(ast, opts);
    REQUIRE(tr.success);
    brass::Function* reader = tr.module->get_function("read_elem");
    REQUIRE(reader != nullptr);
    CHECK(brass::verify_function(*reader));

    const brass::Value* receiver = reader->entry_block()->param(0);
    const auto is_receiver_bits = [&](const brass::Value* v) {
        const brass::Instruction* d = v && v->is_instruction() ? v->defining_instruction() : nullptr;
        return v == receiver ||
               (d && d->opcode() == brass::Opcode::bitcast_i64_tagged && d->operand(0) == receiver);
    };
    const uint64_t objectTag = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
    int masked = 0;
    int untagged = 0;
    int tested = 0;
    for (brass::BasicBlock* bb : reader->blocks()) {
        for (brass::Instruction* inst : *bb) {
            if (inst->operand_count() != 2) continue;
            const brass::Instruction* k = inst->operand(1)->is_instruction() ? inst->operand(1)->defining_instruction() : nullptr;
            const bool constant = k && k->opcode() == brass::Opcode::iconst_i64;
            if (inst->opcode() == brass::Opcode::and_ && is_receiver_bits(inst->operand(0))) ++masked;
            if (inst->opcode() == brass::Opcode::xor_ && is_receiver_bits(inst->operand(0)) && constant &&
                static_cast<uint64_t>(k->imm_i64()) == objectTag) {
                ++untagged;
            }
            if (inst->opcode() == brass::Opcode::ult && constant && k->imm_i64() == (int64_t{1} << 48)) {
                const brass::Instruction* x = inst->operand(0)->is_instruction() ? inst->operand(0)->defining_instruction() : nullptr;
                if (x && x->opcode() == brass::Opcode::xor_ && is_receiver_bits(x->operand(0))) ++tested;
            }
        }
    }
    CHECK(masked == 0);
    CHECK(untagged >= 1);
    CHECK(tested >= 1);
}
