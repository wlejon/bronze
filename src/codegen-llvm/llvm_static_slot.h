#pragma once

#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_repr.h"

#include <string>

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

// The double form of an `Int32`-tagged Value, as a Value again: sign-extend the
// payload, convert, take the bits. No NaN canonicalization, because every int32
// converts to a finite double.
//
// Shared with the set-site inline cache, which faces the same store: an
// `il::Type::I32` boxes to `Tag::Int32`, whose bits are a tag and a payload
// rather than an f64, and stage R1's arms could only refuse it. This is
// `slotReprCanonicalize`'s Int32 case (runtime/slot_repr.h) written as two
// instructions at the site, so the slot ends up holding the same word the
// helper would have put there.
llvm::Value* emitInt32BoxAsDouble(llvm::IRBuilder<>& builder, llvm::Value* bits);

// Emits the guard at the current insert point and leaves the builder in
// `missBb`. A site with no claim emits nothing.
//
// `store` is null for a read; non-null makes the hit block store it into the
// slot instead of loading from it.
//
// `storeRepr` is what the value being stored is made of (llvm_repr.h), and it
// decides which of stage R1's representation tests the site emits:
//
//   Number     nothing. A Number's box IS the canonical double a double slot
//              must hold, so the store is correct whichever way the shape's
//              `double_slots` bit reads and the whole test folds away. THIS IS
//              THE RAW STORE: the eight bytes written are the f64.
//   Int32Boxed the test, and on the double arm a `sitofp` of the payload
//              instead of a miss - which is exactly the conversion
//              `slotReprCanonicalize` performs in the helper, moved inline so
//              that `this.n = i | 0` stops paying a call per store.
//   otherwise  stage R1's test-and-store arm, unchanged.
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
                                    ValueRepr storeRepr, const char* prefix);

// THE SAME GUARD AS AN i1, hoisted away from the access it licenses.
//
// The guard above branches to a hit block and reads the slot there, which is
// what a lone site wants. A RUN of reads off one receiver wants the questions
// asked once and the answer spent per field, so that the reads between two
// element accesses stop cutting the element run in half (llvm_run_arms.h). This
// is that form: the identical tests, in the identical order, off the identical
// fields, ending at a join whose `ok` a group can `and` with its other proofs.
//
// It is deliberately not a second opinion about the same question. What one
// answers the other must answer, so the slot address below is the address the
// guard computes and the two forms' shape questions are the two the guard asks.
struct OwnSlotProof {
    // The receiver's object header, poison on every edge where `ok` is false.
    llvm::Value* hdr = nullptr;
    llvm::Value* ok = nullptr;

    bool live() const { return ok != nullptr; }
};

// Emits the ladder at the current insert point and leaves the builder in its
// join. A site with no claim, or one whose table this module does not have,
// yields a proof with a null `ok` — the caller diagnoses that rather than
// emitting an access the guard never covered.
OwnSlotProof emitOwnSlotProof(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                              llvm::Value* objBits, const StaticSite& site,
                              const std::string& tag);

// One field of a proven receiver: the slot's address at a compile-time constant
// offset, and the load. No branch and no test, which is what makes it a step of
// a run-arm group's straight-line fast arm.
llvm::Value* emitOwnSlotLoad(llvm::IRBuilder<>& builder, const OwnSlotProof& proof, uint32_t slot,
                             const std::string& tag);

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
