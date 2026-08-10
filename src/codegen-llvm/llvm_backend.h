#pragma once
#include "codegen/backend.h"

namespace bronze {

class LLVMBackend : public Backend {
public:
    LLVMBackend() = default;
    ~LLVMBackend() override = default;

    const char* name() const override { return "llvm"; }

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;
};

}  // namespace bronze
