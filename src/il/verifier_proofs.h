#pragma once

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

// Proof-certificate verification for bronze IL:
//   1. `Op::PinGuard`: operand count/type, valid `PinBarrier` kind, and
//      `keyIndex < module.keyConstants.size()`.
//   2. `Param::pinned` and `Function::returnPinned`: `Type::F64` requirement and
//      valid `pinKeyIndex` / `returnPinKeyIndex`.
//   3. `Op::IsDenseArray`: receiver not a known non-array producer.
bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags);
bool verifyModuleProofs(const Module& module, DiagnosticSink& diags);

}  // namespace bronze::il
