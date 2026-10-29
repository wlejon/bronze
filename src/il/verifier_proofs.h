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
//   5. Typed entries (`Function::typedOrigin`, `Instruction::typedTarget`): an
//      entry is a plain method body differing from its origin only in unboxed
//      f64/bool source parameters, and a typed edge's operands are exactly the
//      entry's parameter types (verifier_typed_entry.cpp).
bool verifyTypedEntries(const Module& module, DiagnosticSink& diags);
// Whether `def` can never produce an Array or a typed view — the receivers
// rule 4 refuses. Exposed so the passes that produce those ops refuse the same
// receivers rather than keeping a second list.
bool isKnownNonArrayProducer(const Instruction& def);
bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags);
bool verifyModuleProofs(const Module& module, DiagnosticSink& diags);

}  // namespace bronze::il
