#pragma once

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an inlined DynamicCall fast path in generated code:
// Guard:
//   - Callee is an Object-tagged value (`(callee >> 48) == TAG_OBJECT`)
//   - Callee carries HeapKind::Function flags (`BRONZE_ABI_OBJ_FLAGS_FUNCTION`)
//   - Function arity is <= call site argc (`fn->arity <= argc`)
//   - Feature is enabled (`bronze_inline_call_enabled != 0`)
// Fast path:
//   Loads `fn->env_record` and `fn->code`, and invokes `code(env, thisVal, argc, argv)`
//   directly in generated LLVM IR via indirect function pointer call.
// Fallback:
//   Calls `bronze_dynamic_call(callee, thisVal, argc, argv)` on any miss.
llvm::Value* emitDynamicCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   const AbiGlobals& globals, llvm::Value* callee,
                                   llvm::Value* thisVal, uint32_t argc, llvm::Value* argv);

}  // namespace bronze::codegen_llvm
