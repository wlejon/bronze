#pragma once
#include <string>

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

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    std::string entrySymbol_ = "bronze_main";
};

}  // namespace bronze
