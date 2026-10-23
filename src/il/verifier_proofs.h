#pragma once

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

// Proof-certificate verification for bronze IL:
//   1. `Op::Unbox` (`rawUnbox`, `nullishUnbox`): mutual exclusion, operand/result
//      types, positive proof origin (dominating `IsNumber`, dominating `PinGuard`,
//      `Box(F64|I32)`, audited `PropGet`/`ElemGet`, consistent `EnvGet`, or
//      parameter/phi), and rejection of known non-Number origins or false-edge
//      `IsNumber` paths.
//   2. `Op::PinGuard`: operand count/type, valid `PinBarrier` kind, and
//      `keyIndex < module.keyConstants.size()`.
//   3. `Param::pinned` and `Function::returnPinned`: `Type::F64` requirement and
//      valid `pinKeyIndex` / `returnPinKeyIndex`.
//   4. `Op::ElemGetTyped`, `Op::ElemSetTyped`, and `Op::IsDenseArray`: receiver
//      not a known non-array producer, numeric index/value types, and valid
//      element kind (`0..11` or `kElemKindPlainArrayF64`).
bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags);
bool verifyModuleProofs(const Module& module, DiagnosticSink& diags);

}  // namespace bronze::il
