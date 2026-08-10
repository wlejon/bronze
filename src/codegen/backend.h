#pragma once
#include <string>

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze {

// Backend interface. The LLVM implementation lives in src/codegen-llvm
// behind BRONZE_WITH_LLVM so the rest of the compiler builds, tests, and
// iterates without the heavy dependency. There is deliberately NO fallback
// backend: requesting codegen without a backend compiled in is a hard error
// at the CLI, never a silent no-op.
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;

    // Emits a native object file for the module. Returns false and reports
    // to the sink on failure.
    virtual bool emitObject(const il::Module& module, const std::string& outputPath,
                            DiagnosticSink& diags) = 0;
};

}  // namespace bronze
