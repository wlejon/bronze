#pragma once

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags);
bool verifyModuleProofs(const Module& module, DiagnosticSink& diags);

}  // namespace bronze::il
