#pragma once

// One IL function's body, emitted into one LLVM function.
//
// The state below is shared by every instruction of that body — the GC root
// frame, the block phis of the block-argument SSA, and the SSA-value table — so
// it is held together rather than threaded through a dozen parameters.

#include <cstdint>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_frame.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::codegen_llvm {

// The LLVM type an IL type lowers to. Null for a type with no mapping, which
// the caller reports against the construct that produced it.
llvm::Type* mapILType(il::Type type, llvm::LLVMContext& ctx);

class FunctionEmitter {
public:
    struct Context {
        llvm::LLVMContext& ctx;
        const il::Module& module;
        const AbiFns& abi;
        // The tables this object file owns (llvm_abi.h): the IC sites, the key
        // remap, and the two module-local caches.
        const ModuleTables& tables;
        // Indexed by IL function index: the typed entry point, and the
        // uniform-convention wrapper that adapts to it.
        const std::vector<llvm::Function*>& entries;
        const std::vector<llvm::Function*>& wrappers;
        DiagnosticSink& diags;
        // Every function's frame layout and the region plan over the
        // module's inline-asking direct edges (llvm_frame.h). Both are
        // computed before any body is emitted, because a caller has to size
        // its frame for a callee it may not have emitted yet.
        const std::vector<FramePlan>& plans;
        const RegionPlan& regions;
        // The frameless variant of each merge target, null elsewhere.
        const std::vector<llvm::Function*>& inlineVariants;
        // Whether ANY function in the module reads `new.target`. The inline
        // `new` fast path skips the helper's NewTargetScope push, which is
        // observable through bronze_get_new_target and nothing else — so one
        // mention anywhere keeps every construct site on the helper.
        bool moduleHasNewTarget;
        // What each function's `dynamic` values are made of (llvm_repr.h,
        // stage R2). Indexed like `plans`, and planned beside them for the same
        // reason: the frame layout is derived from it.
        const std::vector<ReprPlan>& reprPlans;
        // Where a collection can see each value, and where a use may read its
        // register instead of its slot (llvm_live_roots.h). Indexed like
        // `plans`, and planned beside them for the frame layout's sake too.
        const std::vector<LiveRootPlan>& livePlans;
    };

    // `frameless` emits the body against a region of the CALLER's frame,
    // handed over as the two trailing parameters `llvmFunc` then carries: the
    // region base and the thread's ABI block. Anything else owns its frame.
    FunctionEmitter(const Context& shared, uint32_t funcIndex, llvm::Function* llvmFunc,
                    bool frameless);

    // Emits every block of the function. False on a diagnosed error.
    bool emit();

private:
    void emitPrologue();
    void createBlockPhis();
    void emitModuleInit();
    void emitFunctionSourceTables();
    bool emitBlock(size_t blockIndex);
    // One instruction with everything that surrounds it: the operand reloads in
    // front, the proof bookkeeping and the root store behind, the exception
    // test last. Its own function because a run-arm group's SLOW ARM is exactly
    // this, several times, in a block of its own — `forceReload` is what says
    // so, because the plan's anchors describe the group as one unit that does
    // not collect and inside the slow arm every ladder does.
    bool emitInstructionAt(size_t blockIndex, size_t instIndex, bool forceReload);
    bool emitInstruction(const il::Instruction& inst);
    // A run of element reads as one proof branch over two straight-line arms
    // (llvm_run_arms.h). Leaves the builder in the join, with every result and
    // every value live across the run in a register the join phi'd.
    bool emitRunArmGroup(const RunArmGroup& group);

    // Instruction families. Each returns false only after diagnosing.
    bool emitTerminator(const il::Instruction& inst);
    bool emitRuntimeOp(const il::Instruction& inst);
    bool emitDynamicCall(const il::Instruction& inst);
    bool emitMethodCall(const il::Instruction& inst);
    // The guarded direct edge in front of it, or false when this site has none.
    bool emitMethodCallDirect(const il::Instruction& inst, llvm::Value* thisVal, uint32_t argc);
    bool emitMethodCallSpread(const il::Instruction& inst);
    bool emitConstruct(const il::Instruction& inst);
    bool emitCall(const il::Instruction& inst);
    bool emitArithmetic(const il::Instruction& inst);
    // The ACCESS family (llvm_ops_access.cpp): every op that reads or writes a
    // property of an object, by name or by index — including the two receiver
    // proofs, which are a property of a RUN of accesses and so belong beside
    // the accesses themselves.
    bool emitAccessOp(const il::Instruction& inst);
    // The private-element family (llvm_private.cpp). Its own unit because it
    // is one mechanism with six instructions, not six unrelated helpers.
    bool emitPrivateOp(const il::Instruction& inst);

    // Reloads a Dynamic value from its root slot at the point of use: if
    // anything collected since the def, the slot was forwarded and the SSA
    // register was not.
    // `holeInsensitive` says the one use this reload is for cannot tell an
    // element's own bits from the `undefined` a hole reads as, so a hole-raw
    // slot hands its bits over uncorrected (llvm_func.h, `holeRawSlot_`).
    // `anchor` is the plan's answer for this use (llvm_live_roots.h): the IL
    // block the register was last written in, or `kReload`. The load is skipped
    // only when the emitter's own record agrees with it — the plan is about the
    // IL and `values_` is about what was emitted, and a disagreement costs a
    // load rather than soundness.
    void reload(il::ValueId id, bool holeInsensitive = false,
                uint32_t anchor = LiveRootPlan::kReload);
    llvm::Value* slotAddr(uint32_t slot);
    // The base of the region a merged callee's slots live in — this function's
    // own slots end exactly there. Null when it has no frame at all.
    llvm::Value* calleeRegionBase();
    // The ADDRESS of an operand's root slot, for code that has to re-read the
    // value after a call rather than before it. Null when the value has no slot
    // (it is not Dynamic, so nothing can move it, and there is nothing to
    // reload) — a caller that needs a live pointer must treat that as "cannot".
    llvm::Value* rootSlotAddrOf(const il::Instruction& inst, size_t index);
    // Fills the frame's argv region and returns a pointer to it, or a null
    // pointer constant when the call takes no arguments. `first` is the index
    // of the first argument operand.
    llvm::Value* emitArgv(const il::Instruction& inst, size_t first, uint32_t argc, bool& ok);

    // The pending-exception cell test, emitted after any instruction
    // `il::canThrow` admits: load the cell, compare it against "nothing
    // pending", and branch to this block's handler or out of the function.
    // Splits the current LLVM block, so everything after it in the same IL
    // block is emitted into the continuation.
    void emitExceptionCheck(size_t blockIndex);
    // Where an unwind from `blockIndex` goes: the IL handler this block
    // names, or the function's single unwind block, created on first demand.
    llvm::BasicBlock* unwindTargetFor(size_t blockIndex);
    // Pops the GC root frame and returns — byte-for-byte what `emitTerminator`
    // does before an ordinary `ret`, which is the property that makes the whole
    // mechanism sound with respect to rooting.
    llvm::BasicBlock* functionUnwindBlock();
    void popRootFrame();

    // An operand as an LLVM value, diagnosed as `what` when it has no def.
    llvm::Value* operand(const il::Instruction& inst, size_t index, const char* what);
    bool require(bool condition, const char* message);

    static constexpr uint32_t kNoSlot = UINT32_MAX;

    const Context& shared_;
    const il::Function& func_;
    llvm::Function* llvmFunc_;
    llvm::IRBuilder<> builder_;

    llvm::Type* i64Ty_;
    llvm::Type* ptrTy_;

    std::vector<llvm::BasicBlock*> blocks_;
    std::vector<std::vector<llvm::PHINode*>> blockPhis_;
    std::vector<llvm::Value*> values_;
    // The key a PropGet result was read by (UINT32_MAX otherwise): what lets
    // a DynamicCall recognize `Math.sqrt(x)`-shaped callees and emit the
    // code-pointer-guarded direct dispatch (llvm_math.h). Provenance only —
    // soundness lives entirely in the emitted guard.
    std::vector<uint32_t> propGetKey_;
    // The IL function index a FunctionRef result was read by (UINT32_MAX otherwise):
    // what lets DynamicCall and Construct invoke the known wrapper directly.
    std::vector<uint32_t> funcRefIndex_;
    // HOLE-RAW ROOT SLOTS. A value read through a receiver proof
    // (llvm_recv_proof.h) whose root slot holds the ELEMENT'S OWN BITS, hole
    // tag included, instead of the `undefined` a hole reads as. Every reload
    // corrects it, so what a consumer sees is unchanged; what moves is where
    // the correction is paid. In `Matrix4.multiplyMatrices` the thirty-two
    // reads are consumed by a numberness guard — which answers false for the
    // hole and for `undefined` alike — and by the edge into the guarded
    // region's slow copy, so the correction lands on a cold edge and leaves
    // the fast copy's read block at one load per element.
    //
    // Set only for a value that HAS a root slot: a slotless value is carried
    // in SSA with no reload to correct it, so the read arm keeps the select.
    std::vector<uint8_t> holeRawSlot_;
    // Whether a hole-raw slot would pay for itself, by value id. Moving the
    // correction from the read to the reload trades ONE select for one per
    // USE, so it is only a saving where the uses that need no correction
    // outnumber the ones that do — thirty-two reads consumed by a guard and a
    // raw unbox each against one cold edge into a slow copy, and not
    // `Matrix4.copy`, whose reads go straight into a store and would pay the
    // same select one block later.
    std::vector<uint8_t> holeRawPays_;
    // Whether the element's own bits may travel in a REGISTER, which is the
    // stronger question a run-arm group's fast arm has to ask: a slot's bits are
    // corrected by every reload that needs it, and a register has no reload to
    // ride. So this is set only where EVERY use is hole-insensitive, and the
    // fast arm keeps the select for anything else.
    std::vector<uint8_t> holeRawSafe_;
    // The IL block whose emission last wrote each value's entry in `values_`,
    // or `kNoBlock` for one nothing has written yet. This is what turns the
    // live-root plan's anchor from a claim into a check: an elided reload reads
    // a register, and a register is only legal where the block that produced it
    // dominates the use — which the plan guarantees for the block it names, and
    // for no other.
    std::vector<uint32_t> regBlock_;

    const std::vector<uint32_t>& slotOf_;
    uint32_t argvBase_ = 0;
    // What this function needs for itself, and what its frame holds in total —
    // the same number unless it merges a callee's region in beneath its own
    // slots (llvm_frame.h).
    uint32_t ownSlots_ = 0;
    uint32_t frameSlots_ = 0;
    // Set while emitting a frameless variant: the region base and the ABI
    // block, both parameters rather than things this function makes.
    bool frameless_ = false;
    uint32_t funcIndex_ = 0;
    llvm::Value* parentSlots_ = nullptr;
    llvm::Value* tlsBase_ = nullptr;
    // A dedicated root slot for the inline `new` fast path's fresh instance,
    // live only across each site's constructor call — every site shares it,
    // exactly as the argv region is shared. kNoSlot when the function has no
    // Construct or the module's `new.target` use keeps the path off.
    uint32_t constructSelfSlot_ = kNoSlot;
    // This function's view of the per-thread ABI block: field addresses off
    // the one bronze_tls_block_addr() call bound at the top of the entry
    // block (llvm_abi.h, bindTlsBlock).
    AbiGlobals globals_;
    llvm::StructType* frameTy_ = nullptr;
    llvm::Value* framePtr_ = nullptr;
    llvm::Value* slotsBase_ = nullptr;
    llvm::BasicBlock* unwindBlock_ = nullptr;
    // The IL block being emitted, which a terminator needs in order to find
    // its handler: `builder_.GetInsertBlock()` may be a continuation an
    // inlined guard or a cell test split off, and a continuation is not an
    // IL block at all.
    size_t currentILBlock_ = 0;
    // The index of the instruction being emitted inside that block: what
    // `storeValueRepr` needs to see the `pin.guard` in front of a store.
    size_t currentILInst_ = 0;
    // What this function's `dynamic` values are made of (llvm_repr.h). The
    // frame layout above was derived from it; the property emitters spend it
    // at the store-side representation tests.
    const ReprPlan& repr_;
    // Where a collection can see this function's values, and which uses may
    // read a register (llvm_live_roots.h). The frame layout above was derived
    // from the first half; `emitBlock` spends the second.
    const LiveRootPlan& live_;
    // The receiver runs of the block being emitted, and the proofs currently
    // alive inside them (llvm_recv_proof.h, llvm_store_proof.h,
    // llvm_array_store_proof.h). Planned per block, over the straight-line
    // CHAIN of blocks the block belongs to: a proof is an LLVM value and a
    // value has to dominate every use, and a chain member is dominated by its
    // head because it has one predecessor and takes no parameters.
    //
    // Three of them, because the kernels interleave runs: `Matrix4.toArray`
    // reads off one Array while storing into a typed array, `Matrix4.copy`
    // reads off one Array while storing into another. Each run's members sit
    // inside the others' spans, and a single slot would let each kill the
    // others at its first member.
    BlockRunPlan runPlan_;
    ReceiverProof recvProof_;
    StoreProof storeProof_;
    ArrayStoreProof arrayStoreProof_;
    // The IL block whose emission just finished, so `emitBlock` can tell a real
    // chain edge from a plan that merely hoped for one. `kNoBlock` before the
    // first block of a function, which is what makes that block open its own
    // chain whatever the plan says.
    il::BlockId lastEmittedBlock_ = il::kNoBlock;
    // Whether the instruction just emitted carried the live proofs across a
    // join of its own. Cleared before every instruction, so anything that can
    // collect and did NOT carry them ends them — the invariant is enforced at
    // the instruction rather than left to be a property of the plan.
    bool proofsCarried_ = false;
    // Set while a run-arm group's SLOW ARM is being emitted. The members there
    // are the ladder they would have been with no run at all: the group already
    // branched on the proof once, and a second test inside the arm it selected
    // could only be false.
    bool inRunArm_ = false;
    // Whether this module drops the environment-record ACCESS guards
    // (llvm_env.h, `envAccessGuardsElided`). A property of the invocation, so
    // it is read once rather than per env instruction.
    bool envGuardsElided_ = false;
};

// The framed entry of a merge target: it allocates the whole region, links it,
// calls the frameless variant, and unlinks. Byte for byte the prologue and
// epilogue the body used to carry, which is what makes an ordinary caller —
// the uniform wrapper, a refused edge — see exactly the function it saw before.
void emitFrameForwarder(const FunctionEmitter::Context& shared, uint32_t funcIndex,
                        llvm::Function* entry, llvm::Function* variant);

}  // namespace bronze::codegen_llvm
