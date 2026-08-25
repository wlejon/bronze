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

// Whether this build drops the ACCESS guards — the object tag, the Env brand
// and the slot-range test that `emitEnvSlotPtr` re-derives at every access.
// Off unless BRONZE_ELIDE_ENV_GUARDS=1; llvm_env.cpp has what they are, what
// licenses dropping them, and the measurement that kept it a flag. The TDZ
// test is NOT one of them and is never dropped: it is 9.1.1.1.6, not a
// tripwire.
bool envAccessGuardsElided();

// Emits an environment slot read and returns its i64 (NaN-boxed) result.
// With `tdz` set, a slot still holding the uninitialized marker takes the
// helper path, which raises the ReferenceError `keyIndex` names.
llvm::Value* emitEnvGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                        const ModuleTables& tables, llvm::Value* envBits, uint32_t depth,
                        uint32_t index, bool tdz, uint32_t keyIndex, bool elideGuards);

// Emits an environment slot write.
void emitEnvSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                uint32_t depth, uint32_t index, llvm::Value* valBits, bool elideGuards);

}  // namespace bronze::codegen_llvm
