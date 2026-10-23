#include <doctest/doctest.h>

#include <brass/mir/builder.hpp>
#include <brass/mir/pass_manager.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/mir/verifier.hpp>

#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/il2mir/il_pipeline.h"
#include "codegen-brass/il2mir/il_translator.h"
#include "codegen-brass/il_to_brass_ast.h"
#include "eval/eval.h"
#include "il/il.h"
#include "support/diagnostics.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedEnvVar {
    std::string name;
    std::string old_val;
    bool had_old = false;

    ScopedEnvVar(const char* n, const char* v) : name(n) {
        if (const char* cur = std::getenv(n)) {
            had_old = true;
            old_val = cur;
        }
        set_env_var(n, v);
    }
    ~ScopedEnvVar() {
        set_env_var(name.c_str(), had_old ? old_val.c_str() : nullptr);
    }
};

} // namespace

TEST_CASE("Bronze Optimization Pipeline - Includes smt_guard_elim and mir_superopt (#82)") {
    ScopedEnvVar clear_no_smt("BRONZE_NO_SMT_OPT", nullptr);

    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = true;
    opts.enable_bce = true;
    opts.enable_wbe = true;
    opts.enable_smt_guard_elim = true;
    opts.enable_superopt = true;

    const brass::Pipeline pipe = il2mir::bronze_optimization_pipeline(opts);
    const std::vector<std::string> names = pipe.names();

    auto find_idx = [&](const std::string& target) -> size_t {
        auto it = std::find(names.begin(), names.end(), target);
        REQUIRE(it != names.end());
        return static_cast<size_t>(it - names.begin());
    };

    const size_t bce2_idx = find_idx("bce 2");
    const size_t smt_guard_idx = find_idx("smt_guard_elim");
    const size_t vec_idx = find_idx("loop_vectorize");
    const size_t superopt_idx = find_idx("mir_superopt");
    const size_t wbe_idx = find_idx("wbe");

    // smt_guard_elim runs right after bce 2 (before loop_vectorize), and
    // mir_superopt runs after the loop pipeline right before wbe.
    CHECK(bce2_idx < smt_guard_idx);
    CHECK(smt_guard_idx < vec_idx);
    CHECK(vec_idx < superopt_idx);
    CHECK(superopt_idx < wbe_idx);
}

TEST_CASE("Bronze Optimization Pipeline - Respects BRONZE_NO_SMT_OPT=1 (#82)") {
    ScopedEnvVar disable_smt("BRONZE_NO_SMT_OPT", "1");

    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = true;
    opts.enable_smt_guard_elim = true;
    opts.enable_superopt = true;

    const brass::Pipeline pipe = il2mir::bronze_optimization_pipeline(opts);
    const std::vector<std::string> names = pipe.names();

    CHECK(std::find(names.begin(), names.end(), "smt_guard_elim") == names.end());
    CHECK(std::find(names.begin(), names.end(), "mir_superopt") == names.end());
}

TEST_CASE("Bronze Optimization Pipeline - Collects Superopt & SMT Guard Stats on Lowered IL (#82)") {
    ScopedEnvVar clear_no_smt("BRONZE_NO_SMT_OPT", nullptr);

    // Build an IL function that computes `(((x & 255) | 16) & 65535)` where
    // the outer `& 65535` mask is provably redundant once `x & 255` narrows
    // the bitvector range to [16, 255].
    bronze::il::Module m;
    m.name = "smt_superopt_il_test";

    bronze::il::Function fn;
    fn.name = "mask_chain";
    fn.returnType = bronze::il::Type::I32;
    fn.params = {{"x", bronze::il::Type::I32}};
    fn.valueCount = 8;

    bronze::il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({bronze::il::Op::ConstI32, bronze::il::Type::I32, 1, {}, 0.0, 255, false});
    b0.instructions.push_back({bronze::il::Op::BitAnd, bronze::il::Type::I32, 2, {0, 1}, 0.0, 0, false});
    b0.instructions.push_back({bronze::il::Op::ConstI32, bronze::il::Type::I32, 3, {}, 0.0, 16, false});
    b0.instructions.push_back({bronze::il::Op::BitOr, bronze::il::Type::I32, 4, {2, 3}, 0.0, 0, false});
    b0.instructions.push_back({bronze::il::Op::ConstI32, bronze::il::Type::I32, 5, {}, 0.0, 65535, false});
    b0.instructions.push_back({bronze::il::Op::BitAnd, bronze::il::Type::I32, 6, {4, 5}, 0.0, 0, false});
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::I32, bronze::il::kNoValue, {6}, 0.0, 0, false});

    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));

    std::vector<std::string> uniqueNames = {"mask_chain"};
    il2mir::BronzeModuleAST ast = bronze::codegen::lowerToBrassAst(m, uniqueNames);

    brass::SmtGuardElimStats g_stats;
    brass::SuperoptStats s_stats;

    il2mir::TranslatorOptions opts;
    opts.enable_optimizations = true;
    opts.verify_lowered_module = true;
    opts.smt_guard_stats_collector = &g_stats;
    opts.superopt_stats_collector = &s_stats;

    il2mir::TranslationResult tr = il2mir::translate_bronze_ast(ast, opts);
    REQUIRE(tr.success);
    REQUIRE(tr.module != nullptr);

    CHECK(s_stats.masks_elided >= 1u);
    CHECK(s_stats.solver_queries >= 1u);

    brass::DiagnosticReporter diag;
    CHECK(brass::verify_module(*tr.module, &diag));
}

TEST_CASE("Bronze Tier-2 SMT Optimization Helper - Discharges Armed Guards & Extensions (#82)") {
    ScopedEnvVar clear_no_smt("BRONZE_NO_SMT_OPT", nullptr);

    brass::Module mod("tier2_smt_copy");
    brass::Builder b(mod);
    brass::Function* fn = mod.create_function(
        "strided_kernel", brass::Type::i64(),
        {brass::Type::i64(), brass::Type::i64(), brass::Type::i64()});
    b.set_function(fn);
    brass::BasicBlock* entry = b.append_block("entry");
    brass::Value* x = b.add_block_param(entry, brass::Type::i64());
    brass::Value* base = b.add_block_param(entry, brass::Type::i64());
    brass::Value* len = b.add_block_param(entry, brass::Type::i64());
    b.position_at_end(entry);

    brass::Value* zero = b.build_iconst_i64(0);
    brass::Value* tile = b.build_iconst_i64(8);
    b.build_guard(b.build_sge(base, zero), "bronze.spec");
    b.build_guard(b.build_sge(len, tile), "bronze.spec");
    brass::Value* lim = b.build_sub(len, tile);
    b.build_guard(b.build_sle(base, lim), "bronze.spec");

    // Inner offset k = x & 7 (in [0, 7])
    brass::Value* mask7 = b.build_iconst_i64(7);
    brass::Value* k = b.build_and(x, mask7);
    // Redundant extension sext_i64(trunc_i32(k))
    brass::Value* k32 = b.build_trunc_i32(k);
    brass::Value* k_ext = b.build_sext_i64(k32);

    // Tiled index base + k_ext: provably >= 0 and < len
    brass::Value* idx = b.build_add(base, k_ext);
    b.build_guard(b.build_sge(idx, zero), "bronze.spec");
    b.build_guard(b.build_slt(idx, len), "bronze.spec");
    b.build_ret(idx);
    fn->rebuild_cfg_predecessors();

    brass::SmtGuardElimStats g_stats;
    brass::SuperoptStats s_stats;
    il2mir::TranslatorOptions opts;
    opts.smt_guard_stats_collector = &g_stats;
    opts.superopt_stats_collector = &s_stats;

    CHECK(il2mir::run_smt_optimizations(mod, opts));
    CHECK_EQ(g_stats.guards_proven_eliminated, 2u);
    CHECK_EQ(s_stats.extensions_elided, 1u);

    brass::DiagnosticReporter diag;
    CHECK(brass::verify_module(mod, &diag));
}

TEST_CASE("Bronze All Tiers - Differential Equivalence with SMT Guard Elim & Superopt Enabled (#82)") {
    ScopedEnvVar clear_no_smt("BRONZE_NO_SMT_OPT", nullptr);

    const std::string valid_script = R"JS(
        function bitKernel(n) {
            let acc = 0;
            const table = [3, 7, 11, 19, 23, 31, 43, 59];
            for (let i = 0; i < n; i = (i + 1) | 0) {
                const idx = i & 7;
                const v = table[idx];
                const masked = ((v & 0xff) | 0x10) & 0xffff;
                const deMorgan = (~i & ~idx) ^ (~(i | idx));
                acc = (acc + masked + deMorgan) | 0;
            }
            return acc;
        }
        let total = 0;
        for (let r = 0; r < 60; r++) {
            total += bitKernel(64);
        }
        total;
    )JS";

    auto run_tier = [&](bronze::ExecutionTier tier) -> double {
        bronze::eval::EvalOptions eval_opts;
        eval_opts.filename = "smt_tier_diff.js";
        eval_opts.tier = tier;
        bronze::embed::CallResult res = bronze::eval::evalScript(valid_script, eval_opts);
        REQUIRE(!res.thrown);
        return res.value.asNumber();
    };

    const double out_t0 = run_tier(bronze::ExecutionTier::Tier0_Interpreter);
    const double out_t1 = run_tier(bronze::ExecutionTier::Tier1_Baseline);
    const double out_t2 = run_tier(bronze::ExecutionTier::Tier2_Optimized);
    const double out_auto = run_tier(bronze::ExecutionTier::Auto);

    CHECK_EQ(out_t0, out_t1);
    CHECK_EQ(out_t0, out_t2);
    CHECK_EQ(out_t0, out_auto);
}
