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
                           // code, which a background compiler builds and installs while the program runs;
                           // the optimizer runs per function as it tiers up, and a hot loop moves into
                           // optimized code mid-loop (OSR)
};

std::string_view executionTierToString(ExecutionTier tier) noexcept;
std::optional<ExecutionTier> parseExecutionTier(std::string_view str) noexcept;

struct TieredEngineConfig {
    ExecutionTier tier = ExecutionTier::Auto;
    std::string entrySymbol = "main";
    std::vector<std::string> hostGlobals;
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

    // Runs the entry on the calling thread and returns its result bits. A
    // throw the program does not catch leaves as a C++
    // brass::runtime::BrassException (runtime/exception.h), whichever tier
    // was running the frame it came from.
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

    // Whether a run on another thread starts from fresh module data: true
    // for the pipeline tiers, whose writable tables are per thread; false
    // for Tier2_Optimized, whose tables are the image's own.
    bool perThreadData() const noexcept { return tier_ != ExecutionTier::Tier2_Optimized; }
    // The cell naming the module's per-thread data slot (bronze_abi.h,
    // `bronze_module_instance`); null for Tier2_Optimized.
    const uint64_t* moduleSlotCell() const;

    // Drops the optimizations queued for this program and waits out the one
    // being compiled; later tier-ups queue again. For a host about to exit.
    void stopBackgroundCompilation();

private:
    friend class BrassTieredEngine;
    BrassTieredProgram(ExecutionTier tier, std::string entrySymbol);
    void activateStackMaps() const;
    brass::RuntimeValue invokeUntranslated(std::string_view fnName,
                                           const std::vector<brass::RuntimeValue>& args);
    // Registers the calling thread's instance of the module's per-thread
    // data (bronze_module_instance), as the entry does on its way in.
    void enterThreadInstance() const;

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
