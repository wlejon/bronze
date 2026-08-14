#pragma once

// Captured-variable access in generated code. An environment record is a
// fixed layout the collector already scans generically (runtime/env.h), the
// depth and the slot index are compile-time constants, and the operation is a
// handful of loads — so a closure variable read in a loop costs loads, not a
// helper call per iteration. Every guard failure — wrong tag, wrong kind, a
// chain shorter than the depth, a slot past the record — falls through to the
// helper, which owns the fatal that names the lowering bug, and the TDZ hit
// falls through to bronze_env_get_tdz, which owns the ReferenceError.

#include <cstdint>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an environment slot read and returns its i64 (NaN-boxed) result.
// With `tdz` set, a slot still holding the uninitialized marker takes the
// helper path, which raises the ReferenceError `keyIndex` names.
llvm::Value* emitEnvGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                        uint32_t depth, uint32_t index, bool tdz, uint32_t keyIndex);

// Emits an environment slot write.
void emitEnvSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                uint32_t depth, uint32_t index, llvm::Value* valBits);

}  // namespace bronze::codegen_llvm
