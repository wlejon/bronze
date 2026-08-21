#pragma once

#include "codegen-llvm/llvm_abi.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace bronze::codegen_llvm {

// The static-slot fast path, emitted IN FRONT of the ordinary inline-cache
// sequence rather than instead of it.
//
// In front, deliberately. A site whose class layout proved a slot is still a
// site that can be reached by an object of some other shape — a subclass, a
// plain literal, an instance whose constructor took a branch the layout did not
// model — and a design that replaced the cache would make every one of those a
// helper call where today it is a cache hit. What this adds to a miss is a load
// of a module global, one compare and one branch, most of which the following
// sequence's own tag and flags tests fold away; what it removes from a hit is
// the cache-entry load, the slot-word decode, the depth ladder and the
// inline/overflow test, leaving a compare and a load at a constant offset.
//
// The guard is the shape word, exactly as the inline cache's is, and the cell
// it compares against is published by `bronze_static_shape_publish` only after
// the runtime has checked that this key really is an own, data, correctly-slotted
// (and for a store, writable) property of that shape. So nothing upstream —
// the class layout, the receiver typing, the `this` binding — has to be sound
// for this to be correct. It has to be RIGHT to be fast, which is a different
// obligation and the one class_layout.cpp discharges.

struct StaticSlotGuard {
    // Where the caller continues emitting the ordinary sequence. Equal to the
    // block the caller was in when nothing was emitted.
    llvm::BasicBlock* missBb = nullptr;
    // The block the fast path produced its result in, or null when no guard
    // was emitted. The caller adds it to its result PHI (reads) or simply
    // branches it to `done` (writes).
    llvm::BasicBlock* hitBb = nullptr;
    // The loaded slot value; reads only.
    llvm::Value* value = nullptr;
};

// Emits the guard at the current insert point and leaves the builder in
// `missBb`. `slot` is `il::Instruction::kNoStaticSlot` to emit nothing.
//
// `store` is null for a read; non-null makes the hit block store it into the
// slot instead of loading from it.
StaticSlotGuard emitStaticSlotGuard(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                    llvm::Value* objBits, uint32_t slot, uint32_t cellIndex,
                                    llvm::BasicBlock* doneBb, llvm::Value* store,
                                    const char* prefix);

// Emits the one-shot publish call: reached from the ordinary sequence's slow
// block, and guarded there on the cell still being zero, so a site whose layout
// was wrong probes once and never again.
void emitStaticSlotPublish(llvm::IRBuilder<>& builder, const AbiFns& abi,
                           const ModuleTables& tables, llvm::Value* objBits, uint32_t keyIndex,
                           uint32_t slot, uint32_t cellIndex, bool forWrite,
                           llvm::BasicBlock* continueBb, const char* prefix);

}  // namespace bronze::codegen_llvm
