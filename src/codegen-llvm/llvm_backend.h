#pragma once
#include <string>
#include <vector>

#include "codegen/backend.h"

namespace bronze {

class LLVMBackend : public Backend {
public:
    LLVMBackend() = default;
    ~LLVMBackend() override = default;

    const char* name() const override { return "llvm"; }

    // The name the object exports its entry point under. It exists so a host
    // can link MORE THAN ONE compiled module into one image: the entry is one
    // of only two symbols an object defines, so distinct names here are what
    // keep two modules from colliding at link. The default is the name the two
    // program entries (src/rt/rt.cpp, embed_run.cpp) call.
    //
    // The object's ABI stamp is named after it for the same reason — see
    // `entrySymbol` in llvm_backend.cpp.
    void setEntrySymbol(std::string symbol) { entrySymbol_ = std::move(symbol); }

    // Compile for a runtime that will be a SHARED library. Two effects, both
    // Windows-only and both off by default (llvm_abi.h's RuntimeLinkage says
    // why the import half must be a mode rather than the default): the
    // registry's data symbols are reached through import slots, and the
    // module's three exported names are marked for export so a `/DLL` link
    // publishes exactly them.
    void setSharedRuntime(bool on) { sharedRuntime_ = on; }

    // The `--host-globals` manifest this module was compiled against, in the
    // order the manifest gave it. Emitted as the module's third exported
    // symbol; bronze_abi.h documents the layout and why a loader needs it.
    void setHostGlobals(std::vector<std::string> names) { hostGlobals_ = std::move(names); }

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    std::string entrySymbol_ = "bronze_main";
    bool sharedRuntime_ = false;
    std::vector<std::string> hostGlobals_;
};

}  // namespace bronze
