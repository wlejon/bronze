#pragma once

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an inlined MethodCall with IC check in generated code:
// Fast path:
//   - TLS flag method_call_ic_enabled != 0
//   - Receiver is Object (TAG_OBJECT)
//   - Receiver is plain Object (flags == BRONZE_ABI_OBJ_FLAGS_PLAIN)
//   - Receiver shape == cached_shape (icEntry[0])
//   -> Loads cached_fn_code (icEntry[1]) and cached_arity (icEntry[2])
//   -> Direct call cached_fn_code(BRONZE_ABI_UNDEFINED_BITS, thisVal, argc, argv)
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
