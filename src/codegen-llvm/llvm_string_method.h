#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits inline computation of string.charCodeAt(index).
// Assumes receiver has already been verified to be a String.
// Checks index is a number and an exact integer.
// In-bounds returns the code unit as a boxed double; out-of-bounds returns canonical NaN.
// If index is not a number or not an exact integer, branches to `failBb`.
llvm::Value* emitStringCharCodeAtCompute(llvm::IRBuilder<>& builder, llvm::Value* strVal,
                                        llvm::Value* idxVal, llvm::BasicBlock* failBb);

// Direct method-call fast path for `s.charCodeAt(i)`.
// Intercepts before emitArgv in emitMethodCall.
// Guards:
//   1. IC enabled
//   2. Receiver is TAG_STRING
//   3. IC entry word 0 has primitive string kind
//   4. Holder object shape matches latchedShape
//   5. Holder slot function has code == abi.bronze_string_char_code_at
//   6. Argument is a number and exact integer
// On hit, computes the character code inline without allocating argv.
// On miss, invokes `missEmit()` to build argv and fall back to the slow path.
llvm::Value* emitMethodCallStringCharCodeAtDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, uint32_t argc,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::function_ref<llvm::Value*()> missEmit);

}  // namespace bronze::codegen_llvm
