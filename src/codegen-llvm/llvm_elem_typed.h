#pragma once

// Typed-array element access in generated code: the inline fast paths for
// typed array views and proven typed-element operations (Float64Array,
// Float32Array, etc.). Decomposed from llvm_elem.cpp along the typed-array seam.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Computes the typed array data base pointer (inline data or external store + byte offset).
llvm::Value* emitTypedArrayBasePtr(llvm::IRBuilder<>& builder, llvm::Value* hdr);

// Computes the address of element `idx32` given an already computed data base pointer.
llvm::Value* emitTypedArrayElemPtrFromBase(llvm::IRBuilder<>& builder, llvm::Value* dataPtr,
                                           llvm::Value* idx32, uint32_t elemSize);

// The address of element `idx32` of a typed-array view, computed from
// the view's buffer Value on every access — never cached across allocations,
// per the GC rule the header documents. The builder must already be in the
// view's arm.
llvm::Value* emitTypedArrayElemPtr(llvm::IRBuilder<>& builder, llvm::Value* hdr,
                                   llvm::Value* idx32, uint32_t elemSize);

// The PROVEN forms — elem.get.typed / elem.set.typed, receiver proved a
// Float64Array (isF64) or Float32Array view by inference. No receiver
// guards, no boxing, no fallback edge: the index is a double in SSA, the
// result/value is a double in SSA, and the only control flow is the
// language's own index-validity rule (integral, in range, inside the view),
// whose failure is NaN for the get and a discarded write for the set —
// mirroring what bronze_elem_get / _set answer for a number index on this
// receiver. Neither can call anything, which is what keeps a loop of them
// free of safepoints.
llvm::Value* emitTypedElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                              llvm::Value* idxDbl, uint32_t elemKind);
void emitTypedElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                      llvm::Value* idxDbl, llvm::Value* valDbl, uint32_t elemKind);

// Proves whether a double SSA value is guaranteed to be an integral value in [0, 2^32-1].
bool isProvenIntegralDouble(llvm::Value* v, int depth = 3);

// Unwraps a boxed double Value (from Op::Box or canonicalizeNumeric) back to its double SSA value.
llvm::Value* unwrapBoxedDouble(llvm::Value* val);

// Proves whether a 64-bit NaN-boxed Value is guaranteed to be a valid Number (<= BRONZE_ABI_NUMBER_MAX_BITS).
bool isProvenNumberValue(llvm::Value* val, int depth = 3);

}  // namespace bronze::codegen_llvm
