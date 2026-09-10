#pragma once

#include <cstdint>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Direct method-call fast path for `arr.push(arg)`.
// Intercepts before emitArgv in emitMethodCall.
// Guards:
//   1. Receiver is TAG_OBJECT
//   2. Receiver flags == BRONZE_ABI_OBJ_FLAGS_ARRAY
//   3. Receiver properties is not TAG_OBJECT (no own-property shadowing)
//   4. Capacity: head + length < capacity
// On hit, stores arg directly into array element buffer and increments length.
// On miss, invokes `missEmit()` to fall back to the slow IC path.
llvm::Value* emitMethodCallArrayPushDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, llvm::Value* argVal,
    llvm::function_ref<llvm::Value*()> missEmit);

// Direct method-call fast path for `arr.pop()`.
// Intercepts before emitArgv in emitMethodCall.
// Guards:
//   1. Receiver is TAG_OBJECT
//   2. Receiver flags == BRONZE_ABI_OBJ_FLAGS_ARRAY
//   3. Receiver properties is not TAG_OBJECT
// On length == 0: returns undefined bits.
// On hit: loads last element, stores Hole into slot, decrements length.
// If length becomes 0, resets head_offset to 0.
// On miss, invokes `missEmit()`.
llvm::Value* emitMethodCallArrayPopDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex,
    llvm::function_ref<llvm::Value*()> missEmit);

// Direct method-call fast path for `arr.shift()`.
// Intercepts before emitArgv in emitMethodCall.
// Guards:
//   1. Receiver is TAG_OBJECT
//   2. Receiver flags == BRONZE_ABI_OBJ_FLAGS_ARRAY
//   3. Receiver properties is not TAG_OBJECT
// On length == 0: returns undefined bits.
// On hit: loads element at head_offset, stores Hole into slot, increments head_offset, decrements length.
// If length becomes 0, resets head_offset to 0.
// On miss, invokes `missEmit()`.
llvm::Value* emitMethodCallArrayShiftDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex,
    llvm::function_ref<llvm::Value*()> missEmit);

}  // namespace bronze::codegen_llvm
