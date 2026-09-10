#pragma once
#include <string>
#include <utility>
#include <vector>
#include "codegen/backend.h"

namespace bronze {

class BrassBackend : public Backend {
public:
    BrassBackend() = default;
    ~BrassBackend() override = default;

    const char* name() const override { return "brass"; }
    void setEntrySymbol(std::string symbol) { entrySymbol_ = std::move(symbol); }
    void setSharedRuntime(bool on) { sharedRuntime_ = on; }
    void setHostGlobals(std::vector<std::string> names) { hostGlobals_ = std::move(names); }
    void setEmittedPathsOut(std::vector<std::string>* out) { emittedPathsOut_ = out; }

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    std::string entrySymbol_ = "bronze_main";
    bool sharedRuntime_ = false;
    std::vector<std::string> hostGlobals_;
    std::vector<std::string>* emittedPathsOut_ = nullptr;
};

}  // namespace bronze
