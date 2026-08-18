#pragma once

// Dynamic-index element access in generated code: the inline fast paths for
// `o[i]` where `i` is a value rather than a constant key. The compile-time
// constant-key form lives with the property caches in llvm_prop.h; this is
// the loop form — `v[i]` over an Array or a float typed array — whose whole
// cost was a helper call per element until it was inlined.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits `o[i]` and returns its i64 (NaN-boxed) result. The inline path covers
// an in-bounds numeric index on an Array (hole answers undefined) and on a
// Float32/Float64 typed array; everything else — out of bounds, other element
// kinds, string or symbol keys, non-objects — falls through to
// bronze_elem_get, whose behavior this path mirrors exactly.
llvm::Value* emitElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                         llvm::Value* idxBits);

// Emits `o[i] = v`. The inline path covers an in-bounds numeric index on an
// Array with no named-properties side object, and a numeric value into a
// Float32/Float64 typed array — where an out-of-bounds index discards the
// write, as the spec and the helper both do. Everything else falls through to
// bronze_elem_set.
void emitElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                 llvm::Value* idxBits, llvm::Value* valBits, bool strict);

// Computes the element pointer for a typed array view.
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
llvm::Value* emitTypedElemGet(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                              llvm::Value* idxDbl, bool isF64);
void emitTypedElemSet(llvm::IRBuilder<>& builder, llvm::Value* objBits, llvm::Value* idxDbl,
                      llvm::Value* valDbl, bool isF64);

// Re-boxes a double value into a NaN-boxed 64-bit value.
llvm::Value* emitBoxDouble(llvm::IRBuilder<>& builder, llvm::Value* d);

}  // namespace bronze::codegen_llvm
