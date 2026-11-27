#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_backend_sections.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_tiered_image.h"
#include "codegen-brass/il2mir/il_pipeline.h"
#include "codegen-brass/il2mir/il_translator.h"
#include "embed/embed.h"

#include "support/diagnostics.h"
#include "support/timings.h"

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

#include <cstdint>
#include <cstdlib>
#include <string_view>
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

// Whether an in-process compile verifies the MIR it lowers. It checks the
// translator rather than the program, and on a 40k-line app it was a quarter
// of the load, so a host build skips it; bronze's own builds with tests (and
// Debug builds) keep it, which is where a translator bug is to be caught.
// BRONZE_VERIFY_MIR=1 / =0 overrides either way.
#ifndef BRONZE_VERIFY_MIR_DEFAULT
#define BRONZE_VERIFY_MIR_DEFAULT 1
#endif
bool jitVerifiesMir() {
    static const bool on = [] {
        if (const char* v = std::getenv("BRONZE_VERIFY_MIR"); v && (v[0] == '0' || v[0] == '1')) {
            return v[0] == '1';
        }
        return BRONZE_VERIFY_MIR_DEFAULT != 0;
    }();
    return on;
}

// The pipeline's configuration: Fast Tier-0 interpreter with background Tier-1
// and Tier-2 compilation + OSR.
brass::runtime::TieringConfig defaultTieringConfig() {
    brass::runtime::TieringConfig config;
    config.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
    // Optimized code is built off the mutator, on the process's one compile
    // pool (brass CompilePool::shared()): a hot function keeps running in its
    // lower tier until its code is installed, and a hot loop until its OSR
    // entry is. Baseline code too: a function past the tier-1 threshold stays
    // interpreted until its code, and that of every function it reaches, is
    // ready.
    config.enable_background_compile = true;
    config.enable_background_tier1 = true;
    // A failed guard of brass's own tier-2 code finishes its call in Tier 0
    // (a loop in it interpreted to the end), and code whose guards failed
    // twice is dropped rather than paying three more.
    config.deopt_threshold = 2;
    return config;
}

}  // namespace

BrassTieredProgram::BrassTieredProgram(std::string entrySymbol)
    : entrySymbol_(std::move(entrySymbol)),
      dispatchTable_(std::make_unique<brass::runtime::FunctionDispatchTable>()) {}

// Member order does the teardown (the header says which way).
BrassTieredProgram::~BrassTieredProgram() {
    if (const brass::ModuleStackMap* maps = stackMaps(); maps && brass::brass_get_active_stack_maps() == maps) {
        brass::brass_set_active_stack_maps(nullptr);
    }
    // The image outlives the dispatch table that owns the pipeline.
    if (image_) image_->detachPipeline();
}

const brass::ModuleStackMap* BrassTieredProgram::stackMaps() const noexcept {
    return &dispatchTable_->pipeline().active_stack_maps();
}

void BrassTieredProgram::activateStackMaps() const {
    if (const brass::ModuleStackMap* maps = stackMaps()) {
        brass::brass_set_active_stack_maps(const_cast<brass::ModuleStackMap*>(maps));
    }
}

void* BrassTieredProgram::symbolAddress(std::string_view name) const {
    if (void* addr = image_ ? image_->symbolAddress(name) : nullptr) return addr;
    const brass::Function* fn = mirModule_ ? mirModule_->get_function(name) : nullptr;
    if (!fn || !fn->has_body()) return nullptr;
    return dispatchTable_->pipeline().function_address(name, fn);
}

void BrassTieredProgram::stopBackgroundCompilation() {
    dispatchTable_->pipeline().stop_background_compiles();
    dispatchTable_->osr().stop_compiles();
}

brass::RuntimeValue BrassTieredProgram::run() {
    // A program of no statements has no entry to run.
    if (!mirModule_->get_function(entrySymbol_)) {
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
    if (!perThreadData_) return;
    auto* slot = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_module_slot")));
    auto* begin = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_instance")));
    auto* end = static_cast<uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_instance_end")));
    if (slot && begin && end) bronze_module_instance(slot, begin, end);
}

const uint64_t* BrassTieredProgram::moduleSlotCell() const {
    if (!perThreadData()) return nullptr;
    return static_cast<const uint64_t*>(symbolAddress(codegen::moduleSymbolName(entrySymbol_, "__bronze_module_slot")));
}

const void* BrassTieredProgram::icIdentity() const {
    return symbolAddress(codegen::icIdentitySymbol(entrySymbol_));
}

BrassTieredEngine::BrassTieredEngine(const TieredEngineConfig& config)
    : config_(config) {}

BrassTieredEngine::~BrassTieredEngine() = default;

std::unique_ptr<BrassTieredProgram> BrassTieredEngine::compile(
    const il::Module& module, DiagnosticSink& diags) {

    const std::string entrySymbol = config_.entrySymbol.empty() ? "main" : config_.entrySymbol;
    const bool perThreadData = config_.perThreadModuleData;

    BrassBackend backend;
    backend.setEntrySymbol(entrySymbol);
    backend.setHostGlobals(config_.hostGlobals);
    backend.setPerThreadModuleData(perThreadData);
    // Translate as it stands and optimize each function when it tiers up.
    backend.setOptimize(false);
    backend.setEmitDebugInfo(config_.emitDebugInfo);
    backend.setVerifyLowered(jitVerifiesMir());
    // Every closure's code is the function's module stub
    // (MultiTierPipeline::function_address), which follows the best code and
    // bridges to Tier 0 for a function not compiled yet. So a compiled caller
    // may enter it directly when the program is retained for the process,
    // skipping rtEnterJs and its scopes.
    const bool entersDirectly = config_.livesForProcess;
    if (entersDirectly) backend.setDirectJsCalls(true);
    // Build a function's MIR when it first runs, or when a tier-up copies it:
    // a large program's load lowers what its start needs, not every function.
    backend.setLazyBodies(true);
    backend.setHarvestEvidence(config_.harvestEvidence);

    bool hasEscapable = false;
    for (const auto& fn : module.functions) {
        if (!fn.isExternal && (!fn.isEntryPoint || fn.isCoroutineBody())) {
            hasEscapable = true;
            break;
        }
    }

    std::unique_ptr<BrassTieredProgram> prog(new BrassTieredProgram(entrySymbol));
    prog->perThreadData_ = perThreadData;
    prog->hasEscapableCode_ = hasEscapable;
    prog->entersDirectly_ = entersDirectly;

    // The module's writable tables are per thread (bronze_module_instance):
    // one compiled program runs on any number of threads, each with its own
    // globals and caches, as a worker running the same script does.
    backend.setInProcess(true);
    // The inside of eval's "codegen" phase for a tiered program.
    support::PhaseTimer ptimer(support::timingsEnabled(), 4);
    std::vector<uint32_t> globalReadKeys;
    prog->mirModule_ = backend.buildMirModule(module, diags, &globalReadKeys);
    if (!prog->mirModule_) return nullptr;
    ptimer.mark("mir");

    // Recursion depth is the native stack's to bound: every function's code
    // checks the stack against the thread's limit (bronze_tls_enter) and
    // throws a RangeError there, in Tier 0 as in compiled code. The fast
    // interpreter's own frame count (10000 by default) would otherwise end
    // a deep recursion first on a large stack (an 8 MB+ main thread on arm64
    // macOS, any thread given more) with an InterpreterException no script
    // can catch: async_suspend_protocol aborted that way under a raised
    // `ulimit -s`.
    brass::FastInterpreter::set_default_max_call_depth(SIZE_MAX);
    brass::runtime::MultiTierPipeline& pipeline = prog->dispatchTable().pipeline();
    pipeline.initialize(defaultTieringConfig());
    registerBronzeMultiTierSymbols(pipeline);
    // Tier-up and OSR code run the optimizer one function (or one loop's entry) at a time.
    pipeline.set_tier2_passes(backend.tierUpPasses());
    brass::runtime::OsrCoordinator& osr = prog->dispatchTable().osr();
    osr.set_threshold(kOsrBackedgeThreshold);
    osr.set_enabled(true);

    ptimer.mark("pipeline");
    brass::object::ObjectFile dataImage = backend.buildDataImage(module, globalReadKeys.size());
    ptimer.mark("data image");
    prog->image_ = TieredProgramImage::load(std::move(dataImage), *prog->mirModule_, module, entrySymbol,
                                            pipeline, diags);
    if (!prog->image_) return nullptr;
    ptimer.mark("image load");
    // A body built after this (lazy bodies, a tier-up's) arms its slow sites
    // in the image's own bitmap, the words right before the counters, which
    // the harvest reads.
    if (const auto& armed = backend.slowArmed()) {
        auto* counts = static_cast<uint64_t*>(
            prog->image_->symbolAddress(codegen::moduleSymbolName(entrySymbol, "__bronze_slow_counts")));
        if (counts) armed->attach(counts - armed->words.size());
    }

    prog->codeTokens_.reserve(prog->mirModule_->functions().size() + 1);
    prog->codeTokens_.push_back(&prog->dispatchTable());
    for (const brass::Function* fn : prog->mirModule_->functions()) {
        if (!fn || !fn->has_body()) continue;
        if (void* addr = pipeline.function_address(fn->name(), fn)) {
            prog->codeTokens_.push_back(addr);
        }
    }

    prog->activateStackMaps();
    return prog;
}

}  // namespace bronze
