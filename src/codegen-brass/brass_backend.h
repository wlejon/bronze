#pragma once
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <brass/object/object_writer.hpp>
#include "codegen/backend.h"
#include "codegen-brass/brass_jit.h"

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
    void setSharedRuntime(bool on) { sharedRuntime_ = on; }
    void setHostGlobals(std::vector<std::string> names) { hostGlobals_ = std::move(names); }
    void setEmittedPathsOut(std::vector<std::string>* out) { emittedPathsOut_ = out; }
    void setPropagateExceptionsInEntry(bool val) { propagateExceptionsInEntry_ = val; }
    // Off is the baseline tier: no MIR optimizer, no scheduling, no layout —
    // the code the translator emits, selected and allocated as it stands.
    // Same semantics either way; only how long the compile takes and how
    // fast the result runs differ. BRONZE_NO_OPT=1 in the environment forces
    // it off for every backend in the process, so any pipeline (the CLI, the
    // oracle suite, a host's JIT) can be run in the baseline tier as a check.
    void setOptimize(bool on) { optimize_ = on; }
    bool optimize() const;
    // The machine the object is for: this one unless a build says otherwise
    // (`--target`). Only the object and the module written from it change;
    // a program needs the target's own host binary, so it stays native.
    void setTarget(const brass::Target& target) { target_ = target; }
    const brass::Target& target() const { return target_; }

    std::unique_ptr<brass::Module> buildMirModule(const il::Module& module,
                                                  DiagnosticSink& diags,
                                                  std::vector<uint32_t>* globalReadKeysOut = nullptr);

    std::optional<brass::object::ObjectFile> buildObjectFile(const il::Module& module,
                                                            DiagnosticSink& diags);

    std::unique_ptr<BrassJitProgram> compileToJit(const il::Module& module,
                                                  DiagnosticSink& diags);

    bool emitObject(const il::Module& module, const std::string& outputPath,
                    DiagnosticSink& diags) override;

private:
    std::string entrySymbol_ = "bronze_main";
    bool sharedRuntime_ = false;
    bool propagateExceptionsInEntry_ = false;
    bool optimize_ = true;
    brass::Target target_ = brass::Target::host();
    std::vector<std::string> hostGlobals_;
    std::vector<std::string>* emittedPathsOut_ = nullptr;
};

}  // namespace bronze
