#include <doctest/doctest.h>
#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "support/diagnostics.h"

#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/tiering.hpp>

using namespace bronze;

namespace {

// An IL module whose `pp_work(a, b)` computes a+b (isMul false) or a*b, and
// whose `main` calls it once.
il::Module makeWorkModule(const std::string& name, bool isMul) {
    il::Module m;
    m.name = name;

    il::Function workFn;
    workFn.name = "pp_work";
    workFn.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
    workFn.returnType = il::Type::F64;
    workFn.isExported = true;
    workFn.valueCount = 3;
    il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({isMul ? il::Op::Mul : il::Op::Add, il::Type::F64, 2, {0, 1}, 0, 0, 0});
    b0.instructions.push_back({il::Op::Ret, il::Type::F64, il::kNoValue, {2}, 0, 0, 0});
    workFn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(workFn));

    il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = il::Type::Void;
    mainFn.valueCount = 3;
    il::Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 0, {}, 6.0, 0, 0});
    mb0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 1, {}, 7.0, 0, 0});
    il::Instruction callInst;
    callInst.op = il::Op::Call;
    callInst.type = il::Type::F64;
    callInst.result = 2;
    callInst.operands = {0, 1};
    callInst.calleeIndex = 0;
    mb0.instructions.push_back(callInst);
    mb0.instructions.push_back({il::Op::Ret, il::Type::Void, il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(mainFn));
    return m;
}

brass::runtime::TieringConfig tier1Config(uint64_t threshold) {
    brass::runtime::TieringConfig cfg;
    cfg.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
    cfg.invocation_tier1_threshold = threshold;
    cfg.invocation_tier2_threshold = 1000000;
    cfg.enable_background_compile = false;
    return cfg;
}

uint64_t invocations(const BrassTieredProgram& prog, std::string_view name) {
    const brass::runtime::TieringFeedback* fb = prog.dispatchTable().tiering().find_feedback(name);
    return fb ? fb->invocation_count() : 0;
}

}  // namespace

TEST_CASE("two live auto-tiered programs keep separate same-named functions and tier-up counts") {
    TieredEngineConfig config;
    config.tier = ExecutionTier::Auto;
    config.entrySymbol = "main";
    BrassTieredEngine engine(config);

    DiagnosticSink diagsA;
    DiagnosticSink diagsB;
    auto progA = engine.compile(makeWorkModule("pp_prog_a", false), diagsA);
    REQUIRE(!diagsA.hasErrors());
    REQUIRE(progA != nullptr);
    // Compiling B must not disturb A (no global clears between programs).
    auto progB = engine.compile(makeWorkModule("pp_prog_b", true), diagsB);
    REQUIRE(!diagsB.hasErrors());
    REQUIRE(progB != nullptr);

    auto& tableA = progA->dispatchTable();
    auto& tableB = progB->dispatchTable();
    CHECK(&tableA != &tableB);
    CHECK(&tableA != &brass::runtime::FunctionDispatchTable::instance());
    CHECK(&tableA.pipeline() != &tableB.pipeline());
    CHECK(&tableA.tiering() != &tableB.tiering());

    // A tiers pp_work up quickly; B effectively never does.
    tableA.pipeline().initialize(tier1Config(3));
    tableB.pipeline().initialize(tier1Config(1000));

    progA->run();
    progB->run();
    // main's call may be inlined, so count B's invokes from here.
    const uint64_t b0 = invocations(*progB, "pp_work");

    const auto a = brass::RuntimeValue::from_f64(6.0);
    const auto b = brass::RuntimeValue::from_f64(7.0);
    for (int i = 0; i < 10; ++i) {
        CHECK(progA->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(13.0));
        if (i < 4) {
            CHECK(progB->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(42.0));
        }
    }

    // Each program counted only its own calls.
    CHECK(invocations(*progA, "pp_work") >= 10);
    CHECK_EQ(invocations(*progB, "pp_work"), b0 + 4);

    // A's pp_work crossed its threshold and runs baseline code; B's did not.
    brass::runtime::FunctionHandle* hA = tableA.find("pp_work");
    brass::runtime::FunctionHandle* hB = tableB.find("pp_work");
    REQUIRE(hA != nullptr);
    REQUIRE(hB != nullptr);
    CHECK(hA != hB);
    CHECK(hA->tier() == brass::runtime::TierLevel::Tier1_Baseline);
    CHECK(!hB->has_native_entry());
    CHECK(tableA.tiering().get_feedback("pp_work").current_tier() == brass::runtime::TierLevel::Tier1_Baseline);
    CHECK(tableB.tiering().get_feedback("pp_work").current_tier() == brass::runtime::TierLevel::Tier0_Interpreter);
    CHECK(tableA.pipeline().find_baseline_compiled("pp_work") != nullptr);
    CHECK(tableB.pipeline().find_baseline_compiled("pp_work") == nullptr);

    // The default program saw none of it.
    CHECK(!brass::runtime::TieringRegistry::instance().has("pp_work"));
    CHECK(!brass::runtime::FunctionDispatchTable::instance().has("pp_work"));

    // Tiered-up A and interpreted B still give their own results, and B's
    // count moves only with B's calls.
    CHECK(progA->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(13.0));
    CHECK(progB->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(42.0));
    CHECK_EQ(invocations(*progB, "pp_work"), b0 + 5);

    // Destroying A leaves B intact.
    progA.reset();
    CHECK(progB->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(42.0));
    CHECK_EQ(invocations(*progB, "pp_work"), b0 + 6);
}

TEST_CASE("an auto-tiered program's hot function reaches optimized code with no caller action") {
    TieredEngineConfig config;
    config.tier = ExecutionTier::Auto;
    config.entrySymbol = "main";
    DiagnosticSink diags;
    auto prog = BrassTieredEngine(config).compile(makeWorkModule("hot_prog", true), diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(prog != nullptr);
    prog->run();

    // Only calls: the program's own thresholds baseline-compile it, then
    // queue it for the background compiler, which installs its code.
    const auto a = brass::RuntimeValue::from_f64(6.0);
    const auto b = brass::RuntimeValue::from_f64(7.0);
    for (int i = 0; i < 2000; ++i) {
        CHECK(prog->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(42.0));
    }
    prog->dispatchTable().pipeline().background_compiler().wait_idle();
    brass::runtime::FunctionHandle* h = prog->dispatchTable().find("pp_work");
    REQUIRE(h != nullptr);
    CHECK(h->tier() == brass::runtime::TierLevel::Tier2_Optimized);
    CHECK(prog->invoke("pp_work", {a, b}).as_f64() == doctest::Approx(42.0));
}

TEST_CASE("execution tier parsing and string conversion") {
    CHECK(parseExecutionTier("0") == ExecutionTier::Tier0_Interpreter);
    CHECK(parseExecutionTier("tier0") == ExecutionTier::Tier0_Interpreter);
    CHECK(parseExecutionTier("interp") == ExecutionTier::Tier0_Interpreter);
    CHECK(parseExecutionTier("interpreter") == ExecutionTier::Tier0_Interpreter);
    CHECK(parseExecutionTier("fast") == ExecutionTier::Tier0_Interpreter);

    CHECK(parseExecutionTier("1") == ExecutionTier::Tier1_Baseline);
    CHECK(parseExecutionTier("tier1") == ExecutionTier::Tier1_Baseline);
    CHECK(parseExecutionTier("baseline") == ExecutionTier::Tier1_Baseline);

    CHECK(parseExecutionTier("2") == ExecutionTier::Tier2_Optimized);
    CHECK(parseExecutionTier("tier2") == ExecutionTier::Tier2_Optimized);
    CHECK(parseExecutionTier("opt") == ExecutionTier::Tier2_Optimized);
    CHECK(parseExecutionTier("optimized") == ExecutionTier::Tier2_Optimized);
    CHECK(parseExecutionTier("jit") == ExecutionTier::Tier2_Optimized);

    CHECK(parseExecutionTier("3") == ExecutionTier::Auto);
    CHECK(parseExecutionTier("auto") == ExecutionTier::Auto);
    CHECK(parseExecutionTier("multi") == ExecutionTier::Auto);
    CHECK(parseExecutionTier("tiered") == ExecutionTier::Auto);

    CHECK(parseExecutionTier("invalid") == std::nullopt);

    CHECK(executionTierToString(ExecutionTier::Tier0_Interpreter) == "interpreter");
    CHECK(executionTierToString(ExecutionTier::Tier1_Baseline) == "baseline");
    CHECK(executionTierToString(ExecutionTier::Tier2_Optimized) == "optimized");
    CHECK(executionTierToString(ExecutionTier::Auto) == "auto");
}

TEST_CASE("tiered engine executes IL arithmetic across all tiers identically") {
    const ExecutionTier tiers[] = {
        ExecutionTier::Tier0_Interpreter,
        ExecutionTier::Tier1_Baseline,
        ExecutionTier::Tier2_Optimized,
        ExecutionTier::Auto
    };

    for (ExecutionTier tier : tiers) {
        il::Module m;
        m.name = "tiered_arith_" + std::string(executionTierToString(tier));

        // add(a: f64, b: f64) -> f64
        il::Function addFn;
        addFn.name = "add";
        addFn.params = {{"a", il::Type::F64}, {"b", il::Type::F64}};
        addFn.returnType = il::Type::F64;
        addFn.isExported = true;
        addFn.valueCount = 3;
        il::Block b0;
        b0.id = 0;
        b0.instructions.push_back({il::Op::Add, il::Type::F64, 2, {0, 1}, 0, 0, 0});
        b0.instructions.push_back({il::Op::Ret, il::Type::F64, il::kNoValue, {2}, 0, 0, 0});
        addFn.blocks.push_back(std::move(b0));
        m.functions.push_back(std::move(addFn));

        // main() -> void
        il::Function mainFn;
        mainFn.name = "main";
        mainFn.isEntryPoint = true;
        mainFn.returnType = il::Type::Void;
        mainFn.valueCount = 3;
        il::Block mb0;
        mb0.id = 0;
        mb0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 0, {}, 20.0, 0, 0});
        mb0.instructions.push_back({il::Op::ConstF64, il::Type::F64, 1, {}, 22.0, 0, 0});
        il::Instruction callInst;
        callInst.op = il::Op::Call;
        callInst.type = il::Type::F64;
        callInst.result = 2;
        callInst.operands = {0, 1};
        callInst.calleeIndex = 0;
        mb0.instructions.push_back(callInst);
        mb0.instructions.push_back({il::Op::Ret, il::Type::Void, il::kNoValue, {}, 0, 0, 0});
        mainFn.blocks.push_back(std::move(mb0));
        m.functions.push_back(std::move(mainFn));

        TieredEngineConfig config;
        config.tier = tier;
        config.entrySymbol = "main";

        BrassTieredEngine engine(config);
        DiagnosticSink diags;
        auto prog = engine.compile(m, diags);
        REQUIRE(!diags.hasErrors());
        REQUIRE(prog != nullptr);

        // Run the entry point cleanly
        prog->run();

        // Also test invoke() on the exported function across all tiers
        auto invRes = prog->invoke("add", {brass::RuntimeValue::from_f64(100.5), brass::RuntimeValue::from_f64(200.25)});
        CHECK(invRes.as_f64() == doctest::Approx(300.75));
    }
}

TEST_CASE("evalScript executes expressions and functions consistently across tiers") {
    embed::setupIo();
    ShadowStackFrame rootFrame;
    eval::installDefaultDynamicHooks();

    const ExecutionTier tiers[] = {
        ExecutionTier::Tier0_Interpreter,
        ExecutionTier::Tier1_Baseline,
        ExecutionTier::Tier2_Optimized,
        ExecutionTier::Auto
    };

    for (ExecutionTier tier : tiers) {
        CAPTURE(executionTierToString(tier));

        // 1. Simple arithmetic
        {
            eval::EvalOptions opts{.filename = "<test>", .tier = tier};
            embed::CallResult res = eval::evalScript("12 * 3 + 6", opts);
            REQUIRE(!res.thrown);
            CHECK(res.value.asNumber() == doctest::Approx(42.0));
        }

        // 2. Loop and accumulator
        {
            eval::EvalOptions opts{.filename = "<test>", .tier = tier};
            embed::CallResult res = eval::evalScript(
                "let sum = 0;\n"
                "for (let i = 1; i <= 10; i++) {\n"
                "    sum += i;\n"
                "}\n"
                "sum;\n", opts);
            REQUIRE(!res.thrown);
            CHECK(res.value.asNumber() == doctest::Approx(55.0));
        }

        // 3. Recursive function
        {
            eval::EvalOptions opts{.filename = "<test>", .tier = tier};
            embed::CallResult res = eval::evalScript(
                "function fact(n) {\n"
                "    if (n <= 1) return 1;\n"
                "    return n * fact(n - 1);\n"
                "}\n"
                "fact(6);\n", opts);
            REQUIRE(!res.thrown);
            CHECK(res.value.asNumber() == doctest::Approx(720.0));
        }

        // 4. Object property manipulation
        {
            eval::EvalOptions opts{.filename = "<test>", .tier = tier};
            embed::CallResult res = eval::evalScript(
                "let obj = { a: 15, b: 27 };\n"
                "obj.a + obj.b;\n", opts);
            REQUIRE(!res.thrown);
            CHECK(res.value.asNumber() == doctest::Approx(42.0));
        }

        // 5. Higher-order function / closure
        {
            eval::EvalOptions opts{.filename = "<test>", .tier = tier};
            embed::CallResult res = eval::evalScript(
                "function makeAdder(x) {\n"
                "    return function(y) { return x + y; };\n"
                "}\n"
                "let add10 = makeAdder(10);\n"
                "add10(32);\n", opts);
            REQUIRE(!res.thrown);
            CHECK(res.value.asNumber() == doctest::Approx(42.0));
        }
    }
}
