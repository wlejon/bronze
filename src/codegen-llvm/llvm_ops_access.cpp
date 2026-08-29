// The ACCESS family: every IL op that reads or writes a property of an object,
// by name (`o.x`), by index (`o[i]`), on a proven view (`elem.get.typed`), or
// through a home object (`super.x`). Peeled off llvm_ops.cpp the way
// llvm_ops_call.cpp peels off the call family, and for a stronger reason than
// size: the two RECEIVER PROOFS are a property of a RUN of accesses rather
// than of any one of them, so the code that establishes, spends and re-joins
// them belongs beside the accesses and nowhere else.

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_prop.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_repr.h"
#include "codegen-llvm/llvm_static_slot.h"
#include "codegen-llvm/llvm_store_proof.h"

namespace bronze::codegen_llvm {

namespace {

// The four IL fields the static-slot emitters read, in the one struct they
// take — so a site's guard form is decided in one place rather than at each
// use.
StaticSite staticSiteOf(const il::Instruction& inst) {
    StaticSite site;
    site.slot = inst.staticSlot;
    site.cellIndex = inst.staticCellIndex;
    site.familyLo = inst.familyLo;
    site.familySpan = inst.familySpan;
    return site;
}

}  // namespace

bool FunctionEmitter::emitAccessOp(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };
    auto callWith = [&](llvm::Function* fn, std::initializer_list<llvm::Value*> args) {
        llvm::Value* res = builder_.CreateCall(fn, std::vector<llvm::Value*>(args));
        if (inst.result != il::kNoValue) values_[inst.result] = res;
    };

    // Every live proof this site does not own crosses the join it built: the
    // one proof-preserving edge carries it, every other predecessor kills it.
    // A null `fastBb` means nothing preserved anything, and every proof dies —
    // which is the honest answer for a site that reached a helper.
    auto carryOtherProofs = [&](const ProofJoin& join, bool ownsRead, bool ownsStore) {
        if (join.doneBb == nullptr) return;
        if (!ownsRead) rejoinReceiverProof(builder_, recvProof_, join.fastBb, join.doneBb);
        if (!ownsStore) rejoinStoreProof(storeProof_, join.fastBb, join.doneBb);
        proofsCarried_ = join.fastBb != nullptr;
    };

    switch (inst.op) {
        case il::Op::SuperGet: {
            if (!needs(2, false, "Invalid operands for SuperGet")) return false;
            const char* what = "Undefined operand in SuperGet instruction";
            llvm::Value* proto = operand(inst, 0, what);
            llvm::Value* thisArg = operand(inst, 1, what);
            if (!proto || !thisArg) return false;
            callWith(abi.bronze_super_get,
                     {proto, emitKeyId(builder_, shared_.tables, inst.keyIndex), thisArg});
            return true;
        }
        case il::Op::SuperSet: {
            if (!needs(3, false, "Invalid operands for SuperSet")) return false;
            const char* what = "Undefined operand in SuperSet instruction";
            llvm::Value* proto = operand(inst, 0, what);
            llvm::Value* thisArg = operand(inst, 1, what);
            llvm::Value* val = operand(inst, 2, what);
            if (!proto || !thisArg || !val) return false;
            builder_.CreateCall(abi.bronze_super_set,
                                {proto, emitKeyId(builder_, shared_.tables, inst.keyIndex),
                                 thisArg, val});
            return true;
        }
        case il::Op::PropDelete: {
            if (!needs(1, false, "Invalid operands for PropDelete")) return false;
            llvm::Value* target = operand(inst, 0, "Undefined operand in PropDelete instruction");
            if (!target) return false;
            callWith(abi.bronze_prop_delete,
                     {target, emitKeyId(builder_, shared_.tables, inst.keyIndex),
                      builder_.getInt1(inst.immI32 != 0)});
            return true;
        }
        case il::Op::ElemDelete: {
            if (!needs(2, false, "Invalid operands for ElemDelete")) return false;
            const char* what = "Undefined operand in ElemDelete instruction";
            llvm::Value* target = operand(inst, 0, what);
            llvm::Value* index = operand(inst, 1, what);
            if (!target || !index) return false;
            callWith(abi.bronze_elem_delete, {target, index, builder_.getInt1(inst.immI32 != 0)});
            return true;
        }

        case il::Op::PropGet: {
            if (!needs(1, true, "Invalid operands for PropGet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined object in PropGet instruction");
            if (!obj) return false;
            // A site inference proved monomorphic gets the guard inlined here;
            // an unproven one keeps the plain call, so the inline form never
            // grows into a polymorphic guard chain in the object file. This may
            // SPLIT the current block.
            const std::string& keyStr = inst.keyIndex < shared_.module.keyConstants.size()
                                            ? shared_.module.keyConstants[inst.keyIndex]
                                            : "";
            // Where this site sits in its block's receiver runs
            // (llvm_recv_proof.h). The run's FIRST member pays for the proof
            // in front of its own cache; the rest spend what it left, and a
            // site in no run passes nothing and emits exactly what it always
            // did.
            const ReceiverRunPlan::Site runSite = runPlan_.reads.at(currentILInst_);
            ReceiverProof* proofArg = nullptr;
            if (runSite.run == ReceiverRunPlan::kNoRun) {
                recvProof_ = ReceiverProof{};
            } else {
                if (runSite.establishes) {
                    recvProof_ = emitReceiverProof(builder_, obj, inst.operands[0], runSite.run,
                                                   runSite.runMaxIndex);
                }
                if (recvProof_.live() && recvProof_.run == runSite.run) proofArg = &recvProof_;
            }
            ProofJoin join;
            values_[inst.result] =
                emitPropGet(builder_, abi, globals_, shared_.tables, obj,
                            rootSlotAddrOf(inst, 0), inst.keyIndex, inst.icIndex,
                            inst.icMonomorphic, staticSiteOf(inst), keyStr, proofArg, &join);
            carryOtherProofs(join, /*ownsRead=*/proofArg != nullptr, /*ownsStore=*/false);
            if (inst.result < propGetKey_.size()) propGetKey_[inst.result] = inst.keyIndex;
            return true;
        }
        case il::Op::PropSet: {
            if (!needs(2, false, "Invalid operands for PropSet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in PropSet instruction");
            llvm::Value* val = operand(inst, 1, "Undefined operand in PropSet instruction");
            if (!obj || !val) return false;
            const std::string& keyStr = inst.keyIndex < shared_.module.keyConstants.size()
                                            ? shared_.module.keyConstants[inst.keyIndex]
                                            : "";
            // What the VALUE is made of decides which of stage R1's
            // representation tests this site emits (llvm_repr.h). Asked with
            // the store's POSITION, because a `pin.guard` standing immediately
            // in front of it proves the value on the path that reaches the
            // store and on no other.
            const ValueRepr valRepr = storeValueRepr(func_, repr_, currentILBlock_,
                                                     currentILInst_, inst.operands[1]);
            emitPropSet(builder_, abi, globals_, shared_.tables, obj, rootSlotAddrOf(inst, 0),
                        inst.keyIndex, val, inst.icIndex, inst.immI32 != 0, inst.icMonomorphic,
                        staticSiteOf(inst), valRepr, keyStr);
            return true;
        }
        case il::Op::ElemGet: {
            if (!needs(2, true, "Invalid operands for ElemGet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemGet instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemGet instruction");
            if (!obj || !idx) return false;
            values_[inst.result] = emitElemGet(builder_, abi, obj, idx);
            return true;
        }
        case il::Op::ElemSet: {
            if (!needs(3, false, "Invalid operands for ElemSet")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemSet instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemSet instruction");
            llvm::Value* val = operand(inst, 2, "Undefined operand in ElemSet instruction");
            if (!obj || !idx || !val) return false;

            // The STORE proof (llvm_store_proof.h), spent the same way the read
            // proof above is: the run's first member pays for the ladder, the
            // rest branch on what it left. The fast arm is emitted in front of
            // emitElemSet and joins it at a block of this emitter's own, so
            // llvm_elem.cpp keeps emitting exactly the ladder it always did.
            const StoreRunPlan::Site site = runPlan_.stores.at(currentILInst_);
            ProvenStore proven;
            llvm::BasicBlock* doneBb = nullptr;
            if (site.run != StoreRunPlan::kNoRun) {
                if (site.establishes) {
                    llvm::Value* baseDbl =
                        site.base < values_.size() ? values_[site.base] : nullptr;
                    storeProof_ = emitStoreProof(builder_, obj, baseDbl, inst.operands[0],
                                                 site.base, site.run, site.runMaxOffset);
                }
                if (storeProof_.live() && storeProof_.run == site.run) {
                    doneBb = llvm::BasicBlock::Create(shared_.ctx, "es.proven.done", llvmFunc_);
                    proven =
                        emitProvenElementStore(builder_, storeProof_, site.offset, val, doneBb);
                }
            }

            emitElemSet(builder_, abi, obj, idx, val, inst.immI32 != 0);

            if (doneBb != nullptr) {
                builder_.CreateBr(doneBb);
                builder_.SetInsertPoint(doneBb);
                ProofJoin join{proven.fastBb, doneBb};
                rejoinStoreProof(storeProof_, join.fastBb, join.doneBb);
                carryOtherProofs(join, /*ownsRead=*/false, /*ownsStore=*/true);
            }
            return true;
        }
        // The proven forms: `immI32` is the types::TypedArrayElem number, 0
        // for Float64Array and 1 for Float32Array — the only two lowering
        // emits (lower_typed_elem.cpp). The index and value are f64 SSA.
        case il::Op::ElemGetTyped: {
            if (!needs(2, true, "Invalid operands for ElemGetTyped")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemGetTyped instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemGetTyped instruction");
            if (!obj || !idx) return false;
            values_[inst.result] = emitTypedElemGet(builder_, shared_.abi, obj, idx,
                                                    static_cast<uint32_t>(inst.immI32));
            return true;
        }
        case il::Op::ElemSetTyped: {
            if (!needs(3, false, "Invalid operands for ElemSetTyped")) return false;
            llvm::Value* obj = operand(inst, 0, "Undefined operand in ElemSetTyped instruction");
            llvm::Value* idx = operand(inst, 1, "Undefined operand in ElemSetTyped instruction");
            llvm::Value* val = operand(inst, 2, "Undefined operand in ElemSetTyped instruction");
            if (!obj || !idx || !val) return false;
            emitTypedElemSet(builder_, shared_.abi, obj, idx, val,
                             static_cast<uint32_t>(inst.immI32));
            return true;
        }
        default:
            return require(false, "Instruction is not an access op");
    }
}

}  // namespace bronze::codegen_llvm
