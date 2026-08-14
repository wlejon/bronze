#pragma once

// Inline reads of the two rooted runtime caches (rt_state.cpp): the
// provided-globals cache and the function-singleton slot table. Both hold
// heap Values the collector forwards IN PLACE through registered root
// sources, so generated code loads through the live published pointer and
// always sees current bits — the property that makes an inline read of a
// moving-heap cache sound at all. Every miss falls back to the helper, which
// owns filling and every diagnostic.

#include <cstdint>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// `global.get key`: a cached, non-undefined cell IS the helper's committed
// fast path (bronze_global_get caches builtins only, and never a host or
// globalThis fallthrough — those keep their scan-per-read by never being
// cached, so this path can never serve them stale).
llvm::Value* emitGlobalGetCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                 const AbiGlobals& globals, uint32_t keyIndex);

// A mention of a top-level function declaration: the slot entry answers only
// when its code word matches this mention's own function pointer, so a slot
// collision (two programs in one embed process) refills instead of handing
// out the wrong identity.
llvm::Value* emitFunctionSingletonCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                         const AbiGlobals& globals, llvm::Function* wrapper,
                                         uint32_t arity, uint32_t length, uint32_t nameKey,
                                         uint32_t slot);

}  // namespace bronze::codegen_llvm
