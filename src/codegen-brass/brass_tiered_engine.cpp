#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_tiered_image.h"
#include "embed/embed.h"

#include "support/diagnostics.h"

#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/exception.hpp>
#include <brass/runtime/osr_coordinator.hpp>
#include <brass/runtime/tiering.hpp>
#include <brass/mir/pass_pipeline.hpp>
#include <brass/gc/native_frames.hpp>
#include <brass/gc/runtime_gc.hpp>
#include <brass/interpreter/interpreter.hpp>
#include <brass/mir/function.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include <utility>

// runtime/module_instance.cpp
extern "C" uint64_t bronze_module_instance(uint64_t* slotCell, uint64_t* begin, uint64_t* end);

namespace bronze {

namespace {

// Backedges a function's interpreted loops take before a loop still running
// asks for its OSR entry: enough that a loop about to finish is not
// compiled for, few enough that a long one spends most of its time in
// optimized code.
constexpr uint64_t kOsrBackedgeThreshold = 1000;

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
            // Optimized code is built off the mutator, on the process's one
            // compile pool (brass CompilePool::shared()): a hot function
            // keeps running in its lower tier until its code is installed,
            // and a hot loop until its OSR entry is.
            config.enable_background_compile = true;
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
    dispatchTable_->osr().stop_compiles();
}

brass::RuntimeValue BrassTieredProgram::run() {
    // A program of no statements has no entry to run.
    if (tier_ != ExecutionTier::Tier2_Optimized && !mirModule_->get_function(entrySymbol_)) {
        return brass::RuntimeValue::from_u64(0);
    }
    return invoke(entrySymbol_, {});
}

// A throw leaving the program continues as the one exception the runtime
// knows (runtime/exception.h): a C++ BrassException carrying the value's bits.
// An interpreted frame's uncaught throw arrives as brass's interpreter
// exception and is raised again as that, past the catch block for the reason
// exception.h gives.
brass::RuntimeValue BrassTieredProgram::invoke(std::string_view fnName,
                                              const std::vector<brass::RuntimeValue>& args) {
    uint64_t thrown = 0;
    try {
        return invokeUntranslated(fnName, args);
    } catch (const brass::InterpreterThrownException& e) {
        thrown = e.value().raw_bits();
    }
    throw brass::runtime::BrassException(brass::HostValue::from_raw(thrown));
}

brass::RuntimeValue BrassTieredProgram::invokeUntranslated(std::string_view fnName,
                                                          const std::vector<brass::RuntimeValue>& args) {
    // Native code opens no scope of its own; this makes brass's runtime name
    // lookups resolve in this program rather than the default one.
    brass::runtime::ProgramScope scope(*dispatchTable_);
    // The thread that runs a program need not be the one that compiled it:
    // its heap must exist, and be brass's current heap, before the pipeline
    // builds the interpreter that runs it here, so that the interpreter's
    // frames are that heap's roots.
    embed::bindThreadHeap();
    activateStackMaps();
    if (tier_ == ExecutionTier::Tier2_Optimized) {
        if (!jitProgram_) return brass::RuntimeValue::from_u64(0);
        if (fnName == entrySymbol_) {
            auto* entry = reinterpret_cast<uint64_t (*)()>(jitProgram_->entryPoint());
            if (!entry) return brass::RuntimeValue::from_u64(0);
            // A native raise out of the entry leaves here as a C++ exception
            // (brass native_frames.hpp).
            brass::GeneratedCodeEntryScope entryScope;
            return brass::RuntimeValue::from_u64(entry());
        }
        return jitProgram_->engine()->invoke(fnName, args);
    }
    // The runtime's calls into this thread's interpreted functions
    // (embed::setEnterJsHook) are per thread, and the thread that compiled
    // a program need not be the one that runs it.
    installBronzeEnterJsHook();
    // A function other than the entry finds this thread's instance of the
    // module's data where the entry registered it; called first, it
    // registers it here.
    if (fnName != entrySymbol_) enterThreadInstance();
    return dispatchTable_->pipeline().execute(*mirModule_, fnName, args);
}

void BrassTieredProgram::enterThreadInstance() const {
    auto* slot = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_module_slot")));
    auto* begin = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_instance")));
    auto* end = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_instance_end")));
    if (slot && begin && end) bronze_module_instance(slot, begin, end);
}

const uint64_t* BrassTieredProgram::moduleSlotCell() const {
    if (!perThreadData()) return nullptr;
    return static_cast<const uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_module_slot")));
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
    // Tier 2 optimizes the whole program before it runs. The pipeline tiers
    // translate it as it stands and optimize a function when it tiers up.
    backend.setOptimize(tier == ExecutionTier::Tier2_Optimized);
    backend.setEmitDebugInfo(config_.emitDebugInfo);

    std::unique_ptr<BrassTieredProgram> prog(new BrassTieredProgram(tier, entrySymbol));
    if (tier == ExecutionTier::Tier2_Optimized) {
        prog->jitProgram_ = backend.compileToJit(module, diags);
        if (!prog->jitProgram_) return nullptr;
        prog->activateStackMaps();
        return prog;
    }

    // The module's writable tables are per thread (bronze_module_instance):
    // one compiled program runs on any number of threads, each with its own
    // globals and caches, as a worker running the same script does.
    std::vector<uint32_t> globalReadKeys;
    prog->mirModule_ = backend.buildMirModule(module, diags, &globalReadKeys);
    if (!prog->mirModule_) return nullptr;

    brass::runtime::MultiTierPipeline& pipeline = prog->dispatchTable().pipeline();
    pipeline.initialize(tieringConfigFor(tier));
    registerBronzeMultiTierSymbols(pipeline);
    if (tier == ExecutionTier::Auto) {
        // Tier-up and OSR code run the optimizer the whole-program tier runs,
        // one function (or one loop's entry) at a time.
        pipeline.set_tier2_passes(backend.tierUpPasses());
        brass::runtime::OsrCoordinator& osr = prog->dispatchTable().osr();
        osr.set_threshold(kOsrBackedgeThreshold);
        osr.set_enabled(true);
    }

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
