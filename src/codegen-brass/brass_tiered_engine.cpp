#include "codegen-brass/brass_tiered_engine.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_coroutine_bridge.h"
#include "codegen-brass/brass_backend_sections.h"

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "support/diagnostics.h"

#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/tiering.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/gc/runtime_gc.hpp>

#include <cstring>
#include <algorithm>
#include <utility>

namespace bronze {

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

// Member order does the teardown: compiled code, compiler and interpreter
// go first, then the dispatch table (which stops this program's background
// compiler and hands the GC's stack maps back), then the MIR module.
BrassTieredProgram::~BrassTieredProgram() {
    if (brass::brass_get_active_stack_maps() == &moduleStackMap_ ||
        (jitProgram_ && jitProgram_->stackMaps() && brass::brass_get_active_stack_maps() == jitProgram_->stackMaps())) {
        brass::brass_set_active_stack_maps(nullptr);
    }
}

void BrassTieredProgram::setJitProgram(std::unique_ptr<BrassJitProgram> jitProg) {
    jitProgram_ = std::move(jitProg);
    if (jitProgram_) {
        entryPoint_ = jitProgram_->entryPoint();
        if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_->stackMaps()) {
            brass::brass_set_active_stack_maps(jitProgram_->stackMaps());
        }
    }
}

void BrassTieredProgram::setMirModule(std::unique_ptr<brass::Module> mirMod) {
    mirModule_ = std::move(mirMod);
}

void BrassTieredProgram::setFastInterpreter(std::unique_ptr<brass::FastInterpreter> interp) {
    fastInterpreter_ = std::move(interp);
}

void BrassTieredProgram::setBaselineCompiler(std::unique_ptr<brass::codegen::BaselineJitCompiler> compiler) {
    baselineCompiler_ = std::move(compiler);
}

void BrassTieredProgram::setBaselineCompiledFunctions(std::vector<brass::codegen::BaselineCompiledFunction> fns) {
    compiledFunctions_ = std::move(fns);
    functionIndexMap_.clear();
    moduleStackMap_.clear();
    for (size_t i = 0; i < compiledFunctions_.size(); ++i) {
        const std::string name(compiledFunctions_[i].name());
        functionIndexMap_[name] = i;
        symbolAddressMap_[name] = compiledFunctions_[i].entry_point();
        if (baselineCompiler_) {
            baselineCompiler_->register_external_symbol(name, compiledFunctions_[i].entry_point());
        }
        if (compiledFunctions_[i].name() == entrySymbol_ ||
            (entrySymbol_ != "main" && compiledFunctions_[i].name() == "main")) {
            entryPoint_ = compiledFunctions_[i].entry_point();
        }
        moduleStackMap_.add_function(compiledFunctions_[i].stack_map());
    }
    if (!entryPoint_ && !compiledFunctions_.empty()) {
        entryPoint_ = compiledFunctions_[0].entry_point();
    }
    if (tier_ == ExecutionTier::Tier1_Baseline) {
        brass::brass_set_active_stack_maps(&moduleStackMap_);
    }
}

const brass::ModuleStackMap* BrassTieredProgram::stackMaps() const noexcept {
    if (tier_ == ExecutionTier::Tier1_Baseline) {
        return &moduleStackMap_;
    }
    if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_) {
        return jitProgram_->stackMaps();
    }
    if (tier_ == ExecutionTier::Auto) {
        return &dispatchTable_->pipeline().active_stack_maps();
    }
    return nullptr;
}

void BrassTieredProgram::initDataBuffers(const il::Module& ilMod, const std::vector<uint32_t>& globalReadKeys) {
    moduleEnv_ = BRONZE_ABI_UNDEFINED_BITS;

    // Build key manifest
    keyManifest_.resize(sizeof(uint32_t));
    const uint32_t keyCount = static_cast<uint32_t>(ilMod.keyConstants.size());
    std::memcpy(keyManifest_.data(), &keyCount, sizeof(uint32_t));
    for (const auto& key : ilMod.keyConstants) {
        const uint32_t len = static_cast<uint32_t>(key.size());
        const size_t cur = keyManifest_.size();
        keyManifest_.resize(cur + sizeof(uint32_t) + len + 1);
        std::memcpy(keyManifest_.data() + cur, &len, sizeof(uint32_t));
        std::memcpy(keyManifest_.data() + cur + sizeof(uint32_t), key.data(), len);
        keyManifest_.back() = 0;
    }

    // Key map
    keyMap_.assign(std::max<size_t>(ilMod.keyConstants.size(), 1), 0);

    // Template cells
    const size_t tplCount = std::max<size_t>(1024, static_cast<size_t>(ilMod.templateSiteCount) + 128);
    templateCells_.assign(tplCount, BRONZE_ABI_UNDEFINED_BITS);

    // Global cache
    const size_t cacheCells = std::max<size_t>(globalReadKeys.size(), 1);
    globalCache_.assign(cacheCells, BRONZE_ABI_NO_EXCEPTION_BITS);

    // IC table
    const size_t icSites = std::max<size_t>(ilMod.icSiteCount, 1);
    icTable_.assign(icSites * BRONZE_ABI_IC_SITE_SIZE, 0);

    // Method IC sites
    const std::vector<uint32_t> mSites = ilMod.methodIcSites();
    methodIcSites_.clear();
    methodIcSites_.reserve(mSites.size());
    for (uint32_t s : mSites) {
        methodIcSites_.push_back(static_cast<uint64_t>(s));
    }

    // Native imports table
    if (!ilMod.nativeImports.empty()) {
        const auto& imports = ilMod.nativeImports;
        const uint32_t count = static_cast<uint32_t>(imports.size());
        const uint32_t namesOffset = static_cast<uint32_t>(8 + count * sizeof(uint64_t));
        nativeImportsTable_.resize(namesOffset);
        std::memcpy(nativeImportsTable_.data(), &count, sizeof(uint32_t));
        std::memcpy(nativeImportsTable_.data() + sizeof(uint32_t), &namesOffset, sizeof(uint32_t));
        for (size_t i = 0; i < imports.size(); ++i) {
            *reinterpret_cast<uint64_t*>(nativeImportsTable_.data() + 8 + i * sizeof(uint64_t)) = 0;
        }
        for (const auto& imp : imports) {
            const size_t cur = nativeImportsTable_.size();
            nativeImportsTable_.resize(cur + imp.name.size() + 1 + imp.signature.size() + 1);
            std::memcpy(nativeImportsTable_.data() + cur, imp.name.data(), imp.name.size());
            nativeImportsTable_[cur + imp.name.size()] = 0;
            const size_t sigOffset = cur + imp.name.size() + 1;
            std::memcpy(nativeImportsTable_.data() + sigOffset, imp.signature.data(), imp.signature.size());
            nativeImportsTable_[sigOffset + imp.signature.size()] = 0;
        }
    }
}

void BrassTieredProgram::registerSymbolsWithEngines() {
    auto regSym = [this](const std::string& name, void* addr) {
        symbolAddressMap_[name] = addr;
        if (fastInterpreter_) {
            fastInterpreter_->register_external_symbol(name, addr);
        }
        if (baselineCompiler_) {
            baselineCompiler_->register_external_symbol(name, addr);
        }
        if (tier_ == ExecutionTier::Auto) {
            dispatchTable_->pipeline().register_external_symbol(name, addr);
        }
    };

    const std::string entry = entrySymbol_;
    const auto modSym = [&](const std::string& base) {
        return codegen::moduleSymbolName(entry, base);
    };

    regSym("__bronze_module_env", &moduleEnv_);
    regSym(modSym("__bronze_module_env"), &moduleEnv_);

    regSym("__bronze_key_map", keyMap_.data());
    regSym(modSym("__bronze_key_map"), keyMap_.data());

    regSym("__bronze_template_cells", templateCells_.data());
    regSym(modSym("__bronze_template_cells"), templateCells_.data());

    regSym("__bronze_global_cache", globalCache_.data());
    regSym(modSym("__bronze_global_cache"), globalCache_.data());

    regSym("__bronze_ic_table", icTable_.data());
    regSym(modSym("__bronze_ic_table"), icTable_.data());

    if (!methodIcSites_.empty()) {
        regSym("__bronze_method_ic_sites", methodIcSites_.data());
        regSym(modSym("__bronze_method_ic_sites"), methodIcSites_.data());
    }

    const std::string keySym = (entry.empty() || entry == "main" || entry == "bronze_main")
                                    ? "bronze_main_key_constants"
                                    : (entry + "_key_constants");
    regSym("bronze_main_key_constants", keyManifest_.data());
    regSym("main_key_constants", keyManifest_.data());
    regSym(keySym, keyManifest_.data());

    if (!nativeImportsTable_.empty()) {
        regSym(entry + "_native_imports", nativeImportsTable_.data());
        regSym("native_imports", nativeImportsTable_.data());
    }
}

void* BrassTieredProgram::entryPoint() const noexcept {
    if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_) {
        return jitProgram_->entryPoint();
    }
    return entryPoint_;
}

void* BrassTieredProgram::symbolAddress(std::string_view name) const {
    if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_) {
        return jitProgram_->symbolAddress(name);
    }
    const std::string key(name);
    auto it = symbolAddressMap_.find(key);
    if (it != symbolAddressMap_.end()) {
        return it->second;
    }
    auto fnIt = functionIndexMap_.find(key);
    if (fnIt != functionIndexMap_.end()) {
        return compiledFunctions_[fnIt->second].entry_point();
    }
    if (tier_ == ExecutionTier::Auto) {
        auto fn = dispatchTable_->pipeline().find_baseline_compiled(name);
        if (fn) return fn->entry_point();
    }
    return nullptr;
}

brass::RuntimeValue BrassTieredProgram::run() {
    // Native entries (Tier 1) open no scope of their own; this makes brass's
    // runtime name lookups resolve in this program rather than the default one.
    brass::runtime::ProgramScope scope(*dispatchTable_);
    if (tier_ == ExecutionTier::Tier1_Baseline) {
        brass::brass_set_active_stack_maps(&moduleStackMap_);
    } else if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_ && jitProgram_->stackMaps()) {
        brass::brass_set_active_stack_maps(jitProgram_->stackMaps());
    } else if (tier_ == ExecutionTier::Auto) {
        brass::brass_set_active_stack_maps(&dispatchTable_->pipeline().active_stack_maps());
    }

    switch (tier_) {
        case ExecutionTier::Tier0_Interpreter: {
            if (!fastInterpreter_ || !mirModule_) return brass::RuntimeValue::from_u64(0);
            return fastInterpreter_->run(*mirModule_, entrySymbol_);
        }
        case ExecutionTier::Tier1_Baseline: {
            if (entryPoint_) {
                auto fn = reinterpret_cast<uint64_t (*)()>(entryPoint_);
                return brass::RuntimeValue::from_u64(fn());
            }
            return brass::RuntimeValue::from_u64(0);
        }
        case ExecutionTier::Tier2_Optimized: {
            if (jitProgram_) {
                if (jitProgram_->entryPoint()) {
                    auto fn = reinterpret_cast<uint64_t (*)()>(jitProgram_->entryPoint());
                    return brass::RuntimeValue::from_u64(fn());
                }
            }
            return brass::RuntimeValue::from_u64(0);
        }
        case ExecutionTier::Auto: {
            if (!mirModule_) return brass::RuntimeValue::from_u64(0);
            return dispatchTable_->pipeline().execute(*mirModule_, entrySymbol_);
        }
    }
    return brass::RuntimeValue::from_u64(0);
}

brass::RuntimeValue BrassTieredProgram::invoke(std::string_view fnName,
                                              const std::vector<brass::RuntimeValue>& args) {
    brass::runtime::ProgramScope scope(*dispatchTable_);
    if (tier_ == ExecutionTier::Tier1_Baseline) {
        brass::brass_set_active_stack_maps(&moduleStackMap_);
    } else if (tier_ == ExecutionTier::Tier2_Optimized && jitProgram_ && jitProgram_->stackMaps()) {
        brass::brass_set_active_stack_maps(jitProgram_->stackMaps());
    } else if (tier_ == ExecutionTier::Auto) {
        brass::brass_set_active_stack_maps(&dispatchTable_->pipeline().active_stack_maps());
    }

    switch (tier_) {
        case ExecutionTier::Tier0_Interpreter: {
            if (!fastInterpreter_) return brass::RuntimeValue::from_u64(0);
            return fastInterpreter_->run(fnName, args);
        }
        case ExecutionTier::Tier1_Baseline: {
            auto it = functionIndexMap_.find(std::string(fnName));
            if (it != functionIndexMap_.end()) {
                return compiledFunctions_[it->second].invoke(args);
            }
            return brass::RuntimeValue::from_u64(0);
        }
        case ExecutionTier::Tier2_Optimized: {
            if (jitProgram_ && jitProgram_->engine()) {
                return jitProgram_->engine()->invoke(fnName, args);
            }
            void* addr = symbolAddress(fnName);
            if (addr) {
                auto fn = reinterpret_cast<uint64_t (*)()>(addr);
                return brass::RuntimeValue::from_u64(fn());
            }
            return brass::RuntimeValue::from_u64(0);
        }
        case ExecutionTier::Auto: {
            if (!mirModule_) return brass::RuntimeValue::from_u64(0);
            return dispatchTable_->pipeline().execute(*mirModule_, fnName, args);
        }
    }
    return brass::RuntimeValue::from_u64(0);
}

BrassTieredEngine::BrassTieredEngine(const TieredEngineConfig& config)
    : config_(config) {}

BrassTieredEngine::~BrassTieredEngine() = default;

std::unique_ptr<BrassTieredProgram> BrassTieredEngine::compile(
    const il::Module& module, DiagnosticSink& diags) {

    const ExecutionTier tier = config_.tier;
    const std::string entrySymbol = config_.entrySymbol.empty() ? "main" : config_.entrySymbol;

    if (tier == ExecutionTier::Tier2_Optimized) {
        BrassBackend backend;
        backend.setEntrySymbol(entrySymbol);
        backend.setHostGlobals(config_.hostGlobals);
        backend.setOptimize(config_.optimize);
        backend.setPropagateExceptionsInEntry(config_.propagateExceptionsInEntry);
        backend.setEmitDebugInfo(config_.emitDebugInfo);

        auto jitProg = backend.compileToJit(module, diags);
        if (!jitProg) return nullptr;

        auto prog = std::make_unique<BrassTieredProgram>(ExecutionTier::Tier2_Optimized, entrySymbol);
        prog->setJitProgram(std::move(jitProg));
        return prog;
    }

    BrassBackend backend;
    backend.setEntrySymbol(entrySymbol);
    backend.setHostGlobals(config_.hostGlobals);
    backend.setOptimize(config_.optimize);
    backend.setPropagateExceptionsInEntry(config_.propagateExceptionsInEntry);
    backend.setEmitDebugInfo(config_.emitDebugInfo);
    // No object file holds the source text and entry tables in these tiers
    // (brass_backend_sections.cpp builds them), so their data symbols would
    // not resolve: the baseline JIT rejects an unresolved data symbol.
    backend.setRegisterFnSources(false);
    backend.setPerThreadModuleData(false);

    std::vector<uint32_t> globalReadKeys;
    auto mirMod = backend.buildMirModule(module, diags, &globalReadKeys);
    if (!mirMod) return nullptr;

    transformCoroutinesIfNeeded(*mirMod);

    auto prog = std::make_unique<BrassTieredProgram>(tier, entrySymbol);
    prog->initDataBuffers(module, globalReadKeys);

    if (tier == ExecutionTier::Tier0_Interpreter) {
        auto interp = std::make_unique<brass::FastInterpreter>(config_.gcSemispaceSize);
        registerBronzeFastInterpreterSymbols(*interp);
        interp->set_dispatch_table(&prog->dispatchTable());
        interp->set_module(mirMod.get());
        prog->setFastInterpreter(std::move(interp));
        prog->registerSymbolsWithEngines();
    } else if (tier == ExecutionTier::Tier1_Baseline) {
        auto compiler = std::make_unique<brass::codegen::BaselineJitCompiler>(brass::Target::host());
        registerBronzeBaselineSymbols(*compiler);
        compiler->set_dispatch_table(&prog->dispatchTable());
        prog->setBaselineCompiler(std::move(compiler));
        prog->registerSymbolsWithEngines();

        auto compiledFns = prog->baselineCompiler()->compile_module(*mirMod);
        prog->setBaselineCompiledFunctions(std::move(compiledFns));
    } else if (tier == ExecutionTier::Auto) {
        auto& pipeline = prog->dispatchTable().pipeline();
        brass::runtime::TieringConfig tierConfig;
        tierConfig.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
        pipeline.initialize(tierConfig);
        registerBronzeMultiTierSymbols(pipeline);
        prog->registerSymbolsWithEngines();
        brass::brass_set_active_stack_maps(&pipeline.active_stack_maps());
    }

    prog->setMirModule(std::move(mirMod));
    return prog;
}

}  // namespace bronze
