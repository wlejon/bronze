#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include <brass/interpreter/value.hpp>
#include <brass/codegen/baseline_jit.hpp>
#include <brass/vm/fast_interpreter.hpp>

namespace bronze {

namespace il {
struct Module;
}
class DiagnosticSink;
class BrassJitProgram;

enum class ExecutionTier : uint8_t {
    Tier0_Interpreter = 0, // FastInterpreter direct-threaded bytecode engine
    Tier1_Baseline = 1,    // Baseline JIT compiler
    Tier2_Optimized = 2,   // Full AOT ModuleCompiler -> ObjectFile -> JitExecutionEngine
    Auto = 3               // MultiTierPipeline (dynamic tiering Tier 0 -> Tier 1 -> Tier 2)
};

std::string_view executionTierToString(ExecutionTier tier) noexcept;
std::optional<ExecutionTier> parseExecutionTier(std::string_view str) noexcept;

struct TieredEngineConfig {
    ExecutionTier tier = ExecutionTier::Auto;
    std::string entrySymbol = "main";
    std::vector<std::string> hostGlobals;
    bool optimize = true;
    bool propagateExceptionsInEntry = true;
    bool emitDebugInfo = false;
    size_t gcSemispaceSize = 1024 * 1024;
};

class BrassTieredProgram {
public:
    BrassTieredProgram(ExecutionTier tier, std::string entrySymbol);
    ~BrassTieredProgram();

    BrassTieredProgram(const BrassTieredProgram&) = delete;
    BrassTieredProgram& operator=(const BrassTieredProgram&) = delete;

    BrassTieredProgram(BrassTieredProgram&&) noexcept;
    BrassTieredProgram& operator=(BrassTieredProgram&&) noexcept;

    ExecutionTier tier() const noexcept { return tier_; }
    std::string_view entrySymbol() const noexcept { return entrySymbol_; }

    void* entryPoint() const noexcept;
    void* symbolAddress(std::string_view name) const;

    brass::RuntimeValue run();
    brass::RuntimeValue invoke(std::string_view fnName,
                               const std::vector<brass::RuntimeValue>& args = {});

    void setJitProgram(std::unique_ptr<BrassJitProgram> jitProg);
    BrassJitProgram* jitProgram() const noexcept { return jitProgram_.get(); }

    void setMirModule(std::unique_ptr<brass::Module> mirMod);
    brass::Module* mirModule() const noexcept { return mirModule_.get(); }

    void setFastInterpreter(std::unique_ptr<brass::FastInterpreter> interp);
    brass::FastInterpreter* fastInterpreter() const noexcept { return fastInterpreter_.get(); }

    void setBaselineCompiler(std::unique_ptr<brass::codegen::BaselineJitCompiler> compiler);
    brass::codegen::BaselineJitCompiler* baselineCompiler() const noexcept { return baselineCompiler_.get(); }

    void setBaselineCompiledFunctions(std::vector<brass::codegen::BaselineCompiledFunction> fns);
    const std::vector<brass::codegen::BaselineCompiledFunction>& baselineCompiledFunctions() const noexcept {
        return compiledFunctions_;
    }
    const brass::ModuleStackMap* stackMaps() const noexcept;

    void initDataBuffers(const il::Module& ilMod, const std::vector<uint32_t>& globalReadKeys);
    void registerSymbolsWithEngines();

    void* moduleEnvPtr() noexcept { return &moduleEnv_; }
    uint32_t* keyMapData() noexcept { return keyMap_.data(); }
    uint64_t* templateCellsData() noexcept { return templateCells_.data(); }
    uint64_t* globalCacheData() noexcept { return globalCache_.data(); }
    uint8_t* icTableData() noexcept { return icTable_.data(); }
    uint64_t* methodIcSitesData() noexcept { return methodIcSites_.data(); }
    uint8_t* keyManifestData() noexcept { return keyManifest_.data(); }
    uint8_t* nativeImportsData() noexcept { return nativeImportsTable_.data(); }

private:
    ExecutionTier tier_;
    std::string entrySymbol_;

    std::unique_ptr<BrassJitProgram> jitProgram_;
    std::unique_ptr<brass::Module> mirModule_;
    std::unique_ptr<brass::FastInterpreter> fastInterpreter_;
    std::unique_ptr<brass::codegen::BaselineJitCompiler> baselineCompiler_;
    std::vector<brass::codegen::BaselineCompiledFunction> compiledFunctions_;
    std::unordered_map<std::string, size_t> functionIndexMap_;
    brass::ModuleStackMap moduleStackMap_;

    uint64_t moduleEnv_ = 0;
    std::vector<uint32_t> keyMap_;
    std::vector<uint64_t> templateCells_;
    std::vector<uint64_t> globalCache_;
    std::vector<uint8_t> icTable_;
    std::vector<uint64_t> methodIcSites_;
    std::vector<uint8_t> keyManifest_;
    std::vector<uint8_t> nativeImportsTable_;

    std::unordered_map<std::string, void*> symbolAddressMap_;
    void* entryPoint_ = nullptr;
};

class BrassTieredEngine {
public:
    explicit BrassTieredEngine(const TieredEngineConfig& config = {});
    ~BrassTieredEngine();

    const TieredEngineConfig& config() const noexcept { return config_; }
    void setConfig(const TieredEngineConfig& config) { config_ = config; }

    std::unique_ptr<BrassTieredProgram> compile(const il::Module& module,
                                                DiagnosticSink& diags);

private:
    TieredEngineConfig config_;
};

}  // namespace bronze
