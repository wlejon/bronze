#pragma once

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an inlined MethodCall with IC check in generated code:
// Guards (any failure -> slow path):
//   - TLS flag method_call_ic_enabled != 0
//   - Receiver is Object (TAG_OBJECT)
//   - Receiver is plain Object (flags == BRONZE_ABI_OBJ_FLAGS_PLAIN)
//   - Receiver shape == cached_shape (icEntry[0])
// Hit: word 2's high half selects the form (bronze_abi.h's METHOD-CALL site
// contract): DIRECT calls the cached code with the cached env and arity;
// SLOT loads the receiver's own slot the entry names, verifies a Function is
// there now, and calls its current code/env/arity — the callee itself is
// never cached, which is what keeps per-instance closures and host functions
// correct on shared shapes.
// Slow path:
//   -> abi.bronze_call_method(thisVal, emitKeyId(...), argc, argv, icEntry)
llvm::Value* emitMethodCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const AbiGlobals& globals, const ModuleTables& tables,
                                  llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                  uint32_t argc, llvm::Value* argv);

// Emits MethodCallSpread fallback / helper:
llvm::Value* emitMethodCallSpreadInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                        const AbiGlobals& globals, const ModuleTables& tables,
                                        llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                        llvm::Value* argsArr);

}  // namespace bronze::codegen_llvm
