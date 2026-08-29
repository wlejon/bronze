#pragma once

// The ARRAY STORE-RUN RECEIVER PROOF: one guard ladder spent by a run of
// constant-index `prop.set` writes into one JS Array.
//
// The third of the three run proofs, and the two that came first are the
// argument this one assumes: llvm_recv_proof.h for a run of constant-index
// READS off an Array, llvm_store_proof.h for a run of affine `elem.set` writes
// into a typed array. The shape this one exists for is what three.js's matrix
// kernels END with:
//
//     copy(m) { const te = this.elements; const me = m.elements;
//               te[0] = me[0]; te[1] = me[1]; ... te[15] = me[15]; }
//
// Each of those sixteen writes is a `prop.set` with an index key, and
// emitPropSet's array arm already inlines one: flags, length, capacity, head,
// the named-properties side object and the elements object — six loads and five
// branches, sixteen times, for ONE receiver. Worse, `prop.set` is
// `il::canCollect` (it reaches bronze_prop_set, which can run a setter), so
// before this file the FIRST store killed the read run standing over it and
// `copy`'s reads paid their own ladder sixteen times as well.
//
// WHAT IS PROVEN, once, in front of the run:
//
//   1. the receiver is an object;
//   2. its kind word is exactly ARRAY;
//   3. the run's largest index is below `length`;
//   4. `head + maxIndex` is below `capacity`;
//   5. its named-properties side object is `undefined`;
//   6. its elements object is an object.
//
// That is guard for guard the set emitPropSet's per-store arm makes, which is
// the rule this proof is held to: a run may not skip a test one store made.
// What it leaves is a base pointer at element zero — byte for byte the address
// the read proof and the per-store arm compute for index zero — and each member
// is then a GEP and one eight-byte store of the boxed value.
//
// WHY THOSE SIX ARE THE WHOLE OF IT. bronze_prop_set's array path
// (rt_prop_write.cpp) does four things this arm does not, and each is bounded
// out by one of the guards above rather than skipped:
//
//   - rtArrayElementWriteRefusal (integrity.h). An array records its integrity
//     level in the dictionary of its named-properties side object, because that
//     is the only table it has; an array with no side object has nowhere to
//     have recorded one, so guard 5 IS the frozen/sealed/non-extensible test.
//     A frozen array has been through `Object.freeze`, which creates the side
//     object, so it fails guard 5 and takes the ladder that owns the TypeError.
//   - ArrayHeader::setLength. Reached only for `idx > length`; guard 3 is
//     strict, so every member of a proven run writes an index that already
//     exists as a slot and `length` is never touched.
//   - ArrayHeader::setElemSlow — growth and head compaction. Reached only when
//     `head + idx >= capacity` or `idx > length`; guards 3 and 4 are exactly
//     its two conditions read the other way.
//   - A hole store. Filling a hole at an index below `length` is a CREATE
//     rather than a write (10.1.6.3 step 2.b needs [[Extensible]]), and that
//     question is asked of the integrity table, which guard 5 has already said
//     does not exist. The dense element word is the only bookkeeping bronze
//     keeps for a hole — `hasElem` reads the slot itself — so writing the
//     Value over the hole sentinel is the whole of the update.
//
// There is no GC WRITE BARRIER to skip: bronze's collector is a semispace
// copier (runtime/heap.h) that traces from roots at collection time and keeps
// no remembered set, so storing a Value into an element slot owes nothing
// beyond the store.
//
// WHY THE RUN ENDS WHERE IT DOES is the read proof's rule unchanged: the base
// pointer is DERIVED into the elements object, no root slot forwards it, and
// `il::canCollect` bounds the span. A member's own miss arm reaches
// bronze_prop_set and can collect, which is why the proof is threaded through a
// phi at each member's join rather than held in a variable.
//
// NO VALUE TEST. A typed-array store owes ToNumber and so its proven arm tests
// the value first (llvm_store_proof.h); an Array element slot holds a Value,
// and `arr[i] = v` performs no coercion of `v` at all, so any sixty-four bits
// go in unchanged. That is also why nothing here reads `ValueRepr`: the array
// arm of emitPropSet stores `valBits` and not the double-slot-canonicalized
// `storeBits`, and a proven store must store the same bits a ladder store of
// the same element would.
//
// Seams: BRONZE_NO_RECV_PROOF=1 turns every receiver proof off;
// BRONZE_NO_ARRAY_STORE_PROOF=1 turns off this file alone.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "il/il.h"

namespace bronze::codegen_llvm {

// What one IL block's constant-index `prop.set` instructions were planned into.
// Indexed by the instruction's position in the block, exactly like
// ReceiverRunPlan and StoreRunPlan.
struct ArrayStoreRunPlan {
    static constexpr uint32_t kNoRun = UINT32_MAX;

    struct Site {
        uint32_t run = kNoRun;
        // The run's first member establishes the proof; the rest spend it.
        bool establishes = false;
        // The largest index any member of the run writes, which is what the one
        // length test and the one capacity test have to clear.
        uint32_t runMaxIndex = 0;
        // This store's own index.
        uint32_t index = 0;
    };

    std::vector<Site> sites;

    bool empty() const { return sites.empty(); }
    Site at(size_t instIndex) const {
        return instIndex < sites.size() ? sites[instIndex] : Site{};
    }
};

// Whether this planner is enabled (BRONZE_NO_ARRAY_STORE_PROOF, and
// BRONZE_NO_RECV_PROOF which turns off every receiver proof at once).
bool arrayStoreProofEnabled();

// The live array-store proof, carried by the emitter across a run's members.
//
// Both fields are re-established at every member's join, for the reason the
// read proof re-establishes its own two: `base` is a DERIVED pointer, into the
// elements object rather than at it, so no root slot forwards it and a
// collection on some member's slow arm leaves it dangling.
struct ArrayStoreProof {
    il::ValueId receiver = il::kNoValue;
    uint32_t run = ArrayStoreRunPlan::kNoRun;
    llvm::Value* ok = nullptr;    // i1: the ladder below may be skipped
    llvm::Value* base = nullptr;  // i64*: element zero of the array's storage

    bool live() const { return ok != nullptr; }
};

// Emits the guard ladder in front of a run and returns the proof it produced.
// Leaves the builder in the block where the caller's own emission continues —
// nothing branches away, because a failed proof is an `ok` of false and not a
// jump.
ArrayStoreProof emitArrayStoreProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                    il::ValueId receiver, uint32_t run, uint32_t maxIndex);

// What the fast arm left behind: the single block that reaches `doneBb` having
// performed the store, for the caller's joins to name as a predecessor.
struct ProvenArrayStore {
    llvm::BasicBlock* fastBb = nullptr;
};

// One member of a run, as a straight line: the GEP and the eight-byte store. No
// branch of its own, which is what makes it a step of a run-arm group's fast arm
// (llvm_run_arms.h) as well as the arm `emitProvenArrayElementStore` builds
// under a test.
void emitElementStore(llvm::IRBuilder<>& builder, const ArrayStoreProof& proof, uint32_t index,
                      llvm::Value* valBits);

// The proof's fast arm for one store, emitted in front of the property cache.
// On return the builder sits in a fresh block — the one the cache's own ladder
// should be emitted into — so the caller carries on as if nothing happened.
ProvenArrayStore emitProvenArrayElementStore(llvm::IRBuilder<>& builder,
                                             const ArrayStoreProof& proof, uint32_t index,
                                             llvm::Value* valBits, llvm::BasicBlock* doneBb);

// Re-establishes `ok` and `base` at a member's join: `fastBb` carries them
// forward, every other predecessor of `doneBb` carries a proof that is not
// live. A null `fastBb` means no edge preserved the proof, and the proof dies.
void rejoinArrayStoreProof(ArrayStoreProof& proof, llvm::BasicBlock* fastBb,
                           llvm::BasicBlock* doneBb);

}  // namespace bronze::codegen_llvm
