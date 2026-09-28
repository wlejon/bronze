#pragma once

#include <brass/mir/module.hpp>
#include <brass/core/diagnostics.hpp>
#include "il_ast.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace brass {
struct DemoteStats;
struct PartialEscapeStats;
struct GvnPreStats;
struct LoopOptStats;
struct FmaOptStats;
struct ParallelLoopStats;
struct RangeAnalysisStats;
} // namespace brass

namespace il2mir {

class SpecFeedback;
class SpecProfile;

struct FunctionMeta {
    bool needs_env = false;
    bool needs_this = false;
    bool needs_arguments = false;
    bool has_rest_param = false;
    bool is_strict = false;
    uint32_t first_source_param = 0;
    uint32_t fn_flags = 0x03; // BRONZE_ABI_FN_FLAGS_ORDINARY (CONSTRUCT | PROTOTYPE)
    uint32_t name_key = 0xFFFFFFFFu; // BRONZE_ABI_FN_NAME_NONE
    uint32_t required_args = 0;
    uint32_t adapt_arity = 0;
    std::vector<bool> params_pinned;
    std::vector<uint32_t> param_pin_keys;
};

struct TranslatorOptions {
    bool enable_optimizations = true;
    // Re-verify the whole module after EVERY optimization pass, naming the
    // pass that broke it. A pass-author's tool: it multiplies the verifier's
    // cost by the number of passes on every compile, which is a large share of
    // an in-memory JIT's turnaround. The module is always verified once
    // before the passes and once after them; this is only the per-pass
    // bisection in between. Defaults on in debug builds, off in release.
#if defined(NDEBUG)
    bool verify_after_each_pass = false;
#else
    bool verify_after_each_pass = true;
#endif
    // Run brass's verifier over the lowered module (before the passes, and
    // again after them when the module is optimized). A check on the
    // translator, not a step of the compile: an in-process JIT load of a large
    // program spends about a quarter of its lowering time in it, so the
    // tiered engine turns it off unless asked (BrassTieredEngine,
    // BRONZE_VERIFY_MIR). An object-file build keeps it.
    bool verify_lowered_module = true;
    // Build each function's body when something first needs it (its first
    // call, or a tier-up that copies it) instead of up front: the module
    // declares every function, builds the entry and the coroutine bodies
    // with every function that starts one (coroutine lowering pairs them
    // once, before anything runs), and leaves the rest lazy, with a body
    // provider (brass Module::set_body_provider) that owns the lowering.
    // Only an unoptimized module for brass's tiered pipeline, whose tiers
    // build a body on demand; see translate_bronze_ast_lazy.
    bool lazy_bodies = false;
    bool allow_fp_reassociation = false;
    bool trace_lowering = false;
    bool enable_f64_demote = true;
    bool demote_stats = false;
    bool enable_inlining = false;
    bool inline_leaf_only = true;
    bool enable_sroa = false;
    bool enable_gvn = true;
    bool enable_sccp = true;
    bool enable_guard_elim = true;
    bool enable_cfg_simplify = true;
    bool enable_loop_unswitch = true;
    bool enable_jump_threading = true;
    bool enable_trace_layout = true;
    bool run_escape_analysis = false;
    bool enable_partial_escape = false;
    bool enable_allocation_sinking = false;
    bool dump_pea_stats = false;
    bool run_alias_analysis = false;
    bool enable_vectorize = true;
    bool enable_slp = true;
    bool enable_loop_tile = true;
    size_t tile_size = 16;
    bool enable_inlined_fastpaths = true;
    bool enable_tlab = false;
    bool dump_ic_stats = false;
    bool enable_wbe = true;
    bool dump_wbe_stats = false;
    bool enable_gvn_pre = true;
    bool dump_pre_stats = false;
    bool enable_osr = false;
    uint64_t osr_threshold = 100;
    bool dump_tiering_stats = false;
    bool enable_loop_fusion = false;
    bool enable_loop_distribution = false;
    bool enable_array_contraction = false;
    bool dump_loop_transform_stats = false;
    DemoteStats* demote_stats_collector = nullptr;
    PartialEscapeStats* pea_stats_collector = nullptr;
    GvnPreStats* pre_stats_collector = nullptr;
    LoopOptStats* loop_transform_stats_collector = nullptr;
    bool enable_avx2 = false;
    bool enable_fma = false;
    uint32_t vector_width = 0;
    bool dump_fma_stats = false;
    FmaOptStats* fma_stats_collector = nullptr;
    bool enable_parallel_loops = false;
    uint64_t parallel_threshold = 1000;
    uint32_t parallel_workers = 0;
    bool dump_parallel_stats = false;
    ParallelLoopStats* parallel_stats_collector = nullptr;
    bool enable_bce = true;
    bool dump_range_stats = false;
    RangeAnalysisStats* range_stats_collector = nullptr;
    bool enable_speculative_inlining = false;
    bool dump_tfv_stats = false;
    std::vector<std::string> key_constants;
    std::unordered_map<std::string, FunctionMeta> function_meta;
    std::string entry_symbol;
    // Keep the runtime's thread-local block in a pinned callee-saved register
    // (Module::set_pinned_tls_register): the module entry fetches it once
    // through `bronze_tls_enter`, and every allocation fast path and
    // stack-limit check reads through the register instead of
    // calling `bronze_tls_block_addr`. Requires the runtime to enter compiled
    // code only through its trampoline (which sets the register).
    bool pin_tls_register = false;
    bool enable_census = false;
    uint32_t census_site_count = 0;
    // The module's inline-cache table (bronze_abi.h, "the inline property
    // cache contract"): `ic_site_count` sites of BRONZE_ABI_IC_SITE_SIZE
    // bytes each, laid out by the embedder as the module data symbol
    // `__bronze_ic_table` (module-suffixed). A property read, a property
    // write and a method call whose IL carries an ic index below the count
    // pass the address of THEIR site's way 0 to the runtime helper; zero
    // sites (the default, and every standalone brass test) keeps every site
    // pointer null, which the helpers accept as "cache nothing".
    // `method_ic_sites` is the subset of indexes that belong to METHOD-CALL
    // sites, whose env words the module must hand to
    // `bronze_register_method_ic_cells` at init; the embedder emits them as
    // the u64 array `__bronze_method_ic_sites` in that order.
    uint32_t ic_site_count = 0;
    std::vector<uint32_t> method_ic_sites;
    uint32_t template_site_count = 0;
    // Address the module's WRITABLE tables — `__bronze_module_env`,
    // `__bronze_template_cells`, `__bronze_ic_table`, and the embedder's own
    // (global cache, native imports) — per THREAD rather than per image, so
    // one image can run on several threads, each with its own heap. The
    // embedder lays those tables out as one contiguous run of the module's
    // data between `__bronze_instance` and `__bronze_instance_end`, with a
    // u64 slot cell `__bronze_module_slot` outside it (all module-suffixed).
    // The entry calls `bronze_module_instance(slot, begin, end)` first and
    // gets the calling thread's delta back; every other function loads its
    // delta from the thread's array (kBronzeTlsModuleDeltasOff) at the slot
    // the cell names. A table's address is then `symbol + delta`. Off (the
    // default, and every standalone brass test) keeps the plain addresses.
    bool per_thread_module_data = false;
    // The program's speculation feedback (il_speculation.h), for a module
    // that runs in-process under brass's tiered pipeline: its inline fast
    // paths count their misses there and carry the guards tier 2 arms. Null
    // (AOT objects, whole-program tier 2) lowers them as plain branches.
    std::shared_ptr<SpecFeedback> spec_feedback;
    // An AOT lowering's speculation profile (il_spec_profile.h), written by
    // a tiered run of the same program: every keyed property site inline,
    // each site's slow path laid out as its counts say, and every slow path
    // kept as a branch. Ignored with spec_feedback.
    std::shared_ptr<const SpecProfile> spec_profile;
    struct SourceFileMeta {
        uint32_t text_len = 0;
        uint32_t entry_count = 0;
    };
    std::vector<SourceFileMeta> source_files;
};

struct TranslationResult {
    bool success = false;
    std::unique_ptr<Module> module;
    std::string error_message;
};

// Translate the flattened bronze IL module to a brass MIR module.
TranslationResult translate_bronze_ast(
    const BronzeModuleAST& ast,
    const TranslatorOptions& options,
    DiagnosticReporter* diag = nullptr
);

// translate_bronze_ast for options.lazy_bodies: the module's body provider
// keeps `ast` and the lowering alive for as long as the module lives, and a
// body that fails to build reports its diagnostics on stderr (brass then
// stops the program: the function has nothing to run). Without
// lazy_bodies, the same as translate_bronze_ast.
TranslationResult translate_bronze_ast_lazy(
    BronzeModuleAST&& ast,
    const TranslatorOptions& options,
    DiagnosticReporter* diag = nullptr
);

} // namespace il2mir
