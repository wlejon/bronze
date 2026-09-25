#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_tiered_image.h"

#include "support/diagnostics.h"

#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/tiering.hpp>
#include <brass/gc/native_frames.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/interpreter/interpreter.hpp>
#include <brass/mir/function.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include <utility>

namespace bronze {

namespace {

// brass_enumerate_thread_roots reports the frames of the innermost running
// Interpreter and FastInterpreter only; one of the same kind hidden beneath
// an inner one is the host's to report. A bronze program run from inside
// another (an eval, a host callback that runs a second program) makes
// exactly that nesting, so every entry into a program reports, for its
// lifetime, whichever interpreters were running when it was entered.
// Reporting the entered interpreter itself again when it turns out to be
// the same one is harmless: the collector visits each slot once.
class OuterInterpreterRoots {
public:
    OuterInterpreterRoots() noexcept
        : fast_(brass::FastInterpreter::current()),
          interp_(brass::Interpreter::active_on_thread()),
          scope_(&report, this) {}
    OuterInterpreterRoots(const OuterInterpreterRoots&) = delete;
    OuterInterpreterRoots& operator=(const OuterInterpreterRoots&) = delete;

private:
    static void report(void* ctx, std::vector<uintptr_t*>& roots) {
        auto* self = static_cast<OuterInterpreterRoots*>(ctx);
        if (self->fast_) self->fast_->collect_all_roots(roots);
        if (self->interp_) self->interp_->collect_all_roots(roots);
    }

    brass::FastInterpreter* fast_;
    brass::Interpreter* interp_;
    brass::ThreadRootsScope scope_;
};

// The pipeline's configuration for a tier (ExecutionTier says what each is).
brass::runtime::TieringConfig tieringConfigFor(ExecutionTier tier) {
    brass::runtime::TieringConfig config;
    config.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
    switch (tier) {
        case ExecutionTier::Tier0_Interpreter:
            config.max_tier = brass::runtime::TierLevel::Tier0_Interpreter;
            break;
        case ExecutionTier::Tier1_Baseline:
            config.max_tier = brass::runtime::TierLevel::Tier1_Baseline;
            break;
        case ExecutionTier::Auto:
        case ExecutionTier::Tier2_Optimized:
            // Optimized code is built off the mutator: a hot function keeps
            // running in its lower tier until its code is installed. One
            // worker per program; most programs (an eval, a callback's
            // module) never queue anything and never start it.
            config.enable_background_compile = true;
            config.jit_threads = 1;
            break;
    }
    return config;
}

}  // namespace

std::string_view executionTierToString(ExecutionTier tier) noexcept {
    switch (tier) {
        case ExecutionTier::Tier0_Interpreter: return "interpreter";
        case ExecutionTier::Tier1_Baseline:    return "baseline";
        case ExecutionTier::Tier2_Optimized:   return "optimized";
        case ExecutionTier::Auto:              return "auto";
    }
    return "auto";
}

std::optional<ExecutionTier> parseExecutionTier(std::string_view str) noexcept {
    if (str == "0" || str == "tier0" || str == "interp" || str == "interpreter" || str == "fast") {
        return ExecutionTier::Tier0_Interpreter;
    }
    if (str == "1" || str == "tier1" || str == "baseline") {
        return ExecutionTier::Tier1_Baseline;
    }
    if (str == "2" || str == "tier2" || str == "opt" || str == "optimized" || str == "jit") {
        return ExecutionTier::Tier2_Optimized;
    }
    if (str == "3" || str == "auto" || str == "multi" || str == "tiered") {
        return ExecutionTier::Auto;
    }
    return std::nullopt;
}

BrassTieredProgram::BrassTieredProgram(ExecutionTier tier, std::string entrySymbol)
    : tier_(tier), entrySymbol_(std::move(entrySymbol)),
      dispatchTable_(std::make_unique<brass::runtime::FunctionDispatchTable>()) {}

// Member order does the teardown (the header says which way).
BrassTieredProgram::~BrassTieredProgram() {
    if (const brass::ModuleStackMap* maps = stackMaps(); maps && brass::brass_get_active_stack_maps() == maps) {
        brass::brass_set_active_stack_maps(nullptr);
    }
}

const brass::ModuleStackMap* BrassTieredProgram::stackMaps() const noexcept {
    if (tier_ == ExecutionTier::Tier2_Optimized) return jitProgram_ ? jitProgram_->stackMaps() : nullptr;
    return &dispatchTable_->pipeline().active_stack_maps();
}

void BrassTieredProgram::activateStackMaps() const {
    if (const brass::ModuleStackMap* maps = stackMaps()) {
        brass::brass_set_active_stack_maps(const_cast<brass::ModuleStackMap*>(maps));
    }
}

void* BrassTieredProgram::symbolAddress(std::string_view name) const {
    if (tier_ == ExecutionTier::Tier2_Optimized) return jitProgram_ ? jitProgram_->symbolAddress(name) : nullptr;
    if (void* addr = image_ ? image_->symbolAddress(name) : nullptr) return addr;
    const brass::Function* fn = mirModule_ ? mirModule_->get_function(name) : nullptr;
    if (!fn || fn->block_count() == 0) return nullptr;
    return dispatchTable_->pipeline().function_address(name, fn);
}

void BrassTieredProgram::stopBackgroundCompilation() {
    if (tier_ == ExecutionTier::Tier2_Optimized) return;
    dispatchTable_->pipeline().stop_background_compiles();
}

brass::RuntimeValue BrassTieredProgram::run() {
    // A program of no statements has no entry to run.
    if (tier_ != ExecutionTier::Tier2_Optimized && !mirModule_->get_function(entrySymbol_)) {
        return brass::RuntimeValue::from_u64(0);
    }
    return invoke(entrySymbol_, {});
}

brass::RuntimeValue BrassTieredProgram::invoke(std::string_view fnName,
                                              const std::vector<brass::RuntimeValue>& args) {
    // Native code opens no scope of its own; this makes brass's runtime name
    // lookups resolve in this program rather than the default one.
    brass::runtime::ProgramScope scope(*dispatchTable_);
    OuterInterpreterRoots outerRoots;
    activateStackMaps();
    if (tier_ == ExecutionTier::Tier2_Optimized) {
        if (!jitProgram_) return brass::RuntimeValue::from_u64(0);
        if (fnName == entrySymbol_) {
            auto* entry = reinterpret_cast<uint64_t (*)()>(jitProgram_->entryPoint());
            return brass::RuntimeValue::from_u64(entry ? entry() : 0);
        }
        return jitProgram_->engine()->invoke(fnName, args);
    }
    // The runtime's calls into this thread's interpreted functions
    // (embed::setEnterJsHook) are per thread, and the thread that compiled
    // a program need not be the one that runs it.
    installBronzeEnterJsHook();
    return dispatchTable_->pipeline().execute(*mirModule_, fnName, args);
}

BrassTieredEngine::BrassTieredEngine(const TieredEngineConfig& config)
    : config_(config) {}

BrassTieredEngine::~BrassTieredEngine() = default;

std::unique_ptr<BrassTieredProgram> BrassTieredEngine::compile(
    const il::Module& module, DiagnosticSink& diags) {

    const ExecutionTier tier = config_.tier;
    const std::string entrySymbol = config_.entrySymbol.empty() ? "main" : config_.entrySymbol;

    BrassBackend backend;
    backend.setEntrySymbol(entrySymbol);
    backend.setHostGlobals(config_.hostGlobals);
    backend.setOptimize(config_.optimize);
    backend.setPropagateExceptionsInEntry(config_.propagateExceptionsInEntry);
    backend.setEmitDebugInfo(config_.emitDebugInfo);

    std::unique_ptr<BrassTieredProgram> prog(new BrassTieredProgram(tier, entrySymbol));
    if (tier == ExecutionTier::Tier2_Optimized) {
        prog->jitProgram_ = backend.compileToJit(module, diags);
        if (!prog->jitProgram_) return nullptr;
        prog->activateStackMaps();
        return prog;
    }

    // A JIT program is compiled and run on one thread; its image is not
    // entered on others (BrassBackend::compileToJit says why that is the
    // right trade for in-process programs), so its tables are plain cells.
    backend.setPerThreadModuleData(false);
    std::vector<uint32_t> globalReadKeys;
    prog->mirModule_ = backend.buildMirModule(module, diags, &globalReadKeys);
    if (!prog->mirModule_) return nullptr;

    brass::runtime::MultiTierPipeline& pipeline = prog->dispatchTable().pipeline();
    pipeline.initialize(tieringConfigFor(tier));
    registerBronzeMultiTierSymbols(pipeline);

    prog->image_ = TieredProgramImage::load(backend.buildDataImage(module, globalReadKeys.size()),
                                            *prog->mirModule_, module, entrySymbol, pipeline, diags);
    if (!prog->image_) return nullptr;

    if (tier == ExecutionTier::Tier1_Baseline) {
        // Every function compiled before anything runs; one the baseline
        // compiler rejects stays interpreted.
        for (const brass::Function* fn : prog->mirModule_->functions()) {
            if (fn && fn->block_count() != 0) prog->dispatchTable().get_or_create(fn->name(), fn);
        }
        for (const brass::Function* fn : prog->mirModule_->functions()) {
            if (fn && fn->block_count() != 0) pipeline.compile_and_install_tier1(fn->name(), fn);
        }
    }
    prog->activateStackMaps();
    return prog;
}

}  // namespace bronze
