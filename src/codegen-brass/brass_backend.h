#pragma once
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>
#include "codegen/backend.h"

namespace brass {
struct PassPipelineOptions;
}
namespace il2mir {
struct TranslatorOptions;
struct SlowArmedBits;
}

namespace bronze::codegen {
struct ColdCompanionInputs;
}

namespace bronze {

constexpr brass::Type brassTypeOf(il::Type t) noexcept {
    switch (t) {
        case il::Type::Void: return brass::Type::void_type();
        case il::Type::Bool: return brass::Type::i8();
        case il::Type::I32: return brass::Type::i32();
        case il::Type::F64: return brass::Type::f64();
        case il::Type::Str: return brass::Type::ptr();
        case il::Type::Dynamic: return brass::Type::i64();
        case il::Type::Ptr: return brass::Type::i64();
    }
    return brass::Type::i64();
}

class BrassBackend : public Backend {
public:
    BrassBackend() = default;
    ~BrassBackend() override = default;

    const char* name() const override { return "brass"; }
    void setEntrySymbol(std::string symbol) { entrySymbol_ = std::move(symbol); }
    void setHostGlobals(std::vector<std::string> names) { hostGlobals_ = std::move(names); }
    void setEmittedPathsOut(std::vector<std::string>* out) { emittedPathsOut_ = out; }
    // Whether the whole module is optimized as it is built. Off: no MIR
    // optimizer, no scheduling, no layout — the code the translator emits,
    // selected and allocated as it stands; what a tiered program is built
    // from (its functions are optimized one at a time as they tier up,
    // tierUpPasses). Same semantics either way; only how long the compile
    // takes and how fast the result runs differ.
    void setOptimize(bool on) { optimize_ = on; }
    bool optimize() const { return optimize_; }
    // The passes the optimizer runs over a program, for a tiered program
    // to run over each function it tiers up (MultiTierPipeline::
    // set_tier2_passes) instead of over the whole module before it starts.
    brass::PassPipelineOptions tierUpPasses() const;
    // The machine the object is for: this one unless a build says otherwise
    // (`--target`). Only the object and the module written from it change;
    // a program needs the target's own host binary, so it stays native.
    void setTarget(const brass::Target& target) { target_ = target; }
    const brass::Target& target() const { return target_; }
    // What a Mach-O object's LC_BUILD_VERSION says (`--target
    // aarch64-ios16.0`): the platform and minimum OS; zero fields are
    // brass's defaults (MACOSX_DEPLOYMENT_TARGET, then the deployment target
    // brass was built for).
    void setMachOBuildVersion(const brass::object::MachOBuildVersion& v) { machoVersion_ = v; }
    void setEmitDebugInfo(bool on) { emitDebugInfo_ = on; }
    bool emitDebugInfo() const { return emitDebugInfo_; }
    // The module buildMirModule lowers runs in this process, under brass's
    // tiered pipeline (il2mir::TranslatorOptions::in_process): never an
    // object file, and never harvested.
    void setInProcess(bool on) { inProcess_ = on; }
    // Whether the module init registers function source slices
    // (`__bronze_source_text_N` / `__bronze_source_entries_N`). Only an
    // object file (buildObjectFile) defines those tables; a MIR module
    // compiled by another engine must leave the registration out, since the
    // data symbols would not resolve.
    void setRegisterFnSources(bool on) { registerFnSources_ = on; }
    // Whether the module's writable tables are addressed per THREAD
    // (bronze_abi.h, `bronze_module_instance`): one contiguous instance run
    // in the object's data, or in the data image (buildDataImage) of a
    // program the tiered pipeline runs, a delta per thread.
    void setPerThreadModuleData(bool on) { perThreadModuleData_ = on; }
    // Whether buildMirModule runs brass's verifier over what it lowered
    // (il2mir::TranslatorOptions::verify_lowered_module).
    void setVerifyLowered(bool on) { verifyLowered_ = on; }
    // Whether buildMirModule leaves function bodies to be built on first
    // need (il2mir::TranslatorOptions::lazy_bodies). Only for a module run
    // unoptimized by brass's tiered pipeline.
    void setLazyBodies(bool on) { lazyBodies_ = on; }
    // The image is code for the life of the process (an executable, a shared
    // module, an AOT module image, a JIT program the process retains), so a
    // caller may enter its compiled functions directly (bronze_abi.h,
    // BRONZE_ABI_FN_FLAG_DIRECT_CALL; runtime rtEntersWithoutScope): its
    // functions carry the flag where il::functionEntersDirectly allows. Off
    // for JIT code a program may free (an eval, a `new Function`): what keeps
    // such code mapped while it runs is the dynamic-module token rtEnterJs
    // pushes, which a direct entry skips.
    void setDirectJsCalls(bool on) { directCallees_ = on; }
    // Threads the per-function work of a build may use: brass's MIR
    // optimizer (brass::PipelineRunOptions) and its code generation
    // (brass::object::ModuleCompiler::set_threads). 0, the default, is the
    // machine's hardware threads; 1 runs it all on the calling thread. The
    // object's bytes are the same at any count.
    void setBuildThreads(size_t threads) { buildThreads_ = threads; }
    size_t buildThreads() const { return buildThreads_; }
    // A build made to be harvested whose harvest a host names at load
    // (eval::buildModule: an app's AOT module, whose profile is written
    // beside it) rather than through il::Module::harvestOutPath: it keeps
    // the same evidence a `--harvest-out` build does (entry counts, shape
    // and callee turns, allocation mementos), which a build on its harvest
    // needs for heat, never-entered blocks and pretenuring. Nothing changes
    // for an image spending a harvest, which counts nothing. In process
    // (setInProcess) it is the first run of a host's app, compiled to be
    // harvested (eval::EvalOptions::harvestOutPath): every tier runs the
    // counting code, since all of them run the MIR it lowers into.
    void setHarvestEvidence(bool on) { harvestEvidence_ = on; }
    // The last buildMirModule's armed bitmap (il2mir::SlowArmedBits), null
    // when its code counts nothing: an in-process image attaches its own
    // words to it once loaded, since lazy bodies arm their sites later.
    const std::shared_ptr<il2mir::SlowArmedBits>& slowArmed() const { return slowArmed_; }
    // Builds the COMPANION of an image (codegen-brass/cold_companion.h) from
    // `inputs`: buildMirModule lowers only the interpreted bodies of the
    // stripped module it is given, against the image's tables.
    void setColdCompanion(const codegen::ColdCompanionInputs* inputs) { companion_ = inputs; }
    const std::string& entrySymbol() const { return entrySymbol_; }
    bool perThreadModuleData() const { return perThreadModuleData_; }

    std::unique_ptr<brass::Module> buildMirModule(const il::Module& module,
                                                  DiagnosticSink& diags,
                                                  std::vector<uint32_t>* globalReadKeysOut = nullptr);

    std::optional<brass::object::ObjectFile> buildObjectFile(const il::Module& module,
                                                            DiagnosticSink& diags);

    // The bronze-owned sections alone (brass_backend_sections.h), for a MIR
    // module that another engine runs (the tiered pipeline): the tables,
    // descriptors and data cells an object file carries, and no code. Its
    // references to the module's functions stay undefined, for the loader to
    // bind to wherever that engine keeps them. `globalCacheCount` is the
    // number of read keys buildMirModule reported.
    brass::object::ObjectFile buildDataImage(const il::Module& module, size_t globalCacheCount);

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    // The translator's optimization switches, `optimize` or not; lowering
    // itself reads none of them.
    void setOptimizationOptions(il2mir::TranslatorOptions& options, bool optimize) const;

    std::string entrySymbol_ = "bronze_main";
    bool optimize_ = true;
    bool emitDebugInfo_ = false;
    bool registerFnSources_ = true;
    bool perThreadModuleData_ = true;
    bool verifyLowered_ = true;
    bool lazyBodies_ = false;
    bool directCallees_ = false;
    bool harvestEvidence_ = false;
    size_t buildThreads_ = 0;
    brass::Target target_ = brass::Target::host();
    brass::object::MachOBuildVersion machoVersion_;
    std::vector<std::string> hostGlobals_;
    std::vector<std::string>* emittedPathsOut_ = nullptr;
    bool inProcess_ = false;
    // The slow sites the last buildMirModule's lowering armed with a counter
    // (il2mir::TranslatorOptions `slow_armed`), for buildObjectFile to lay out
    // before the counters; null when it counted none.
    std::shared_ptr<il2mir::SlowArmedBits> slowArmed_;
    const codegen::ColdCompanionInputs* companion_ = nullptr;
    // The image buildObjectFile is building: its interpreted functions
    // (il2mir::TranslatorOptions::interpreted), empty for none.
    std::vector<uint32_t> interpreted_;
};

// Per IL function, the name it is compiled under ("main" for the entry).
std::vector<std::string> computeUniqueFunctionNames(const il::Module& module);

}  // namespace bronze
