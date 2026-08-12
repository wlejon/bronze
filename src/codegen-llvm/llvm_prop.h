#pragma once

// Property reads and writes in generated code, including the inlined
// inline-cache check generated code performs itself.

#include <cstdint>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits a property read and returns its i64 (NaN-boxed) result.
//
// `monomorphic` is what inference proved about the site and it selects the
// FORM, never the semantics: false emits the plain helper call, true emits the
// guarded fast path with the same call as its slow arm. Both compute the same
// thing.
//
// IMPORTANT: when `monomorphic` is true this splits the current basic block,
// so `builder.GetInsertBlock()` differs on return. A caller that remembers
// the block it was building — to name a phi predecessor, say — must re-read
// it afterwards.
llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                         llvm::GlobalVariable* icTable, llvm::Value* objBits, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic);

// Property writes are always the helper call. A write can transition the
// shape and grow the out-of-line overflow block, so its interesting half is
// a miss, and a miss is a call whichever way the check is placed.
void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::GlobalVariable* icTable,
                 llvm::Value* objBits, uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex,
                 bool strict);

}  // namespace bronze::codegen_llvm
