#pragma once

// The STORE-RUN RECEIVER PROOF: one guard ladder spent by a run of element
// writes into one typed array at consecutive offsets off one base.
//
// The store-side sibling of llvm_recv_proof.h, and that file's header is the
// argument this one assumes. The shape it exists for is three.js's
// `Matrix4.toArray`, called from `InstancedMesh.setMatrixAt` as
// `m.toArray(this.instanceMatrix.array, i * 16)`:
//
//     array[offset]      = te[0];
//     array[offset + 1]  = te[1];
//     ...                                  // sixteen of these
//     array[offset + 15] = te[15];
//
// `array` and `offset` are dynamic parameters, so each of those sixteen lines
// is an `elem.set` and each one pays the whole of emitElemSet's ladder
// (llvm_elem.cpp): the receiver-is-an-object test, the index-is-an-integral-
// number test, the flags switch, the view-length load, the value-is-a-number
// test, the nine-way kind switch, and the buffer/byteOffset walk that computes
// the address. Sixteen times, for one receiver and one offset, with nothing
// commoned between them because every arm's refusal edge reaches a helper.
//
// WHAT IS PROVEN, once, in front of the run: the receiver is an object; its
// flags say TYPED_ARRAY; its kind is FLOAT32 or FLOAT64; the base `B` is a
// non-negative integral double (an exact round trip through a u32); and
// `B + maxOffset` is inside the view's length. What that leaves is a byte
// pointer at element zero and an integer index, and each store in the run is
// then a Number test on the value, an add, a shift, a GEP and a store.
//
// WHY ONE LENGTH READ COVERS THE WHOLE RUN. The view's length word is
// MAINTAINED rather than fixed: `closeOrReopenViews` (src/runtime/typed_array.cpp)
// rewrites it when the underlying buffer is detached, transferred, resized or
// grown, and zeroing it is exactly how a stranded view is closed. Every one of
// its callers is a builtin method body — ArrayBuffer.prototype.transfer,
// transferToFixedLength, resize, SharedArrayBuffer.prototype.grow — and a
// builtin method body is reached from compiled code only through a CALL. The
// same is true of the buffer's external-storage word, which the embedding API
// flips inside a host call, and of the buffer's address, which only a
// collection can change. So all three of the things one length read and one
// base pointer could go stale against are bounded out by the same
// `il::canCollect` rule that bounds the read runs: a run spans instructions
// that cannot move the heap, and stops at the first one that can. There is no
// operation that resizes, detaches or moves a buffer without being a call, and
// if one were ever added `canCollect` would answer true for it — it is written
// the safe way round — and the run would stop there instead.
//
// An element STORE cannot do any of it: it writes bytes inside the window the
// length word already described. That is what makes a run of them provable at
// all.
//
// THE BASE IS AN SSA VALUE, NOT A LOCATION. "The same base" means the same IL
// ValueId. In the guarded region's fast copy the offset is raw-unboxed once and
// every index is `box.f64(add %B, const.f64 k)` off that one `%B`, so the whole
// run shares a base. In the SLOW copy each store re-derives its own
// `unbox.f64` of the dynamic parameter: those are different ValueIds, and a
// checked `unbox.f64` is ToNumber, which can run `valueOf`, which can collect —
// so `canCollect` would end the run there anyway. Both readings agree, and the
// fast copy is the one this exists for.
//
// Seams: BRONZE_NO_RECV_PROOF=1 turns every receiver proof off, read and store
// alike; BRONZE_NO_STORE_PROOF=1 turns off this file alone.

#include <cstdint>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

#include "il/il.h"

namespace bronze::codegen_llvm {

// What one IL block's `elem.set` instructions were planned into. Indexed by
// the instruction's position in the block, exactly like ReceiverRunPlan.
struct StoreRunPlan {
    static constexpr uint32_t kNoRun = UINT32_MAX;

    struct Site {
        uint32_t run = kNoRun;
        // The run's first member establishes the proof; the rest spend it.
        bool establishes = false;
        // The largest offset any member of the run writes, which is what the
        // one length test has to clear.
        uint32_t runMaxOffset = 0;
        // This store's own offset off the run's base.
        uint32_t offset = 0;
        // The f64 SSA value every member's index is affine over. Carried here
        // rather than re-derived at emission, so the value the proof is built
        // on is the value the plan proved the run about.
        il::ValueId base = il::kNoValue;
    };

    std::vector<Site> sites;

    bool empty() const { return sites.empty(); }
    Site at(size_t instIndex) const {
        return instIndex < sites.size() ? sites[instIndex] : Site{};
    }
};

// What one `elem.set` looks like to the planner: the receiver it writes, the
// f64 SSA value its index is affine over, and the constant added to that base.
// `ok` is false for anything that is not `recv[B + k] = v` with one f64 SSA
// base and a non-negative integral constant `k`.
struct StoreSiteShape {
    bool ok = false;
    il::ValueId receiver = il::kNoValue;
    il::ValueId base = il::kNoValue;
    uint32_t offset = 0;
};

// No offset past this is planned. A run whose offsets span more than a small
// window is not the shape this exists for, and the cap is what lets the
// bounds arithmetic below be obviously free of overflow.
constexpr uint32_t kMaxStoreOffset = 65535;

// Recognises the shape above. `defIndex` maps a ValueId to the position of its
// defining instruction in THIS block (kNoDef for a value defined elsewhere),
// which is what lets the index operand's `box.f64` and `add` be looked at
// without a search per site.
constexpr uint32_t kNoDef = UINT32_MAX;
StoreSiteShape classifyStoreSite(const il::Block& block, const std::vector<uint32_t>& defIndex,
                                 size_t instIndex);

// A value's defining instruction inside ONE block, or null for a value that
// came from anywhere else. Shared with the Array-store planner, which asks the
// same question of a pinned element store's `const.f64` index.
const il::Instruction* defOf(const il::Block& block, const std::vector<uint32_t>& defIndex,
                             il::ValueId id);

// Whether the store-side planner is enabled (BRONZE_NO_STORE_PROOF, and
// BRONZE_NO_RECV_PROOF which turns off both sides at once).
bool storeProofEnabled();

// The live store proof, carried by the emitter across a run's members.
//
// `ok` and `data` are re-established at every member's join for the reason the
// read proof re-establishes its own two: `data` is a DERIVED pointer, into the
// buffer rather than at it, so no root slot forwards it and a collection on
// some member's slow arm leaves it dangling.
//
// `index0`, `shift` and `isF32` are not. They are machine integers computed in
// the proof's join block, which dominates every member of the run — every path
// to a later member goes through it — and no heap event can change a number in
// a register. Phi-ing them too would be three more phis per member saying
// nothing.
struct StoreProof {
    il::ValueId receiver = il::kNoValue;
    il::ValueId base = il::kNoValue;
    uint32_t run = StoreRunPlan::kNoRun;
    llvm::Value* ok = nullptr;      // i1: the ladder below may be skipped
    llvm::Value* data = nullptr;    // i8*: element zero of the view's window
    llvm::Value* index0 = nullptr;  // i64: the base index B, as an integer
    llvm::Value* shift = nullptr;   // i64: 2 for FLOAT32, 3 for FLOAT64
    llvm::Value* isF32 = nullptr;   // i1: which of the two kinds it turned out to be

    bool live() const { return ok != nullptr; }
};

// Emits the guard ladder in front of a run and returns the proof it produced.
// `baseDbl` is the run's base as a double in SSA. Leaves the builder in the
// block where the caller's own emission continues — nothing branches away,
// because a failed proof is an `ok` of false and not a jump.
StoreProof emitStoreProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                          llvm::Value* baseDbl, il::ValueId receiver, il::ValueId base,
                          uint32_t run, uint32_t maxOffset);

// What the fast arm left behind: the single block that reaches `doneBb` having
// performed the store, for the caller's joins to name as a predecessor.
struct ProvenStore {
    llvm::BasicBlock* fastBb = nullptr;
};

// The proof's fast arm for one store, emitted in front of the ladder. On
// return the builder sits in a fresh block — the one emitElemSet should be
// emitted into — so the caller carries on as if nothing happened.
//
// The value is tested for being a Number here and nowhere else: 10.4.5.16 runs
// ToNumber BEFORE it asks whether the index is valid, and this arm has already
// answered the validity question, so a value that needs converting (an object,
// a string, a BigInt) must take the ladder that owns that conversion, its
// ordering, and the out-of-range discard.
ProvenStore emitProvenElementStore(llvm::IRBuilder<>& builder, const StoreProof& proof,
                                   uint32_t offset, llvm::Value* valBits,
                                   llvm::BasicBlock* doneBb);

// Re-establishes `ok` and `data` at a member's join: `fastBb` carries them
// forward, every other predecessor of `doneBb` carries a proof that is not
// live. A null `fastBb` means no edge preserved the proof, and the proof dies.
void rejoinStoreProof(StoreProof& proof, llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb);

}  // namespace bronze::codegen_llvm
