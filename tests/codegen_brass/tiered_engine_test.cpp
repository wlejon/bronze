#include <doctest/doctest.h>
#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "support/diagnostics.h"

using namespace bronze;

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
