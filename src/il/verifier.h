#pragma once

#include <cstddef>

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

bool verifyFunction(const Function& fn, DiagnosticSink& diags);
// The whole module; its bodies are checked on up to `threads` threads (0: the
// machine's), the answer and its diagnostics the same at any count.
bool verify(const Module& module, DiagnosticSink& diags, size_t threads = 0);

}  // namespace bronze::il
