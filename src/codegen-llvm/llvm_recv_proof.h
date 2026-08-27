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
// Seam: BRONZE_NO_RECV_PROOF=1 turns the planner off, so every site emits the
// ladder alone and the A/B is one environment variable.

#include <cstdint>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

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

// Plans the runs of one block. Returns an empty plan when the seam is off or
// the block holds no run worth proving. The module comes along because the
// planner has to read each site's KEY to know it is an index at all.
ReceiverRunPlan planReceiverRuns(const il::Module& module, const il::Function& func,
                                 size_t blockIndex);

// Whether the planner is enabled at all (BRONZE_NO_RECV_PROOF).
bool receiverProofEnabled();

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

// What the fast arm left behind: the block that reaches `doneBb` carrying
// `value`, for the caller's phi to take an incoming from.
struct ProvenRead {
    llvm::BasicBlock* fastBb = nullptr;
    llvm::Value* value = nullptr;
};

// The proof's fast arm for one read, emitted in front of the property cache.
// On return the builder sits in a fresh block — the one the cache's own ladder
// should be emitted into — so the caller carries on as if nothing happened.
ProvenRead emitProvenElementRead(llvm::IRBuilder<>& builder, const ReceiverProof& proof,
                                 uint32_t index, llvm::BasicBlock* doneBb);

// Re-establishes `ok` and `base` at a member's join: the fast arm carries them
// forward, every other predecessor of `doneBb` carries a proof that is not
// live. Called once the cache has finished wiring `doneBb`.
void rejoinReceiverProof(llvm::IRBuilder<>& builder, ReceiverProof& proof,
                         llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb);

}  // namespace bronze::codegen_llvm
