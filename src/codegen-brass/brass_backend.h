#pragma once
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>
#include "codegen/backend.h"
#include "codegen-brass/brass_jit.h"

namespace brass {
struct PassPipelineOptions;
}
namespace il2mir {
struct TranslatorOptions;
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
    void setPropagateExceptionsInEntry(bool val) { propagateExceptionsInEntry_ = val; }
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

    std::unique_ptr<BrassJitProgram> compileToJit(const il::Module& module,
                                                  DiagnosticSink& diags);

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    // The translator's optimization switches, `optimize` or not; lowering
    // itself reads none of them.
    void setOptimizationOptions(il2mir::TranslatorOptions& options, bool optimize) const;

    std::string entrySymbol_ = "bronze_main";
    bool propagateExceptionsInEntry_ = false;
    bool optimize_ = true;
    bool emitDebugInfo_ = false;
    bool registerFnSources_ = true;
    bool perThreadModuleData_ = true;
    brass::Target target_ = brass::Target::host();
    brass::object::MachOBuildVersion machoVersion_;
    std::vector<std::string> hostGlobals_;
    std::vector<std::string>* emittedPathsOut_ = nullptr;
};

}  // namespace bronze
