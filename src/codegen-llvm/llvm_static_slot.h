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

// Which of the two guards a site carries, and everything both need. Passed as
// one value because the choice is per site and the two emitters would otherwise
// grow four parameters each, three of which are ignored either way.
struct StaticSite {
    // The instance slot the layout proved, or `il::Instruction::kNoStaticSlot`
    // for a site with no layout claim at all (which emits nothing).
    uint32_t slot = 0xFFFFFFFFu;
    // IDENTITY form: the module cell holding the one shape this site accepts.
    uint32_t cellIndex = 0;
    // FAMILY form: the preorder id of the receiver's class and the size of its
    // `extends` subtree. `kNoFamily` selects the identity form above.
    static constexpr uint32_t kNoFamily = 0xFFFFFFFFu;
    uint32_t familyLo = kNoFamily;
    uint32_t familySpan = 0;

    bool none() const { return slot == 0xFFFFFFFFu; }
    bool family() const { return familyLo != kNoFamily; }
};

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
// `missBb`. A site with no claim emits nothing.
//
// `store` is null for a read; non-null makes the hit block store it into the
// slot instead of loading from it.
//
// The FAMILY form differs from the identity form in exactly one place: what the
// second compare asks. Instead of `shape == the one shape this site pinned` it
// loads the family word off that shape and asks whether it names a class in the
// site's own `extends` subtree — `stamp - (base + lo) <=u span`, one extra load
// (of an immortal, shared, hot word) and one extra subtract. That is the whole
// price of serving every subclass from one site instead of one.
StaticSlotGuard emitStaticSlotGuard(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                    llvm::Value* objBits, const StaticSite& site,
                                    llvm::BasicBlock* doneBb, llvm::Value* store,
                                    const char* prefix);

// Emits the one-shot fill reached from the ordinary sequence's slow block.
//
// For an identity site that is the publish call, guarded on the cell still
// being zero, so a site whose layout was wrong probes once and never again. For
// a family site it is the STAMP call, guarded on the shape's family word still
// being zero — one-shot per shape rather than per site, which is what lets a
// site that meets five subclasses end up hitting on all five instead of pinning
// the first.
//
// `objSlot` is the receiver's GC ROOT SLOT, and this path may not run without
// one. It sits after the fallback helper call, and that call allocates: a
// collection inside it moves the receiver and writes the new address into the
// root frame, leaving the `objBits` register the guard was built from pointing
// into dead from-space. Every dereference here — the flags byte, the shape word,
// the family word, and the receiver the helper is handed — has to come from the
// slot, reloaded after the call. A null slot means the value is not rooted and
// there is nothing to reload, and the publish is skipped rather than guessed at.
void emitStaticSlotPublish(llvm::IRBuilder<>& builder, const AbiFns& abi,
                           const ModuleTables& tables, llvm::Value* objBits,
                           llvm::Value* objSlot, uint32_t keyIndex, const StaticSite& site,
                           bool forWrite, llvm::BasicBlock* continueBb, const char* prefix);

}  // namespace bronze::codegen_llvm
