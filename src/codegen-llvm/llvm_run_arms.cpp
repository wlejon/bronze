#include "codegen-llvm/llvm_run_arms.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>

#include "codegen-llvm/llvm_array_store_proof.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_prop_ic.h"
#include "codegen-llvm/llvm_recv_proof.h"
#include "codegen-llvm/llvm_store_proof.h"

namespace bronze::codegen_llvm {

namespace {

// The element index a member reads, or nothing for an instruction that is not
// a constant-index property read at all. Read from the KEY, exactly as the run
// planner reads it, so the two agree about what a member is.
std::optional<uint32_t> memberIndexOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return std::nullopt;
    if (inst.result == il::kNoValue) return std::nullopt;
    if (inst.operands.empty()) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return parseIndexKey(module.keyConstants[inst.keyIndex]);
}

}  // namespace

bool runArmsDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_RUN_ARMS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

RunArmPlan planRunArms(const il::Module& module, const il::Function& func) {
    RunArmPlan plan;
    const size_t blockCount = func.blocks.size();
    plan.blockBase.assign(blockCount + 1, 0);
    size_t total = 0;
    for (size_t b = 0; b < blockCount; ++b) {
        plan.blockBase[b] = static_cast<uint32_t>(total);
        total += func.blocks[b].instructions.size();
    }
    plan.blockBase[blockCount] = static_cast<uint32_t>(total);
    plan.startOf.assign(total, RunArmPlan::kNoGroup);
    plan.memberOf.assign(total, RunArmPlan::kNoGroup);
    if (runArmsDisabled() || !receiverProofEnabled()) return plan;

    for (size_t b = 0; b < blockCount; ++b) {
        const il::Block& block = func.blocks[b];
        const BlockRunPlan runs = planBlockRuns(module, func, b);
        if (runs.reads.empty()) continue;

        // One pass over the block, opening a candidate at every member that
        // establishes a run and closing it at the first instruction that is not
        // the next member of that same run. A run whose members are not
        // consecutive therefore closes at the gap and is refused below, which is
        // the rule the header states.
        size_t i = 0;
        while (i < block.instructions.size()) {
            const ReceiverRunPlan::Site head = runs.reads.at(i);
            if (head.run == ReceiverRunPlan::kNoRun || !head.establishes) {
                ++i;
                continue;
            }
            RunArmGroup group;
            group.block = static_cast<uint32_t>(b);
            group.first = static_cast<uint32_t>(i);
            group.run = head.run;
            group.maxIndex = head.runMaxIndex;
            group.receiver = block.instructions[i].operands[0];

            size_t j = i;
            bool refused = false;
            for (; j < block.instructions.size(); ++j) {
                const il::Instruction& inst = block.instructions[j];
                if (runs.reads.at(j).run != head.run) break;
                const std::optional<uint32_t> idx = memberIndexOf(module, inst);
                // A member the emitter could not turn into a load, or one whose
                // site claims a static slot: the group is refused outright
                // rather than cut short, because the run's proof is established
                // at its first member and a cut group would leave the rest of
                // the run spending a proof this planner never described.
                if (!idx.has_value() || inst.operands[0] != group.receiver ||
                    inst.staticSlot != il::Instruction::kNoStaticSlot) {
                    refused = true;
                    break;
                }
                group.index.push_back(*idx);
                group.result.push_back(inst.result);
            }
            group.last = static_cast<uint32_t>(j == i ? i : j - 1);

            // Everything the run holds IN THIS BLOCK has to be inside the span
            // just walked. A member after the gap means the run is interleaved
            // with something else, and interleaved is the case the per-member
            // shape already handles.
            for (size_t k = j; !refused && k < block.instructions.size(); ++k) {
                if (runs.reads.at(k).run == head.run) refused = true;
            }
            if (!refused && group.size() >= 2) {
                const uint32_t id = static_cast<uint32_t>(plan.groups.size());
                plan.startOf[plan.blockBase[b] + group.first] = id;
                for (uint32_t k = group.first; k <= group.last; ++k) {
                    plan.memberOf[plan.blockBase[b] + k] = id;
                }
                plan.groups.push_back(std::move(group));
            }
            i = j > i ? j : i + 1;
        }
    }
    return plan;
}

bool FunctionEmitter::emitRunArmGroup(const RunArmGroup& group) {
    const std::string tag = "run" + std::to_string(group.run) + ".";

    // The receiver out of its slot. The proof derives a pointer FROM it, and a
    // pointer derived from a register the collector has moved out from under
    // points into the place the object used to be — so this is the one reload
    // the group does not try to skip.
    currentILInst_ = group.first;
    reload(group.receiver, false, LiveRootPlan::kReload);
    llvm::Value* obj = values_[group.receiver];
    if (!require(obj != nullptr, "Undefined receiver for a proven element run")) return false;

    // Every restored value into a register that dominates both arms, BEFORE the
    // proof branches. The plan names the block whose emission wrote each one and
    // the emitter holds the block that actually did; where the two agree the
    // register is the answer and nothing is emitted, which is every case the
    // plan describes. Where they do not — a block the plan's meet never saw was
    // emitted in between and overwrote the entry — the register names a
    // definition that reaches neither arm, and the slot every reloading use goes
    // to is what the join has to phi instead. An ARM-LOCAL value has no such
    // slot to fall back on, by construction: its own group's fast arm skipped
    // the store because the plan said no use would ever read one. So a mismatch
    // there is the plan contradicting itself, and it is diagnosed rather than
    // compiled into a load of a word nothing wrote.
    for (size_t k = 0; k < group.restore.size(); ++k) {
        const il::ValueId v = group.restore[k];
        if (v >= func_.valueCount || slotOf_[v] == kNoSlot) continue;
        if (regBlock_[v] == group.restoreAnchor[k] && values_[v] != nullptr) continue;
        if (!require(!live_.armLocal(v),
                     "Live-root plan promised a register for an arm-local value")) {
            return false;
        }
        reload(v, /*holeInsensitive=*/false, LiveRootPlan::kReload);
    }

    ReceiverProof proof =
        emitReceiverProof(builder_, obj, group.receiver, group.run, group.maxIndex);
    const StoreProof enteringStore = storeProof_;
    const ArrayStoreProof enteringArrayStore = arrayStoreProof_;

    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(shared_.ctx, tag + "fast", llvmFunc_);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(shared_.ctx, tag + "slow", llvmFunc_);
    llvm::BasicBlock* joinBb = llvm::BasicBlock::Create(shared_.ctx, tag + "join", llvmFunc_);
    builder_.CreateCondBr(proof.ok, fastBb, slowBb);

    // ---- the fast arm: loads, and not one other thing ----------------------
    //
    // No call, no allocation, no exception test — so nothing here is a
    // safepoint, no result needs its slot written, and every register that was
    // live coming in is still live and still correct going out.
    builder_.SetInsertPoint(fastBb);
    std::vector<llvm::Value*> fastResult;
    fastResult.reserve(group.size());
    for (size_t m = 0; m < group.size(); ++m) {
        const il::ValueId res = group.result[m];
        const bool raw = holeRawSlotEnabled() && res < holeRawSafe_.size() &&
                         holeRawSafe_[res] != 0;
        fastResult.push_back(emitElementLoad(builder_, proof, group.index[m], raw));
        // The slot the join may write below then holds the element's own bits,
        // and every reload corrects them — which is sound here for the reason
        // `holeRawSafe_` names and for no weaker one.
        if (raw && slotOf_[res] != kNoSlot) holeRawSlot_[res] = 1;
    }
    std::vector<llvm::Value*> fastRestore;
    fastRestore.reserve(group.restore.size());
    for (il::ValueId v : group.restore) fastRestore.push_back(values_[v]);
    llvm::BasicBlock* fastExit = builder_.GetInsertBlock();
    builder_.CreateBr(joinBb);

    // ---- the slow arm: exactly the per-member emission, in one block -------
    //
    // The proofs are dead in here: this arm was selected because the receiver
    // failed the ladder, and the store proofs cannot survive the ladders that
    // follow. `values_` and `regBlock_` are put back afterwards because the
    // registers this arm writes live in blocks that reach the join and nothing
    // past it — the join's phis are what the rest of the function reads.
    builder_.SetInsertPoint(slowBb);
    // What the arm is about to put at risk, into the slots that will forward it.
    // A value the rest of the function keeps in its slot is already there — this
    // is the price for the ones that are NOT, the run results of an earlier
    // group whose fast arm wrote nothing at all, and it is paid on the arm that
    // collects rather than on the arm that does not.
    for (il::ValueId v : group.restore) {
        if (slotOf_[v] != kNoSlot && values_[v] != nullptr) {
            builder_.CreateStore(values_[v], slotAddr(slotOf_[v]));
        }
    }
    std::vector<llvm::Value*> savedValues = values_;
    std::vector<uint32_t> savedRegBlock = regBlock_;
    recvProof_ = ReceiverProof{};
    storeProof_ = StoreProof{};
    arrayStoreProof_ = ArrayStoreProof{};
    inRunArm_ = true;
    for (uint32_t k = group.first; k <= group.last; ++k) {
        if (!emitInstructionAt(group.block, k, /*forceReload=*/true)) {
            inRunArm_ = false;
            return false;
        }
    }
    inRunArm_ = false;
    // Out of the slots, after the last ladder that could have moved anything.
    // A value with no slot could not have moved at all, and its register is the
    // answer.
    auto currentValue = [&](il::ValueId v) -> llvm::Value* {
        if (v == il::kNoValue || v >= func_.valueCount) return nullptr;
        if (slotOf_[v] == kNoSlot) return values_[v];
        return builder_.CreateLoad(i64Ty_, slotAddr(slotOf_[v]));
    };
    std::vector<llvm::Value*> slowResult;
    slowResult.reserve(group.size());
    for (il::ValueId r : group.result) slowResult.push_back(currentValue(r));
    std::vector<llvm::Value*> slowRestore;
    slowRestore.reserve(group.restore.size());
    for (il::ValueId v : group.restore) slowRestore.push_back(currentValue(v));
    llvm::BasicBlock* slowExit = builder_.GetInsertBlock();
    builder_.CreateBr(joinBb);
    values_ = std::move(savedValues);
    regBlock_ = std::move(savedRegBlock);

    // ---- the join ----------------------------------------------------------
    builder_.SetInsertPoint(joinBb);
    auto join = [&](llvm::Value* fast, llvm::Value* slow, il::ValueId v, const std::string& name) {
        if (fast == nullptr || slow == nullptr) return;
        // A value both arms hand over unchanged is a value neither arm could
        // have moved — it has no root slot, so no collection forwards it and no
        // reload rewrites it. The phi would be its own operand twice.
        if (fast == slow) return;
        llvm::PHINode* phi = builder_.CreatePHI(fast->getType(), 2, name + std::to_string(v));
        phi->addIncoming(fast, fastExit);
        phi->addIncoming(slow, slowExit);
        values_[v] = phi;
        regBlock_[v] = group.block;
    };
    for (size_t m = 0; m < group.size(); ++m) {
        join(fastResult[m], slowResult[m], group.result[m], tag + "e");
    }
    for (size_t k = 0; k < group.restore.size(); ++k) {
        join(fastRestore[k], slowRestore[k], group.restore[k], tag + "live");
    }
    // The root stores only once every phi is in place: a phi has to be at the
    // top of its block, and a store between two of them is not.
    //
    // The fast arm wrote no slot at all. Where anything outside the slow arm can
    // read one — a use the plan sends to the slot, a handler, an access site that
    // takes its address — the join is where it becomes current, which is one
    // store on a path that would otherwise have paid one per member and one per
    // collection between them.
    for (const il::ValueId res : group.result) {
        if (slotOf_[res] != kNoSlot && !live_.armLocal(res) && values_[res] != nullptr) {
            builder_.CreateStore(values_[res], slotAddr(slotOf_[res]));
        }
    }

    // Every proof the group carried across its own join, on the fast edge only.
    // The receiver proof is the group's own and dies here unless a later block
    // continues its run; the store proofs belong to somebody else's run and are
    // carried exactly as a single member would have carried them.
    recvProof_ = proof;
    storeProof_ = enteringStore;
    arrayStoreProof_ = enteringArrayStore;
    rejoinReceiverProof(builder_, recvProof_, fastExit, joinBb);
    rejoinStoreProof(storeProof_, fastExit, joinBb);
    rejoinArrayStoreProof(arrayStoreProof_, fastExit, joinBb);
    proofsCarried_ = true;
    currentILInst_ = group.last;
    return true;
}

}  // namespace bronze::codegen_llvm
