#include "il_pipeline.h"

#include <brass/mir/dominators.hpp>
#include <brass/mir/pass_catalog.hpp>
#include <brass/mir/proof_cert.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace il2mir {

namespace {

constexpr size_t kMaxSmtOptBlocksPerFunction = 32;

bool smt_opt_disabled_by_env() noexcept {
    const char* v = std::getenv("BRONZE_NO_SMT_OPT");
    return v && std::string_view(v) == "1";
}

bool smt_opt_stats_enabled_by_env() noexcept {
    const char* v = std::getenv("BRONZE_SMT_OPT_STATS");
    return v && std::string_view(v) == "1";
}

void reconcile_guard_certs(brass::Function& fn) {
    if (fn.guard_elim_certs().empty()) return;

    struct ValLoc {
        const brass::Value* val = nullptr;
        const brass::BasicBlock* bb = nullptr;
    };
    std::unordered_map<uint32_t, ValLoc> live_vals;
    for (const brass::BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (size_t i = 0; i < bb->param_count(); ++i) {
            if (const brass::Value* p = bb->param(i)) {
                live_vals[p->id()] = {p, bb};
            }
        }
        for (const brass::Instruction* inst : *bb) {
            if (inst && inst->result()) {
                live_vals[inst->result()->id()] = {inst->result(), bb};
            }
        }
    }

    const bool has_entry = fn.entry_block() != nullptr;
    std::unique_ptr<brass::DominatorTree> dom;
    if (has_entry) {
        dom = std::make_unique<brass::DominatorTree>(fn);
    }

    auto& certs = fn.guard_elim_certs();
    certs.erase(
        std::remove_if(certs.begin(), certs.end(), [&](brass::GuardElimCert& c) {
            const brass::BasicBlock* bb = fn.get_block_by_id(c.block_id);
            if (!bb) return true;
            for (const brass::Instruction* inst : *bb) {
                if (inst && inst->opcode() == brass::Opcode::guard &&
                    inst->resume_id() == c.eliminated_resume_id) {
                    return true;
                }
            }
            if (c.witness_inst_or_val_id != 0) {
                auto it = live_vals.find(c.witness_inst_or_val_id);
                const bool valid_witness =
                    it != live_vals.end() && dom && dom->dominates(it->second.bb, bb);
                if (!valid_witness) {
                    c.witness_inst_or_val_id = 0;
                    if (c.reason == brass::GuardElimCert::Reason::DominatedSameCond ||
                        c.reason == brass::GuardElimCert::Reason::NonNegativeMask) {
                        c.reason = brass::GuardElimCert::Reason::ImpliedRelational;
                    }
                }
            }
            return false;
        }),
        certs.end());
}

void dump_smt_opt_stats(
    std::string_view mod_name,
    const brass::SmtGuardElimStats& g_stats,
    const brass::SuperoptStats& s_stats) {
    std::fprintf(
        stderr,
        "[smt-opt] module=%.*s guards_elim=%zu cmp_true=%zu cmp_false=%zu "
        "branches_folded=%zu masks_elided=%u ext_elided=%u dags_synth=%u "
        "const_synth=%u queries=%zu\n",
        static_cast<int>(mod_name.size()),
        mod_name.data(),
        g_stats.guards_proven_eliminated,
        g_stats.comparisons_proven_true,
        g_stats.comparisons_proven_false,
        g_stats.branches_folded,
        s_stats.masks_elided,
        s_stats.extensions_elided,
        s_stats.dags_synthesized,
        s_stats.constants_synthesized,
        g_stats.queries_solved + static_cast<size_t>(s_stats.solver_queries));
}

} // namespace

PassPipelineOptions pass_pipeline_options(const TranslatorOptions& options) {
    PassPipelineOptions p;
    p.enable_sroa = options.enable_sroa;
    p.enable_inlining = options.enable_inlining;
    p.enable_speculative_inlining = options.enable_speculative_inlining;
    p.inliner.only_inline_leaf_functions = options.inline_leaf_only;
    p.enable_gvn = options.enable_gvn;
    p.enable_gvn_pre = options.enable_gvn_pre;
    p.enable_sccp = options.enable_sccp;
    p.enable_guard_elim = options.enable_guard_elim;
    p.enable_cfg_simplify = options.enable_cfg_simplify;
    p.enable_loop_unswitch = options.enable_loop_unswitch;
    p.enable_jump_threading = options.enable_jump_threading;
    p.enable_bce = options.enable_bce;
    p.enable_wbe = options.enable_wbe;
    p.dump_wbe_stats = options.dump_wbe_stats;
    p.dump_range_stats = options.dump_range_stats;
    p.pre_stats = options.pre_stats_collector;
    p.range_stats = options.range_stats_collector;

    LoopOptOptions& l = p.loop;
    l.deterministic_fp = options.deterministic_fp;
    l.enable_fp_reassociation = options.allow_fp_reassociation && !options.deterministic_fp;
    l.enable_f64_demote = options.enable_f64_demote;
    l.enable_vectorize = options.enable_vectorize;
    l.enable_slp = options.enable_slp;
    l.enable_loop_tile = options.enable_loop_tile;
    l.tile_size_i = options.tile_size;
    l.tile_size_j = options.tile_size;
    l.enable_sroa = options.enable_sroa;
    l.enable_gvn = options.enable_gvn;
    l.enable_sccp = options.enable_sccp;
    l.enable_guard_elim = options.enable_guard_elim;
    l.enable_cfg_simplify = options.enable_cfg_simplify;
    l.enable_loop_unswitch = options.enable_loop_unswitch;
    l.enable_jump_threading = options.enable_jump_threading;
    l.enable_trace_layout = options.enable_trace_layout;
    l.enable_partial_escape = options.enable_partial_escape;
    l.enable_allocation_sinking = options.enable_allocation_sinking;
    l.enable_loop_fusion = options.enable_loop_fusion;
    l.enable_loop_distribution = options.enable_loop_distribution;
    l.enable_array_contraction = options.enable_array_contraction;
    l.dump_loop_transform_stats = options.dump_loop_transform_stats;
    l.stats = options.loop_transform_stats_collector;
    l.pea_stats = options.pea_stats_collector;
    l.demote_stats = options.demote_stats_collector;
    l.enable_avx2 = options.enable_avx2;
    l.enable_fma = options.enable_fma && !options.deterministic_fp;
    l.vector_width = options.vector_width;
    l.dump_fma_stats = options.dump_fma_stats;
    l.fma_stats = options.fma_stats_collector;
    l.enable_parallel_loops = options.enable_parallel_loops;
    l.parallel_threshold = options.parallel_threshold;
    l.parallel_workers = options.parallel_workers;
    l.dump_parallel_stats = options.dump_parallel_stats;
    l.parallel_stats = options.parallel_stats_collector;
    l.enable_bce = options.enable_bce;
    l.dump_range_stats = options.dump_range_stats;
    l.range_stats = options.range_stats_collector;
    return p;
}

brass::Pipeline bronze_optimization_pipeline(const TranslatorOptions& options) {
    PassPipelineOptions base_opts = pass_pipeline_options(options);
    const bool enable_wbe = base_opts.enable_wbe;
    const bool dump_wbe = base_opts.dump_wbe_stats;
    base_opts.enable_wbe = false;

    brass::Pipeline base_pipeline = brass::pass_pipeline(base_opts);
    const bool allow_smt = !smt_opt_disabled_by_env();

    auto make_smt_guard_step = [&]() {
        brass::SmtGuardElimStats* stats_ptr = options.smt_guard_stats_collector;
        brass::PassStep step;
        step.name = "smt_guard_elim";
        step.scope = brass::PassScope::Function;
        step.filter = brass::FunctionFilter::LoopEligible;
        step.run_function = [stats_ptr](brass::Function& fn) -> bool {
            if (!fn.parent() || fn.blocks().size() > kMaxSmtOptBlocksPerFunction) {
                return false;
            }
            brass::SmtGuardElimOptions opts;
            opts.max_sat_conflicts = 400;
            opts.max_slice_depth = 8;
            opts.stats = stats_ptr;
            const bool changed = brass::run_smt_guard_elimination(fn, *fn.parent(), opts);
            if (changed) reconcile_guard_certs(fn);
            return changed;
        };
        return step;
    };

    brass::Pipeline p;
    p.set_marks_loop_optimized(base_pipeline.marks_loop_optimized());
    bool added_smt_guard = false;
    for (const brass::PassStep& s : base_pipeline.steps()) {
        p.add(s);
        if (allow_smt && options.enable_smt_guard_elim && !added_smt_guard && s.name == "bce 2") {
            p.add(make_smt_guard_step());
            added_smt_guard = true;
        }
    }

    if (allow_smt) {
        if (options.enable_smt_guard_elim && !added_smt_guard) {
            p.add(make_smt_guard_step());
        }

        if (options.enable_superopt) {
            brass::SuperoptStats* stats_ptr = options.superopt_stats_collector;
            brass::PassStep step;
            step.name = "mir_superopt";
            step.scope = brass::PassScope::Function;
            step.filter = brass::FunctionFilter::LoopEligible;
            step.run_function = [stats_ptr](brass::Function& fn) -> bool {
                if (!fn.parent() || fn.blocks().size() > kMaxSmtOptBlocksPerFunction) {
                    return false;
                }
                brass::SuperoptOptions opts;
                opts.max_window_depth = 3;
                opts.max_sat_conflicts = 300;
                opts.stats = stats_ptr;
                const bool changed = brass::run_mir_superopt(fn, *fn.parent(), opts);
                if (changed) reconcile_guard_certs(fn);
                return changed;
            };
            p.add(std::move(step));
        }
    }

    if (enable_wbe) {
        p.add(brass::passes::write_barrier_elim(dump_wbe));
    }
    return p;
}

bool run_bronze_optimization_pipeline(
    brass::Module& mod,
    const TranslatorOptions& options,
    const brass::PassPipelineHooks& hooks) {

    const bool dump_stats = options.dump_smt_opt_stats || smt_opt_stats_enabled_by_env();
    brass::SmtGuardElimStats local_g_stats;
    brass::SuperoptStats local_s_stats;

    TranslatorOptions effective = options;
    if (dump_stats) {
        if (!effective.smt_guard_stats_collector) effective.smt_guard_stats_collector = &local_g_stats;
        if (!effective.superopt_stats_collector) effective.superopt_stats_collector = &local_s_stats;
    }

    const bool completed = brass::run_pipeline(mod, bronze_optimization_pipeline(effective), hooks).completed;
    if (completed && dump_stats && !smt_opt_disabled_by_env()) {
        dump_smt_opt_stats(
            mod.name(),
            *effective.smt_guard_stats_collector,
            *effective.superopt_stats_collector);
    }
    return completed;
}

bool run_smt_optimizations(
    brass::Module& mod,
    const TranslatorOptions& options) {

    if (smt_opt_disabled_by_env()) return false;
    if (!options.enable_smt_guard_elim && !options.enable_superopt) return false;

    const bool dump_stats = options.dump_smt_opt_stats || smt_opt_stats_enabled_by_env();
    brass::SmtGuardElimStats local_g_stats;
    brass::SuperoptStats local_s_stats;
    brass::SmtGuardElimStats* g_ptr =
        options.smt_guard_stats_collector ? options.smt_guard_stats_collector
                                          : (dump_stats ? &local_g_stats : nullptr);
    brass::SuperoptStats* s_ptr =
        options.superopt_stats_collector ? options.superopt_stats_collector
                                         : (dump_stats ? &local_s_stats : nullptr);

    bool any_changed = false;
    for (brass::Function* fn : mod.functions()) {
        if (!fn || !brass::loop_eligible(*fn) || fn->blocks().size() > kMaxSmtOptBlocksPerFunction) {
            continue;
        }
        bool fn_changed = false;
        if (options.enable_smt_guard_elim) {
            brass::SmtGuardElimOptions g_opts;
            g_opts.max_sat_conflicts = 400;
            g_opts.max_slice_depth = 8;
            g_opts.stats = g_ptr;
            fn_changed |= brass::run_smt_guard_elimination(*fn, mod, g_opts);
        }
        if (options.enable_superopt) {
            brass::SuperoptOptions s_opts;
            s_opts.max_window_depth = 3;
            s_opts.max_sat_conflicts = 300;
            s_opts.stats = s_ptr;
            fn_changed |= brass::run_mir_superopt(*fn, mod, s_opts);
        }
        if (fn_changed) {
            reconcile_guard_certs(*fn);
            any_changed = true;
        }
    }

    if (dump_stats && g_ptr && s_ptr) {
        dump_smt_opt_stats(mod.name(), *g_ptr, *s_ptr);
    }
    return any_changed;
}

} // namespace il2mir
