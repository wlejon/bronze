#pragma once

#include <brass/mir/module.hpp>
#include <brass/core/diagnostics.hpp>
#include "il_ast.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

// The ARMED bitmap of an image whose code counts its slow sites (one bit per
// site lowering gave a counter: TranslatorOptions::slow_armed), laid out
// before the counters, where a harvest reads it. Lowering may run on several
// threads, and for an image built in process with lazy bodies after the
// image is loaded: `live`, once set, is that image's own copy of the words,
// which every later mark sets too.
struct SlowArmedBits {
    explicit SlowArmedBits(size_t words) : words(words, 0) {}
    std::vector<uint64_t> words;
    std::atomic<uint64_t*> live{nullptr};
    void mark(uint32_t index) {
        if (index / 64 >= words.size()) return;
        const uint64_t bit = uint64_t{1} << (index % 64);
        std::atomic_ref<uint64_t>(words[index / 64]).fetch_or(bit, std::memory_order_relaxed);
        if (uint64_t* image = live.load(std::memory_order_acquire)) {
            std::atomic_ref<uint64_t>(image[index / 64]).fetch_or(bit, std::memory_order_relaxed);
        }
    }
    // The image's words from now on: what was marked so far is copied in.
    void attach(uint64_t* image) {
        live.store(image, std::memory_order_release);
        for (size_t w = 0; w < words.size(); ++w) {
            const uint64_t bits = std::atomic_ref<uint64_t>(words[w]).load(std::memory_order_relaxed);
            if (bits != 0) std::atomic_ref<uint64_t>(image[w]).fetch_or(bits, std::memory_order_relaxed);
        }
    }
};

struct FunctionMeta {
    bool needs_env = false;
    bool needs_this = false;
    bool needs_arguments = false;
    bool arguments_is_list = false;
    bool arguments_is_count = false;
    bool has_rest_param = false;
    bool is_strict = false;
    uint32_t first_source_param = 0;
    uint32_t fn_flags = 0x03; // BRONZE_ABI_FN_FLAGS_ORDINARY (CONSTRUCT | PROTOTYPE)
    uint32_t name_key = 0xFFFFFFFFu; // BRONZE_ABI_FN_NAME_NONE
    uint32_t required_args = 0;
    uint32_t adapt_arity = 0;
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
    // Threads brass's optimization pipeline may use for its function-local
    // passes (brass::PipelineRunOptions::threads): 0 is the machine's
    // hardware threads, 1 the calling thread alone. The module it leaves is
    // the same at any count.
    size_t build_threads = 0;
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
    bool enable_smt_guard_elim = false;
    bool enable_superopt = false;
    uint32_t superopt_max_sat_conflicts = 250;
    size_t superopt_max_func_insts = 250;
    size_t smt_guard_max_func_insts = 300;
    bool enable_cfg_simplify = true;
    bool enable_loop_unswitch = true;
    bool enable_jump_threading = true;
    bool enable_trace_layout = true;
    bool run_escape_analysis = false;
    bool enable_allocation_sinking = false;
    bool dump_pea_stats = false;
    bool run_alias_analysis = false;
    bool enable_vectorize = true;
    bool enable_slp = true;
    bool enable_loop_tile = true;
    size_t tile_size = 16;
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
    // The module runs in this process (brass's tiered pipeline), so its code
    // may hold this process's addresses (il_property.h set_in_process).
    bool in_process = false;
    // The module's shape cells (il.h Module::shapeCells): `shape.cell n`
    // reads word n of `__bronze_shape_cells`, which the entry registers with
    // the runtime with the cells' keys (il_abi.h kShapeCellsSymbol).
    uint32_t shape_cell_count = 0;
    // The keys eligible for a double slot (il.h Module::numberSlotKeys), by
    // key constant index: the entry registers them before anything runs
    // (bronze_register_slot_repr, il_abi.h kNumberSlotKeysSymbol), and an
    // object literal's fast path tests the values it stores under them.
    std::vector<uint32_t> number_slot_keys;
    // The pretenured allocation sites by slow site, ascending (il.h
    // Module::pretenureSites): their plain objects come from
    // bronze_alloc_pretenured (il_alloc_lowering.h bump_plain_object), each
    // with its record (IlLowering::pretenure_site).
    std::vector<uint32_t> pretenure_sites;
    // The module's object literals (il.h Module::objectLiterals): their keys
    // and inline slots. `object.literal n` caches its shape in words 2n and
    // 2n+1 of `__bronze_literal_shapes` (il_abi.h kLiteralShapesSymbol).
    struct ObjectLiteralMeta {
        std::vector<uint32_t> keys;
        uint32_t inline_slots = 0;
    };
    std::vector<ObjectLiteralMeta> object_literals;
    // The module's transition cells (il.h Module::protoCells): `proto.cell
    // n` compares word n of `__bronze_proto_cells` with the thread's
    // prototype epoch (il_abi.h kProtoCellsSymbol).
    struct ProtoCellMeta {
        uint32_t from_cell = 0;
        uint32_t key_index = 0;
        uint32_t to_cell = 0;
        int32_t slot = 0;
    };
    std::vector<ProtoCellMeta> proto_cells;
    // The module's member cells (il.h Module::memberCells): `member.cell n`
    // reads word 2n of `__bronze_member_cells` (il_abi.h kMemberCellsSymbol).
    struct MemberCellMeta {
        uint32_t kind = 0;  // il::Module::MemberKind
        uint32_t key_index = 0;
        uint32_t group = 0;
        uint32_t group_size = 1;
        uint32_t place = 0;  // a data cell's il::Module::memberDataPlace
    };
    std::vector<MemberCellMeta> member_cells;
    // The element cells (codegen-brass/elem_cells.h): the slow sites of the
    // module's computed reads, sorted; cell n is the read at element n.
    std::vector<uint32_t> elem_cell_sites;
    // THE DEOPT COUNTERS (codegen-brass/deopt_sites.h): `__bronze_deopt_counts`
    // holds one RETIRE word per generic function with resume points (by
    // rank), then one counter per point (by slot). A generic function's twin
    // bumps, at the entry of point `id`, the point's counter and the
    // function's retire word; once word r reaches `deopt_retire_after`, the
    // twin of G (rank r) retires every speculated function of G: calls of it
    // go through its entry cell to G from then on (il_lowering_resume.cpp).
    // After the counters, one OPTIMISTIC word per generic function (by rank):
    // an exit at an optimistic point bumps it instead of the retire word,
    // and once it reaches `deopt_optimistic_after` the twin points each
    // optimistic copy's entry cell where its fallback's points
    // (opt/speculate optimistic.h).
    struct DeoptFunctionMeta {
        uint32_t rank = 0;
        std::unordered_map<uint32_t, uint32_t> slots;  // resume id -> slot
        std::unordered_set<uint32_t> optimistic;       // the optimistic resume ids
    };
    std::unordered_map<std::string, DeoptFunctionMeta> deopt_generic;  // by G's name
    std::unordered_map<std::string, std::string> deopt_speculated;     // S's name -> G's name
    std::unordered_map<std::string, std::string> deopt_fallback;       // S+'s name -> S's name
    uint32_t deopt_generic_count = 0;
    uint32_t deopt_site_count = 0;
    uint64_t deopt_retire_after = 1000;
    uint64_t deopt_optimistic_after = 100;
    // SLOW SITES (il.h `slowSite`, il_property.h "SLOW ARMS"): the module's
    // count, laid out as u64 counters right before the IC table
    // (il_abi.h ic_slow_count_offset). With `count_slow_sites` (an image
    // that may be harvested: one built on no profile) each slow arm bumps its
    // instruction's counter, and lowering sets the site's bit in
    // `slow_armed` (SlowArmedBits, sized by the caller, which lays it out
    // before the counters).
    uint32_t slow_site_count = 0;
    bool count_slow_sites = false;
    // The image is built to be harvested (il::Module::harvestOutPath): its
    // named reads and writes count their off-way receivers too
    // (il/feedback_site.h kShapeTurnArm).
    bool cache_evidence = false;
    std::shared_ptr<SlowArmedBits> slow_armed;
    // COLD ENTRIES: words of the image's `__bronze_cold_entries` table (0:
    // none), one a function by its IL index; each cold function
    // (BronzeFunction::cold_entry) adds one to its word on entry.
    uint32_t cold_entry_count = 0;
    // INTERPRETED COLD FUNCTIONS (codegen-brass/cold_companion.h), by IL
    // function index: the function's cell in the image's
    // `__bronze_cold_cells` (il_abi.h kColdCellsSymbol), or kNotInterpreted.
    // An image gives each such function a STUB in place of its body
    // (il_lowering_cold.cpp): the call goes where its cell points, and the
    // first fills the cell (bronze_cold_resolve). Its wrapper stays.
    // A COMPANION (`companion`) is the other half: the module the cells point
    // into, run by the tiered pipeline. It lowers these bodies alone, and
    // every other function, wrapper and module stub is the image's, called by
    // its symbol with the signature the image gave it.
    static constexpr uint32_t kNotInterpreted = UINT32_MAX;
    std::vector<uint32_t> interpreted;
    bool companion = false;
    bool is_interpreted(size_t fn_index) const {
        return fn_index < interpreted.size() && interpreted[fn_index] != kNotInterpreted;
    }
    // A companion's deferred bodies (BronzeFunction::body_deferred): fills
    // AST function `fn_index`'s blocks, which the lowering of its body asks
    // for first; false when its body does not read. Only with lazy_bodies,
    // whose provider owns the AST it writes.
    std::function<bool(size_t fn_index, BronzeFunction& out)> deferred_body;
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
    TranslatorOptions options,
    DiagnosticReporter* diag = nullptr
);

} // namespace il2mir
