#pragma once

// Dynamic-index element access in generated code: the inline fast paths for
// `o[i]` where `i` is a value rather than a constant key. The compile-time
// constant-key form lives with the property caches in llvm_prop.h; this is
// the loop form — `v[i]` over an Array or a float typed array — whose whole
// cost was a helper call per element until it was inlined.

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_elem_typed.h"

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

struct ElemCacheHit {
    // The hit value, as a PHI over the entry's two answers: a slot off the
    // receiver, or `undefined` for a proven-absent pair.
    llvm::Value* value{nullptr};
    // The block that PHI lives in, and the one the caller's own join must name
    // as its predecessor.
    llvm::BasicBlock* hitBb{nullptr};
};

// The committed hit of the computed-read cache, at the site
// (llvm_elem_cache.cpp). Emitted into the CURRENT block, which must be the one
// every fast-path refusal above already branches to: on a hit it branches to
// `doneBb`, and on any refusal of its own to `slowBb`, so it is a filter in
// front of `bronze_elem_get` rather than a fourth receiver arm. NUMBER and
// BOOLEAN keys only — the file says why a string key cannot be confirmed
// without a loop.
ElemCacheHit emitElemCacheGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                              llvm::Value* keyBits, llvm::BasicBlock* slowBb,
                              llvm::BasicBlock* doneBb);

// The committed hit of the computed-write cache, at the site
// (llvm_elem_cache.cpp). Emitted into the CURRENT block: on a hit it branches
// to `doneBb`, and on any refusal of its own to `slowBb`.
void emitElemCacheSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                      llvm::Value* keyBits, llvm::Value* valBits, llvm::BasicBlock* slowBb,
                      llvm::BasicBlock* doneBb);

// Re-boxes a double value into a NaN-boxed 64-bit value.
llvm::Value* emitBoxDouble(llvm::IRBuilder<>& builder, llvm::Value* d);

}  // namespace bronze::codegen_llvm
