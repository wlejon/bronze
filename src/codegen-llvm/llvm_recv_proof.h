#pragma once

// The RECEIVER PROOF: one guard ladder spent by a run of element reads.
//
// A constant-index read — `ae[0]` — compiles to the property inline cache in
// llvm_prop_get.cpp, and that cache re-derives everything it needs at every
// access: the receiver out of its root slot, the header, the kind flags, the
// length, the elements object, the ring head. Sixteen adjacent reads of ONE
// array therefore emit sixteen identical ladders, and LLVM cannot common them
// because each access's miss arm calls bronze_prop_get and merges back, which
// leaves memory unknown for the access after it.
//
// Three.js's `Matrix4.multiplyMatrices` is the shape this exists for: thirty-two
// adjacent constant-index reads off two matrices, then arithmetic. Measured
// against node before this file: 146 ns a call against 15. The same arithmetic
// with the operands in locals runs FASTER in bronze than in node (9.06 ns
// against 11.48), so the whole of that gap was the access ladder around it.
//
// WHAT IS PROVEN, once, in front of the run: the receiver is an object, its
// kind is ARRAY, its length exceeds the run's largest index, and its elements
// object is an object. What that leaves is a base pointer at element zero, and
// each read in the run is then a GEP, a load, and the hole test — four
// instructions against the ladder's twenty.
//
// WHY THE RUN ENDS WHERE IT DOES. The base pointer is DERIVED: it points into
// the elements object rather than at it, so no root slot forwards it and a
// collection leaves it dangling. `il::canCollect` is the whole rule — a run
// spans instructions that cannot move the heap, and stops at the first one that
// can. It also stops when the receiver is redefined, and at the end of the IL
// block, because a proof is a value and a value must dominate its use.
//
// A member's OWN miss arm can collect: it reaches bronze_prop_get, which can
// run a getter. That is why the proof is threaded through a phi at each
// member's join rather than held in a variable — the fast arm carries it
// forward unchanged, every other arm carries `false`, and a run that misses
// once simply finishes on the ladder it would have used anyway.
//
// A run does not have to be emitted that way, and usually is not:
// llvm_run_arms.h branches once on the proofs of every run a SPAN of
// instructions covers and puts the loads, the stores and the arithmetic between
// them on one arm and the ladders on the other, which is what makes the fast
// path free of safepoints and so free of root traffic. Everything below is what
// that arrangement is built out of, and it is still what a site the span rule
// refuses — a typed-array store run, a run cut by a named `prop.set` — is
// emitted as.
//
// THREE PROOFS AT ONCE. `Matrix4.toArray` interleaves a run of reads off
// `this.elements` with a run of stores into a Float32Array, and `Matrix4.copy`
// interleaves a run of reads off one Array with a run of constant-index stores
// into another — so no run is a contiguous stretch of instructions and each
// run's members sit inside the others' spans. A run member is TRANSPARENT to
// the other runs' proofs: its fast arm neither allocates nor calls, and its
// join re-establishes every live proof and not only its own. Anything else that
// `il::canCollect` ends all three. That is why the plans are computed together
// below rather than one per file — whether a store is transparent to a read run
// depends on whether the store's own run was committed, and the other way
// round, so all three are settled to a fixpoint in one pass.
//
// WHAT A NAMED STORE DOES TO A RUN. `Vector3.applyMatrix4` reads sixteen
// elements off one `m.elements` and writes `this.x`, `this.y`, `this.z`
// BETWEEN them, so on the rule above the run is four runs of four and the
// ladder is paid four times for arithmetic that reads one array once. But a
// named `prop.set` has three arms that are a shape test and an eight-byte
// store into a property the object already has — the static slot, the inline
// slot and the overflow slot — and none of those can move the heap. So such a
// store is TRANSPARENT the way a run member is: llvm_prop_set.cpp funnels
// those three arms through one block and hands it back as the join's
// proof-preserving edge, every other arm (a setter call, a shape transition,
// bronze_prop_set) leaves the proof dead at the join, and the run spans the
// store. Measured on the shape it exists for: 39.2 -> 25.6 ns a call.
//
// An INDEX key is deliberately not included: its arms below reach the array
// element store, which can grow the element block and therefore allocate, and
// a run of those is the Array store planner's business already.
//
// WHAT A NAMED OWN-SLOT READ DOES. `Matrix4.compose` finishes with `te[12] =
// position.x` three times, so the same rule read the other way round: a `o.name`
// read whose class layout proved a constant instance slot (il.h, `staticSlot`)
// is a shape compare, a GEP and a load, and a run spans it too. It is carried
// only where llvm_run_arms.h will SPEND it — as a step of a group, which is what
// makes the load unconditional — because a carry with no group behind it keeps
// the proof's phis live across a read with nothing to show for the pressure.
// `ownSlotRead` is the one predicate all three places ask.
//
// Seam: BRONZE_NO_RECV_PROOF=1 turns ALL THREE planners off, so every site
// emits the ladder alone and the A/B is one environment variable;
// BRONZE_NO_STORE_PROOF=1 turns off the typed-array store side alone,
// BRONZE_NO_ARRAY_STORE_PROOF=1 the Array store side alone,
// BRONZE_NO_SLOT_STORE_CARRY=1 the named-store carry, and
// BRONZE_NO_OWN_SLOT_STEP=1 the own-slot read carry with the group step it
// exists for.

#include <cstdint>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "codegen-llvm/llvm_array_store_proof.h"
#include "codegen-llvm/llvm_store_proof.h"
#include "il/il.h"

namespace bronze::codegen_llvm {

// What one IL block's instructions were planned into. Indexed by the
// instruction's position in the block; an instruction in no run has
// `run == kNoRun`.
struct ReceiverRunPlan {
    static constexpr uint32_t kNoRun = UINT32_MAX;

    struct Site {
        uint32_t run = kNoRun;
        // The run's first member establishes the proof; the rest spend it.
        bool establishes = false;
        // The largest index any member of the run reads, which is what the
        // one length test has to clear.
        uint32_t runMaxIndex = 0;
    };

    std::vector<Site> sites;

    bool empty() const { return sites.empty(); }
    Site at(size_t instIndex) const {
        return instIndex < sites.size() ? sites[instIndex] : Site{};
    }
};

// All three of one block's plans. Empty when the seams are off or the block
// holds no run worth proving.
struct BlockRunPlan {
    ReceiverRunPlan reads;
    StoreRunPlan stores;
    ArrayStoreRunPlan arrayStores;
    // The block this one continues a run chain from, or `kNoBlock` where it
    // opens its own. The emitter keeps its live proofs across the edge only
    // when this names the block it has just finished emitting — the plan is
    // free to be optimistic here, because a proof that is not live makes a site
    // emit the ladder it would have emitted anyway.
    il::BlockId continues = il::kNoBlock;
};

// Plans the read runs, the typed-array store runs and the Array store runs of
// one block together, to the fixpoint the header describes. The module comes
// along because the planner has to read each read and Array-store site's KEY to
// know it is an index at all.
BlockRunPlan planBlockRuns(const il::Module& module, const il::Function& func,
                           size_t blockIndex);

// The same plan with the named-store carry decided by the caller rather than by
// the environment. The seam is a process-wide cached read, so a test that wants
// to see both policies in one run cannot get there through the function above;
// this is how both stay pinned.
BlockRunPlan planBlockRuns(const il::Module& module, const il::Function& func, size_t blockIndex,
                           bool carry);

// Whether the read planner is enabled at all (BRONZE_NO_RECV_PROOF).
bool receiverProofEnabled();

// Whether a NAMED `prop.set` hands a live proof across its join
// (BRONZE_NO_SLOT_STORE_CARRY). Read by the planner, which must not span a
// store the emitter is not going to carry, and by llvm_prop_set.cpp, which
// must not build the join block the planner is not going to use — so the two
// answer the same question from the same place.
bool slotStoreCarryEnabled();

// One value re-established at a join: `live` down the proof-preserving edge,
// `dead` down every other predecessor of `doneBb`. The phi goes at the front
// of `doneBb`, so it may be called for several proofs at the same join.
llvm::Value* phiAtJoin(llvm::BasicBlock* doneBb, llvm::BasicBlock* fastBb, llvm::Value* live,
                       llvm::Value* dead, const std::string& name);

// Where a site that offered a proof-preserving edge left it. `fastBb` is null
// for a site that offered none, which is what tells every proof crossing that
// site to die rather than to be carried.
struct ProofJoin {
    llvm::BasicBlock* fastBb = nullptr;
    llvm::BasicBlock* doneBb = nullptr;
};

// The live proof, carried by the emitter across a run's members.
//
// `ok` and `base` are LLVM values in the function being emitted: `ok` is an i1
// that says the ladder below may be skipped, and `base` points at element zero
// of the receiver's storage. Both are re-phi'd at every member's join, so a use
// always reads the version that reaches it.
struct ReceiverProof {
    il::ValueId receiver = il::kNoValue;
    uint32_t run = ReceiverRunPlan::kNoRun;
    llvm::Value* ok = nullptr;
    llvm::Value* base = nullptr;

    bool live() const { return ok != nullptr; }
};

// Emits the guard ladder in front of a run and returns the proof it produced.
// Leaves the builder in the block where the caller's own emission continues —
// nothing here branches away, because a failed proof is an `ok` of false and
// not a jump.
ReceiverProof emitReceiverProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                il::ValueId receiver, uint32_t run, uint32_t maxIndex);

// The same ladder as an i1 and nothing else: what `il::Op::IsDenseArray` is.
// The guarded-region pass emits that instruction in front of a run of reads it
// intends to perform before testing any of their values, and the answer here is
// what says those reads are LOADS — re-runnable, reorderable against each other,
// and free of user code. Same predicate as the proof above by construction, so
// a run the region pass merged is a run this backend can prove.
llvm::Value* emitDenseArrayTest(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                uint32_t maxIndex);

// What the fast arm left behind: the block that reaches `doneBb` carrying
// `value`, for the caller's phi to take an incoming from.
struct ProvenRead {
    llvm::BasicBlock* fastBb = nullptr;
    llvm::Value* value = nullptr;
};

// The proof's fast arm for one read, emitted in front of the property cache.
// On return the builder sits in a fresh block — the one the cache's own ladder
// should be emitted into — so the caller carries on as if nothing happened.
//
// `holeRawSlot` says the CALLER will spend the hole correction where the boxed
// value is read rather than where it is produced (llvm_func.h, `holeRawSlot_`),
// so this arm hands back the element's own bits and emits no select at all.
// False is the original arm: the correction stands here, in the read's block.
ProvenRead emitProvenElementRead(llvm::IRBuilder<>& builder, const ReceiverProof& proof,
                                 uint32_t index, llvm::BasicBlock* doneBb,
                                 bool holeRawSlot = false);

// The hole correction itself, as one place both the read arm and the reload
// spend: a dense element that is a hole reads as `undefined` (10.4.2.1's
// ordinary [[Get]] over an absent index, with Array.prototype's own index
// properties refused by name elsewhere), and no other answer a raw load gives
// needs changing.
llvm::Value* emitHoleCorrection(llvm::IRBuilder<>& builder, llvm::Value* raw,
                                const std::string& name);

// THE SEAM: `BRONZE_NO_HOLE_RAW=1` puts the correction back at the read.
//
// ON by default, which it was not when it was written. The saving was always
// real on the kernel and it stopped at the kernel, because the correction it
// removed from the read block was paid straight back at the RELOAD in the guard
// chain — every one of the run's results was reloaded there, so "one select per
// use that needs it" was one select per read all over again. Run arms
// (llvm_run_arms.h) are what removed those reloads: the results now travel from
// the join to their uses in registers, and the uses of a guarded region's reads
// need no correction, so the select is gone rather than moved. The pair is what
// carries the whole-program fixtures the correction alone could not.
bool holeRawSlotEnabled();

// A use that cannot tell an element's own bits from the `undefined` a hole
// reads as. There are exactly two, and they are the whole soundness argument
// for a hole-raw slot, so they are named here rather than at their emitters
// (llvm_ops.cpp), which must not change what they read without changing this:
//
//   `is.number`      — `bits <=u kNumberMaxBits`, and the hole tag and
//                      `undefined` are both above the number range, so the
//                      answer is false for either.
//   RAW `unbox.f64`  — stands only where something already proved the bits
//                      are a Number, which a hole is not, so the correction
//                      could not have chosen its other arm on any path that
//                      reaches one.
//
// `v` must be the instruction's first operand, the only position either reads.
bool holeInsensitiveUse(const il::Instruction& inst, il::ValueId v);

// Per value of `fn`: would a hole-raw slot emit fewer selects than it removes?
// Moving the correction off the read and onto the reload trades ONE select for
// one per use that still needs it, so the answer is yes exactly where the uses
// that need none are at least as many — thirty-two `multiplyMatrices` reads
// consumed by a guard and a raw unbox each against one edge into a slow copy,
// and no for `Matrix4.copy`, whose reads go straight into a store and would pay
// the same select one block later.
std::vector<uint8_t> planHoleRawSlots(const il::Function& fn);

// Per value of `fn`: may the element's own bits travel in a REGISTER? A slot's
// bits are corrected by whichever reload needs it, and a register has no reload
// to ride — so this is the ALL question where `planHoleRawSlots` above is the
// majority one, and it is what a run-arm group's fast arm asks before it hands
// a raw load to the join phi rather than to a root slot.
std::vector<uint8_t> planHoleRawRegisters(const il::Function& fn);

// One member of a run, as a straight line: the GEP, the load, and the hole
// correction unless the caller is carrying raw bits. No branch of its own,
// which is what makes it the body of a run-arm group's fast arm as well as the
// arm `emitProvenElementRead` builds under a test.
llvm::Value* emitElementLoad(llvm::IRBuilder<>& builder, const ReceiverProof& proof,
                             uint32_t index, bool raw);

// Re-establishes `ok` and `base` at a member's join: the fast arm carries them
// forward, every other predecessor of `doneBb` carries a proof that is not
// live. Called once the cache has finished wiring `doneBb`.
void rejoinReceiverProof(llvm::IRBuilder<>& builder, ReceiverProof& proof,
                         llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb);

}  // namespace bronze::codegen_llvm
