#pragma once

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

bool verifyFunction(const Function& fn, DiagnosticSink& diags);
bool verify(const Module& module, DiagnosticSink& diags);

}  // namespace bronze::il
