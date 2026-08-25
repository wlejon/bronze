#pragma once

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits an inlined MethodCall with IC check in generated code:
// Guards (any failure -> slow path):
//   - TLS flag method_call_ic_enabled != 0
//   - Receiver is Object (TAG_OBJECT)
//   - Receiver is plain Object (flags == BRONZE_ABI_OBJ_FLAGS_PLAIN)
//   - Receiver shape == cached_shape (icEntry[0])
// Hit: word 2's high half selects the form (bronze_abi.h's METHOD-CALL site
// contract): DIRECT calls the cached code with the cached env and arity;
// SLOT loads the receiver's own slot the entry names, verifies a Function is
// there now, and calls its current code/env/arity — the callee itself is
// never cached, which is what keeps per-instance closures and host functions
// correct on shared shapes.
// Slow path:
//   -> abi.bronze_call_method(thisVal, emitKeyId(...), argc, argv, icEntry)
llvm::Value* emitMethodCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const AbiGlobals& globals, const ModuleTables& tables,
                                  llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                  uint32_t argc, llvm::Value* argv);

// Emits MethodCallSpread fallback / helper:
llvm::Value* emitMethodCallSpreadInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                        const AbiGlobals& globals, const ModuleTables& tables,
                                        llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                        llvm::Value* argsArr);

// The guard in front of a DIRECT method-call edge (il.h, `directTarget`).
//
// It asks exactly what the inline cache above already asks — the site is
// enabled, the receiver is a plain object, its shape is the one the entry was
// filled for, and the entry is in its DIRECT form — and then one question more:
// is the code pointer the cache resolved this key to `wrapper`? The cache
// filled those words by performing the real lookup, so a yes means the callee
// really is that IL function, whatever the compiler guessed and whatever the
// prototype chain has since been made to hold. A no is the ordinary indirect
// dispatch, which is what the miss block goes on to emit.
//
// Nothing here is a claim ABOUT the receiver, which is why a guessed receiver
// class is admissible: the compiler proposes a callee and the runtime's own
// cache disposes.
//
// The builder is left positioned in the MISS block. `hit` is empty and
// unterminated; the caller emits the typed call into it. `env` is the cached
// environment word, loaded in the hit block, or null when `needsEnv` is false —
// a function object with this code pointer is not necessarily the one the
// module created (two evaluations of one class body make two), so the env has
// to come from the entry rather than be assumed.
struct MethodDirectGuard {
    llvm::BasicBlock* hit = nullptr;
    llvm::BasicBlock* miss = nullptr;
    llvm::Value* env = nullptr;
};
MethodDirectGuard emitMethodDirectGuard(llvm::IRBuilder<>& builder, const AbiGlobals& globals,
                                        const ModuleTables& tables, llvm::Value* thisVal,
                                        uint32_t icIndex, llvm::Function* wrapper, bool needsEnv);

}  // namespace bronze::codegen_llvm
