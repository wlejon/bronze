#pragma once

// Inline reads of the two caches a compiled module owns (llvm_abi.h
// ModuleTables): the provided-globals cache and the function-singleton slot
// table. Both hold heap Values the collector forwards IN PLACE through the
// spans the module registers at init, so generated code reads a cell at a
// compile-time constant address and always sees current bits — the property
// that makes an inline read of a moving-heap cache sound at all. Every miss
// falls back to the helper, which owns filling and every diagnostic.
//
// The tables being the MODULE's is what makes the addresses constant: the
// counts are compile-time facts, so neither path loads a table pointer and
// neither bounds-checks an index the compiler already knows is in range.

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
                                 const ModuleTables& tables, uint32_t keyIndex);

// A mention of a top-level function declaration: the slot entry answers only
// when its code word matches this mention's own function pointer, so a stale or
// never-filled entry refills instead of handing out the wrong identity.
llvm::Value* emitFunctionSingletonCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                         const ModuleTables& tables, llvm::Function* wrapper,
                                         uint32_t arity, uint32_t length, uint32_t nameKey,
                                         uint32_t fnFlags, uint32_t slot);

}  // namespace bronze::codegen_llvm
