#pragma once

// The inline `new` fast path: for a constructor the runtime has vetted as
// plain (FunctionHeader::construct_vetted — not bound, not a primitive
// wrapper, prototype pair in place), generated code bump-allocates the
// instance from the runtime's inline-allocation window, wires its shape, and
// calls the constructor's code directly — bronze_construct's committed
// ordinary path, inlined exactly. Every miss on any guard is one branch into
// the helper, which owns every slow case and refills the window.

#include <cstdint>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include "codegen-llvm/llvm_abi.h"

namespace bronze::il {
struct Function;
}

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
                                 llvm::Value* argv, llvm::Value* selfSlotAddr,
                                 llvm::Function* knownWrapper = nullptr,
                                 const il::Function* knownFunc = nullptr,
                                 llvm::Function* knownEntry = nullptr,
                                 llvm::ArrayRef<llvm::Value*> directArgs = {});

// Emits the inline bump-pointer allocation for plain `{}` object literals,
// falling back to bronze_create_object on window miss.
llvm::Value* emitCreateObjectInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                     const AbiGlobals& globals);

// The same for an array literal of `length` elements: the ArrayHeader and its
// elements block (capacity max(length, the runtime's floor), HOLE-filled) are
// two adjacent bump allocations from the window, laid out as
// bronze_create_array lays them out; the helper is the window-miss edge. The
// caller keeps literals small — a `[x, z]` tuple, a `[a, b, c]` triple — so
// the HOLE fill stays a handful of stores.
llvm::Value* emitCreateArrayInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   const AbiGlobals& globals, uint32_t length);

}  // namespace bronze::codegen_llvm
