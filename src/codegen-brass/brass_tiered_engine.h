#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <brass/interpreter/value.hpp>
#include <brass/gc/stack_map.hpp>

namespace brass {
class Module;
}
namespace brass::runtime {
class FunctionDispatchTable;
}

namespace bronze {

namespace il {
struct Module;
}
class DiagnosticSink;
class BrassJitProgram;
class TieredProgramImage;

// How a program runs. Every tier but Tier2_Optimized is the tiered pipeline
// (brass MultiTierPipeline) running the program's MIR: it starts in the fast
// interpreter, and the tier caps how far a function may be compiled.
enum class ExecutionTier : uint8_t {
    Tier0_Interpreter = 0, // the pipeline, never compiling: every function interpreted
    Tier1_Baseline = 1,    // the pipeline, every function baseline-compiled before the program runs
    Tier2_Optimized = 2,   // the whole program optimized ahead of running (ModuleCompiler -> JitExecutionEngine)
    Auto = 3               // the pipeline, tiering hot functions up to baseline and then to optimized
                           // code, which a background compiler builds and installs while the program runs
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
};

class BrassTieredProgram {
public:
    ~BrassTieredProgram();

    BrassTieredProgram(const BrassTieredProgram&) = delete;
    BrassTieredProgram& operator=(const BrassTieredProgram&) = delete;
    // Not movable: engines and brass handles hold pointers into this
    // program's data image and dispatch table.
    BrassTieredProgram(BrassTieredProgram&&) = delete;
    BrassTieredProgram& operator=(BrassTieredProgram&&) = delete;

    ExecutionTier tier() const noexcept { return tier_; }
    std::string_view entrySymbol() const noexcept { return entrySymbol_; }

    // A data symbol of the program (its import table, say), or a function's
    // pointer.
    void* symbolAddress(std::string_view name) const;

    // Runs the entry on the calling thread and returns its result bits.
    brass::RuntimeValue run();
    brass::RuntimeValue invoke(std::string_view fnName,
                               const std::vector<brass::RuntimeValue>& args = {});

    // This program's runtime state (function handles, tiering feedback,
    // MultiTierPipeline, background compiler). Owned per program so two
    // live programs never share handles or counters.
    brass::runtime::FunctionDispatchTable& dispatchTable() const noexcept { return *dispatchTable_; }
    brass::Module* mirModule() const noexcept { return mirModule_.get(); }
    BrassJitProgram* jitProgram() const noexcept { return jitProgram_.get(); }
    const brass::ModuleStackMap* stackMaps() const noexcept;

    // Drops the optimizations queued for this program and waits out the one
    // being compiled; later tier-ups queue again. For a host about to exit.
    void stopBackgroundCompilation();

private:
    friend class BrassTieredEngine;
    BrassTieredProgram(ExecutionTier tier, std::string entrySymbol);
    void activateStackMaps() const;

    ExecutionTier tier_;
    std::string entrySymbol_;

    // Tier2_Optimized only.
    std::unique_ptr<BrassJitProgram> jitProgram_;

    // The pipeline tiers. Destroyed in reverse: the dispatch table first
    // (which stops this program's background compiler, whose installs the
    // image observes), then the MIR module, then the image the code refers to.
    std::unique_ptr<TieredProgramImage> image_;
    std::unique_ptr<brass::Module> mirModule_;
    std::unique_ptr<brass::runtime::FunctionDispatchTable> dispatchTable_;
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
