// Function bodies built one at a time: up front by lower_module, or lazily,
// when brass first needs one (TranslatorOptions::lazy_bodies).

#include "il_lowering.h"
#include "support/timings.h"
#include <brass/mir/printer.hpp>
#include <brass/mir/verifier.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

namespace il2mir {

bool IlLowering::lower_body(size_t i, Module& mod) {
    current_fn_idx_ = i;
    return lower_function(current_ast_->functions[i], mod, resolved_names_[i]);
}

bool IlLowering::lower_wrapper(size_t i, Module& mod) {
    const BronzeFunction& fn_ast = current_ast_->functions[i];
    const std::string& fn_name = resolved_names_[i];
    if (fn_name == "main" || fn_ast.is_coroutine_body()) return true;
    current_fn_idx_ = i;
    // What the function's body computed at its entry is not the wrapper's.
    current_module_delta_ = nullptr;
    uint32_t arity = static_cast<uint32_t>(fn_ast.params.size());
    auto it_ar = callee_param_counts_.find(fn_ast.name);
    if (it_ar != callee_param_counts_.end()) {
        arity = it_ar->second;
    } else if (auto it_res = callee_param_counts_.find(fn_name); it_res != callee_param_counts_.end()) {
        arity = it_res->second;
    }
    const bool is_closure = closure_functions_.count(fn_ast.name) || closure_functions_.count(fn_name);
    return emit_wrapper(fn_ast, mod, fn_name, arity, is_closure);
}

bool IlLowering::must_lower_eagerly(size_t i) const {
    const BronzeFunction& fn_ast = current_ast_->functions[i];
    // The entry registers the module's tables, and runs first anyway.
    if (resolved_names_[i] == "main") return true;
    // brass lowers coroutines once, over the bodies the module has when it
    // starts, pairing every coro_create with its body: both ends are built.
    if (fn_ast.is_coroutine_body()) return true;
    for (const auto& blk : fn_ast.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == BronzeOp::CoroStart || inst.op == BronzeOp::CoroSuspend) return true;
        }
    }
    return false;
}

bool IlLowering::lower_lazy_body(Function& fn) {
    const auto it = lazy_bodies_.find(&fn);
    if (it == lazy_bodies_.end()) {
        std::fprintf(stderr, "il2mir: '%.*s' is not a lazy body of this lowering\n",
                     static_cast<int>(fn.name().size()), fn.name().data());
        return false;
    }
    const LazyBody lazy = it->second;
    Module& mod = *fn.parent();
    const bool ok = lazy.wrapper ? lower_wrapper(lazy.index, mod) : lower_body(lazy.index, mod);
    if (!ok || has_error_) return false;
    // The module's one verification pass ran before this body existed.
    if (options_.verify_lowered_module && !verify_function(fn, diag_)) return false;
    return true;
}

namespace {

// Under BRONZE_TIMINGS: how many lazy bodies the process built and the time
// it spent building them, printed at exit (a host may never destroy the
// program, so the module cannot report it).
struct LazyStats {
    std::atomic<uint64_t> bodies{0};
    std::atomic<uint64_t> micros{0};
};
LazyStats& lazyStats() {
    static LazyStats* stats = [] {
        auto* s = new LazyStats();
        std::atexit([] {
            const LazyStats& st = lazyStats();
            std::fprintf(stderr, "il2mir: %llu lazy bodies built, %.1f ms\n",
                         static_cast<unsigned long long>(st.bodies.load()), st.micros.load() / 1000.0);
        });
        return s;
    }();
    return *stats;
}

// What a lazily lowered module's body provider owns.
struct LazyLowering {
    BronzeModuleAST ast;
    DiagnosticReporter diag;
    std::unique_ptr<IlLowering> lowering;
};

} // namespace

TranslationResult translate_bronze_ast_lazy(BronzeModuleAST&& ast, const TranslatorOptions& options,
                                            DiagnosticReporter* diag) {
    if (!options.lazy_bodies) return translate_bronze_ast(ast, options, diag);
    TranslationResult result;
    auto state = std::make_shared<LazyLowering>();
    state->ast = std::move(ast);
    // Up front, the caller's reporter; a lazy body reports into the
    // provider's own, since the caller's is gone by then.
    DiagnosticReporter default_diag;
    DiagnosticReporter* active_diag = diag ? diag : &default_diag;
    state->lowering = std::make_unique<IlLowering>(options, active_diag);
    auto mod = state->lowering->lower_module(state->ast);
    if (!mod) {
        result.error_message = active_diag->has_errors() ? active_diag->format_all()
                                                         : "Failed to lower Bronze IL AST to MIR";
        return result;
    }
    state->lowering->set_diag(&state->diag);
    const bool timed = bronze::support::timingsEnabled();
    // BRONZE_LAZY_TRACE=1: one line per body built, with its cost; =2 also
    // prints the body.
    static const int trace = [] {
        const char* v = std::getenv("BRONZE_LAZY_TRACE");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    mod->set_body_provider([state, timed](Function& fn) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = state->lowering->lower_lazy_body(fn);
        if (trace) {
            size_t insts = 0;
            for (const BasicBlock* bb : fn.blocks()) insts += bb->instruction_count();
            std::fprintf(stderr, "il2mir: built %.*s, %zu blocks, %zu insts, %lld us\n",
                         static_cast<int>(fn.name().size()), fn.name().data(), fn.blocks().size(), insts,
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() - start)
                                                    .count()));
            if (trace == 2 && ok) std::fprintf(stderr, "%s\n", brass::to_string(fn).c_str());
        }
        if (timed) {
            LazyStats& st = lazyStats();
            st.bodies.fetch_add(1, std::memory_order_relaxed);
            st.micros.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                          std::chrono::steady_clock::now() - start)
                                                          .count()),
                                std::memory_order_relaxed);
        }
        if (ok) return true;
        if (state->diag.has_errors()) std::fprintf(stderr, "%s\n", state->diag.format_all().c_str());
        return false;
    });
    result.success = true;
    result.module = std::move(mod);
    return result;
}

} // namespace il2mir
