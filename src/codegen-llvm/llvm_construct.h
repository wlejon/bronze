#pragma once

// The inline `new` fast path: for a constructor the runtime has vetted as
// plain (FunctionHeader::construct_vetted — not bound, not a primitive
// wrapper, prototype pair in place), generated code bump-allocates the
// instance from the runtime's inline-allocation window, wires its shape, and
// calls the constructor's code directly — bronze_construct's committed
// ordinary path, inlined exactly. Every miss on any guard is one branch into
// the helper, which owns every slow case and refills the window.

#include <cstdint>

#include <llvm/IR/IRBuilder.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::codegen_llvm {

// Emits the guarded fast path + helper fallback for one `new` site and
// returns the constructed value. `argv` is the frame's argv region (already
// filled, so the arguments are rooted across the callee) or a null pointer
// when argc is 0. `selfSlotAddr` is a dedicated GC root slot in this
// function's frame: the fresh instance is stored there before the
// constructor runs and re-read after, because the constructor's own
// allocations may move it. Splits the current block.
//
// The caller must not emit this in a module containing `new.target`
// (Op::GetNewTarget): the inline path does not push the helper's
// NewTargetScope, which is sound precisely because bronze_get_new_target is
// that scope's only observer.
llvm::Value* emitConstructInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                 const AbiGlobals& globals, llvm::Value* ctor, uint32_t argc,
                                 llvm::Value* argv, llvm::Value* selfSlotAddr);

// Emits the inline bump-pointer allocation for plain `{}` object literals,
// falling back to bronze_create_object on window miss.
llvm::Value* emitCreateObjectInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                    const AbiGlobals& globals);

}  // namespace bronze::codegen_llvm
