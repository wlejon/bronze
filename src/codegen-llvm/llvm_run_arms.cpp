#include "codegen-llvm/llvm_run_arms.h"
#include "il/key.h"

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

// The element index a read member reads, or nothing for an instruction that is
// not a constant-index property read at all. Read from the KEY, exactly as the
// run planner reads it, so the two agree about what a member is.
std::optional<uint32_t> readIndexOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return std::nullopt;
    if (inst.result == il::kNoValue) return std::nullopt;
    if (inst.operands.empty()) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return il::parseIndexKey(module.keyConstants[inst.keyIndex]);
}

// The element index an Array store member writes. Only the `prop.set` spelling:
// the PINNED `elem.set.typed` form is a member of the same run (llvm_recv_proof.cpp,
// `arrayStoreIndexOf`) but its unproven arm is a raw double store rather than a
// cache, so duplicating it would mean two spellings of one store inside one
// arm. It ends the span instead, and a pinned build keeps exactly the
// per-member shape it had.
std::optional<uint32_t> arrayStoreIndexOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropSet) return std::nullopt;
    if (inst.operands.size() < 2) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return il::parseIndexKey(module.keyConstants[inst.keyIndex]);
}

}  // namespace

bool runArmsDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_RUN_ARMS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

bool interleavedRunArmsEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("BRONZE_NO_RUN_ARMS_INTERLEAVED");
        return env == nullptr || std::strcmp(env, "1") != 0;
    }();
    return on;
}

bool typedStoreRunArmsEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("BRONZE_NO_RUN_ARMS_TYPED_STORES");
        return env == nullptr || std::strcmp(env, "1") != 0;
    }();
    return on;
}

bool runArmDuplicable(const il::Instruction& inst) {
    if (il::isTerminator(inst.op)) return false;
    // The two effect predicates first, because both are written the safe way
    // round: an op added tomorrow answers yes to them and is refused here
    // without anybody having to remember this list.
    if (il::canCollect(inst) || il::canThrow(inst)) return false;
    switch (inst.op) {
        // Machine constants: a bit pattern materialised inline.
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        // Arithmetic and the bitwise family. The predicates above have already
        // refused the BOXED spelling of each, which reaches ToPrimitive; what
        // is left is machine instructions on operands already in hand.
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Neg:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::ToInt32:
        // The number compares and the total predicates: register work, no
        // branch out of the arm.
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::NumTruthy:
        case il::Op::StrictEq:
        case il::Op::IsNullish:
        case il::Op::IsNumber:
        // A non-string box is a bitcast and a select; a raw or nullish-widened
        // unbox is a bitcast and a compare. `canCollect` has refused the string
        // box and the checked unbox, which are the two that call.
        case il::Op::Box:
        case il::Op::Unbox:
            return true;
        // Everything else, including the pin barrier: its branching form
        // neither collects nor throws, but its violating arm LEAVES for the
        // handler, and a handler reads root slots the fast arm has not written.
        default:
            return false;
    }
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
    const bool spans = interleavedRunArmsEnabled();
    const bool typedStores = spans && typedStoreRunArmsEnabled() && storeProofEnabled();

    for (size_t b = 0; b < blockCount; ++b) {
        const il::Block& block = func.blocks[b];
        // A group buys a branch with code size, and a guarded region's SLOW
        // copy is the arm the region exists not to take: whatever is spent
        // there sits in the same function as the fast copy and is paid for out
        // of the same instruction cache. So the fast copy and the unguarded
        // code get groups and the slow copy gets the per-member shape. Measured
        // on `instanced_mesh_churn`, whose `Matrix4.compose` carries both
        // copies of one region.
        if (spans && block.copyClass == il::CopyClass::Slow) continue;
        const size_t n = block.instructions.size();
        const BlockRunPlan runs = planBlockRuns(module, func, b);
        if (runs.reads.empty()) continue;

        // What kind of member instruction `i` is, and which run it belongs to.
        // A site is a read member and an Array-store member never at once: one
        // is a `prop.get` and the other a `prop.set`.
        // `index` is filled here rather than re-derived at the use, so the
        // element the group records is the element this answer was about.
        auto memberAt = [&](size_t i, RunArmProof& out, uint32_t& index) -> bool {
            const il::Instruction& inst = block.instructions[i];
            const ReceiverRunPlan::Site read = runs.reads.at(i);
            if (read.run != ReceiverRunPlan::kNoRun) {
                if (const std::optional<uint32_t> idx = readIndexOf(module, inst)) {
                    out.kind = RunArmProof::Kind::Read;
                    out.run = read.run;
                    out.maxIndex = read.runMaxIndex;
                    out.receiver = inst.operands[0];
                    index = *idx;
                    return true;
                }
            }
            // A store member is a member only where spans are on: the shape
            // that stood before them is a run of READS and nothing else, and
            // the seam has to give that back exactly.
            const ArrayStoreRunPlan::Site store =
                spans ? runs.arrayStores.at(i) : ArrayStoreRunPlan::Site{};
            if (store.run != ArrayStoreRunPlan::kNoRun &&
                arrayStoreIndexOf(module, inst).has_value()) {
                out.kind = RunArmProof::Kind::ArrayStore;
                out.run = store.run;
                out.maxIndex = store.runMaxIndex;
                out.receiver = inst.operands[0];
                index = store.index;
                return true;
            }
            // A typed-array store member. Its proven arm owes a value test, so
            // it is a member only where the GATE that pays for that test may be
            // built — which is the seam above and nothing else.
            const StoreRunPlan::Site typed =
                typedStores ? runs.stores.at(i) : StoreRunPlan::Site{};
            if (typed.run != StoreRunPlan::kNoRun && inst.op == il::Op::ElemSet &&
                inst.operands.size() >= 3) {
                out.kind = RunArmProof::Kind::TypedStore;
                out.run = typed.run;
                out.maxIndex = typed.runMaxOffset;
                out.receiver = inst.operands[0];
                out.base = typed.base;
                index = typed.offset;
                return true;
            }
            return false;
        };
        auto startsSpan = [&](size_t i) {
            RunArmProof pf;
            uint32_t index = 0;
            return memberAt(i, pf, index);
        };

        size_t i = 0;
        while (i < n) {
            if (!startsSpan(i)) {
                ++i;
                continue;
            }
            RunArmGroup group;
            group.block = static_cast<uint32_t>(b);
            group.first = static_cast<uint32_t>(i);

            // The scan. A member of a run this group has already opened, or one
            // that opens its own; a duplicable instruction the fast arm may
            // copy; anything else stops the span where it stands.
            size_t j = i;
            size_t last = i;
            bool refused = false;
            for (; j < n && !refused; ++j) {
                const il::Instruction& inst = block.instructions[j];
                RunArmProof pf;
                uint32_t index = 0;
                if (memberAt(j, pf, index)) {
                    // A member the emitter could not turn into a bare access,
                    // or one whose site claims a static slot: the span stops in
                    // front of it and the check below decides whether what has
                    // been collected so far is still a whole run.
                    if (inst.staticSlot != il::Instruction::kNoStaticSlot) break;
                    uint32_t at = RunArmStep::kNoProof;
                    for (size_t p = 0; p < group.proofs.size(); ++p) {
                        if (group.proofs[p].kind == pf.kind && group.proofs[p].run == pf.run) {
                            at = static_cast<uint32_t>(p);
                        }
                    }
                    if (at == RunArmStep::kNoProof) {
                        bool establishes = false;
                        switch (pf.kind) {
                            case RunArmProof::Kind::Read:
                                establishes = runs.reads.at(j).establishes;
                                break;
                            case RunArmProof::Kind::ArrayStore:
                                establishes = runs.arrayStores.at(j).establishes;
                                break;
                            case RunArmProof::Kind::TypedStore:
                                establishes = runs.stores.at(j).establishes;
                                break;
                        }
                        // A run chained in from an earlier block, or a second
                        // run under the pre-span rule: either way the span
                        // stops here rather than testing a proof it did not
                        // emit.
                        if (!establishes) break;
                        if (!spans && !group.proofs.empty()) break;
                        // An Array store and a typed-array store in one span
                        // would put the gate's hoisted loads above a store that
                        // could be into the very Array they read (`m.copy(m)`),
                        // and the header's aliasing argument covers only the
                        // typed one. So the mix ends the span.
                        bool clash = false;
                        for (const RunArmProof& held : group.proofs) {
                            const bool a = held.kind == RunArmProof::Kind::ArrayStore;
                            const bool t = held.kind == RunArmProof::Kind::TypedStore;
                            if (a && pf.kind == RunArmProof::Kind::TypedStore) clash = true;
                            if (t && pf.kind == RunArmProof::Kind::ArrayStore) clash = true;
                        }
                        if (clash) break;
                        at = static_cast<uint32_t>(group.proofs.size());
                        group.proofs.push_back(pf);
                    }
                    RunArmStep step;
                    step.inst = static_cast<uint32_t>(j);
                    step.proof = at;
                    step.index = index;
                    switch (pf.kind) {
                        case RunArmProof::Kind::Read:
                            group.result.push_back(inst.result);
                            break;
                        case RunArmProof::Kind::ArrayStore:
                            step.value = inst.operands[1];
                            break;
                        case RunArmProof::Kind::TypedStore:
                            step.value = inst.operands[2];
                            break;
                    }
                    group.steps.push_back(step);
                    last = j;
                    continue;
                }
                if (!spans || !runArmDuplicable(inst)) break;
                RunArmStep step;
                step.inst = static_cast<uint32_t>(j);
                group.steps.push_back(step);
                if (inst.result != il::kNoValue) group.result.push_back(inst.result);
            }
            group.last = static_cast<uint32_t>(last);
            // Trailing duplicables buy nothing and would only widen the join.
            while (!group.steps.empty() && group.steps.back().inst > group.last) {
                if (block.instructions[group.steps.back().inst].result != il::kNoValue) {
                    group.result.pop_back();
                }
                group.steps.pop_back();
            }

            // Everything each covered run holds IN THIS BLOCK has to be inside
            // the span just walked. A member after the end means the run is cut,
            // and a cut run would leave the rest of it spending a proof this
            // planner never described.
            for (size_t k = group.last + 1; !refused && k < n; ++k) {
                for (const RunArmProof& pf : group.proofs) {
                    // The three run numberings are separate namespaces, so the
                    // comparison is inside one kind and never across them.
                    uint32_t run = ReceiverRunPlan::kNoRun;
                    switch (pf.kind) {
                        case RunArmProof::Kind::Read:
                            run = runs.reads.at(k).run;
                            break;
                        case RunArmProof::Kind::ArrayStore:
                            run = runs.arrayStores.at(k).run;
                            break;
                        case RunArmProof::Kind::TypedStore:
                            run = runs.stores.at(k).run;
                            break;
                    }
                    if (run == pf.run) refused = true;
                }
            }
            // A proof's ladder stands at the group's head, so its receiver —
            // and, for a typed-array store run, the base its one length test is
            // taken against — has to be a value the head already has. Either
            // one defined by the span itself would be read by the ladder before
            // it exists.
            for (uint32_t k = group.first; !refused && k <= group.last; ++k) {
                const il::ValueId def = block.instructions[k].result;
                if (def == il::kNoValue) continue;
                for (const RunArmProof& pf : group.proofs) {
                    if (pf.receiver == def || pf.base == def) refused = true;
                }
            }

            // THE GATE. Backwards over the span, so one pass closes the set:
            // a step is in the gate when it produces a value some typed-array
            // store writes, or one that another gate step is made of. A store
            // defines nothing, so no store is ever pulled in — which is what
            // makes the gate free of the very effect it stands in front of.
            for (const RunArmProof& pf : group.proofs) {
                if (pf.kind == RunArmProof::Kind::TypedStore) group.gated = true;
            }
            if (group.gated) {
                std::vector<uint8_t> wanted(func.valueCount, 0);
                auto want = [&](il::ValueId v) {
                    if (v != il::kNoValue && v < func.valueCount) wanted[v] = 1;
                };
                for (const RunArmStep& step : group.steps) {
                    if (step.proof != RunArmStep::kNoProof &&
                        group.proofs[step.proof].kind == RunArmProof::Kind::TypedStore) {
                        want(step.value);
                    }
                }
                for (size_t k = group.steps.size(); k-- > 0;) {
                    RunArmStep& step = group.steps[k];
                    if (step.proof != RunArmStep::kNoProof &&
                        group.proofs[step.proof].kind != RunArmProof::Kind::Read) {
                        continue;
                    }
                    const il::Instruction& def = block.instructions[step.inst];
                    if (def.result == il::kNoValue || def.result >= func.valueCount) continue;
                    if (wanted[def.result] == 0) continue;
                    step.gate = true;
                    for (const il::ValueId operand : def.operands) want(operand);
                }
            }

            if (!refused && group.memberCount() >= 2) {
                const uint32_t id = static_cast<uint32_t>(plan.groups.size());
                plan.startOf[plan.blockBase[b] + group.first] = id;
                for (uint32_t k = group.first; k <= group.last; ++k) {
                    plan.memberOf[plan.blockBase[b] + k] = id;
                }
                plan.groups.push_back(std::move(group));
                i = static_cast<size_t>(plan.groups.back().last) + 1;
                continue;
            }
            i = j > i ? j : i + 1;
        }
    }
    return plan;
}

bool FunctionEmitter::emitRunArmGroup(const RunArmGroup& group) {
    const il::Block& block = func_.blocks[group.block];
    const std::string tag =
        "arm" + std::to_string(group.block) + "_" + std::to_string(group.first) + ".";

    // ---- every covered run's ladder, at the head, and one test --------------
    //
    // The receivers come out of their slots. A proof derives a pointer FROM one,
    // and a pointer derived from a register the collector has moved out from
    // under points into the place the object used to be — so this is the one
    // reload the group does not try to skip.
    currentILInst_ = group.first;
    std::vector<ReceiverProof> reads(group.proofs.size());
    std::vector<ArrayStoreProof> arrayStores(group.proofs.size());
    std::vector<StoreProof> typedStores(group.proofs.size());
    llvm::Value* ok = nullptr;
    for (size_t p = 0; p < group.proofs.size(); ++p) {
        const RunArmProof& pf = group.proofs[p];
        reload(pf.receiver, false, LiveRootPlan::kReload);
        llvm::Value* obj = values_[pf.receiver];
        if (!require(obj != nullptr, "Undefined receiver for a proven element run")) return false;
        llvm::Value* mine = nullptr;
        switch (pf.kind) {
            case RunArmProof::Kind::Read:
                reads[p] = emitReceiverProof(builder_, obj, pf.receiver, pf.run, pf.maxIndex);
                mine = reads[p].ok;
                break;
            case RunArmProof::Kind::ArrayStore:
                arrayStores[p] =
                    emitArrayStoreProof(builder_, obj, pf.receiver, pf.run, pf.maxIndex);
                mine = arrayStores[p].ok;
                break;
            case RunArmProof::Kind::TypedStore: {
                // The base is an f64 SSA value, so it has no root slot and
                // nothing can have moved it — its register is the whole answer,
                // exactly as it is at the per-member site.
                llvm::Value* baseDbl =
                    pf.base < values_.size() ? values_[pf.base] : nullptr;
                if (!require(baseDbl != nullptr, "Undefined base for a proven store run")) {
                    return false;
                }
                typedStores[p] = emitStoreProof(builder_, obj, baseDbl, pf.receiver, pf.base,
                                                pf.run, pf.maxIndex);
                mine = typedStores[p].ok;
                break;
            }
        }
        if (!require(mine != nullptr, "A run-arm proof the emitter could not build")) return false;
        ok = ok == nullptr ? mine : builder_.CreateAnd(ok, mine, tag + "ok");
    }
    if (!require(ok != nullptr, "A run-arm group with no proof")) return false;

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

    const StoreProof enteringStore = storeProof_;
    const ArrayStoreProof enteringArrayStore = arrayStoreProof_;
    const ReceiverProof enteringRead = recvProof_;

    // The GATE, where the span holds a typed-array store: the loads and the
    // arithmetic those stores' values are made of, and then one test that every
    // one of them is already a Number. It stands between the proof branch and
    // the fast arm, so a refusal there has stored NOTHING and the slow arm
    // performs the whole span from the top (llvm_run_arms.h).
    llvm::BasicBlock* gateBb =
        group.gated ? llvm::BasicBlock::Create(shared_.ctx, tag + "gate", llvmFunc_) : nullptr;
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(shared_.ctx, tag + "fast", llvmFunc_);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(shared_.ctx, tag + "slow", llvmFunc_);
    llvm::BasicBlock* joinBb = llvm::BasicBlock::Create(shared_.ctx, tag + "join", llvmFunc_);
    builder_.CreateCondBr(ok, gateBb != nullptr ? gateBb : fastBb, slowBb);

    // ---- the fast path: the span, and not one other thing -------------------
    //
    // Nothing `il::canCollect` admits and no exception test — a helper that
    // allocates nothing is still allowed, and `bronze_strict_eq` is one — so
    // nothing here is a safepoint, no result needs its slot written, and every
    // register that was live coming in is still live and still correct going
    // out.
    std::vector<llvm::Value*> savedValues = values_;
    std::vector<uint32_t> savedRegBlock = regBlock_;
    // Values the arm has already defined. An operand in this set is read out of
    // the register the arm just produced; anything else comes out of its slot,
    // which is current because nothing between the def and here could collect.
    std::vector<uint8_t> armDefined(func_.valueCount, 0);
    auto armReload = [&](il::ValueId v, bool holeInsensitive) {
        if (v == il::kNoValue || v >= func_.valueCount) return;
        if (armDefined[v] != 0) return;
        reload(v, holeInsensitive, LiveRootPlan::kReload);
    };
    // One step of the span, wherever it is being emitted. The gate emits the
    // steps it holds and the fast arm emits the rest, so this is written once
    // and the two agree about every one of them by construction.
    auto emitStep = [&](const RunArmStep& step) -> bool {
        const il::Instruction& inst = block.instructions[step.inst];
        currentILInst_ = step.inst;
        if (step.proof == RunArmStep::kNoProof) {
            for (size_t k = 0; k < inst.operands.size(); ++k) {
                armReload(inst.operands[k], k == 0 && holeInsensitiveUse(inst, inst.operands[k]));
            }
            proofsCarried_ = false;
            if (!emitInstruction(inst)) return false;
        } else if (group.proofs[step.proof].kind == RunArmProof::Kind::Read) {
            const il::ValueId res = inst.result;
            const bool raw =
                holeRawSlotEnabled() && res < holeRawSafe_.size() && holeRawSafe_[res] != 0;
            values_[res] = emitElementLoad(builder_, reads[step.proof], step.index, raw);
            // The slot the join may write below then holds the element's own
            // bits, and every reload corrects them — which is sound here for the
            // reason `holeRawSafe_` names and for no weaker one.
            if (raw && slotOf_[res] != kNoSlot) holeRawSlot_[res] = 1;
        } else {
            armReload(step.value, false);
            llvm::Value* val = values_[step.value];
            if (!require(val != nullptr, "Undefined value in a proven element store")) return false;
            if (group.proofs[step.proof].kind == RunArmProof::Kind::ArrayStore) {
                emitElementStore(builder_, arrayStores[step.proof], step.index, val);
            } else {
                // The gate has already proven this value is a Number, so what
                // is left is the store the per-member arm performs behind its
                // own test — the same function, so the same conversion.
                emitTypedElementStore(builder_, typedStores[step.proof], step.index, val,
                                      tag + "e" + std::to_string(step.index) + ".");
            }
        }
        if (inst.result != il::kNoValue && inst.result < func_.valueCount &&
            values_[inst.result] != nullptr) {
            regBlock_[inst.result] = group.block;
            armDefined[inst.result] = 1;
        }
        return true;
    };

    if (gateBb != nullptr) {
        builder_.SetInsertPoint(gateBb);
        for (const RunArmStep& step : group.steps) {
            if (step.gate && !emitStep(step)) return false;
        }
        llvm::Value* numeric = nullptr;
        for (const RunArmStep& step : group.steps) {
            if (step.proof == RunArmStep::kNoProof) continue;
            if (group.proofs[step.proof].kind != RunArmProof::Kind::TypedStore) continue;
            armReload(step.value, false);
            llvm::Value* val = values_[step.value];
            if (!require(val != nullptr, "Undefined value in a proven element store")) return false;
            llvm::Value* isNum = emitStoreValueIsNumber(builder_, val, tag + "isnum");
            numeric = numeric == nullptr ? isNum : builder_.CreateAnd(numeric, isNum, tag + "num");
        }
        if (!require(numeric != nullptr, "A gated run-arm group with no typed store")) return false;
        builder_.CreateCondBr(numeric, fastBb, slowBb);
    }

    builder_.SetInsertPoint(fastBb);
    for (const RunArmStep& step : group.steps) {
        if (step.gate) continue;
        if (!emitStep(step)) return false;
    }
    std::vector<llvm::Value*> fastResult;
    fastResult.reserve(group.result.size());
    for (il::ValueId v : group.result) fastResult.push_back(values_[v]);
    std::vector<llvm::Value*> fastRestore;
    fastRestore.reserve(group.restore.size());
    for (il::ValueId v : group.restore) fastRestore.push_back(values_[v]);
    llvm::BasicBlock* fastExit = builder_.GetInsertBlock();
    builder_.CreateBr(joinBb);
    values_ = savedValues;
    regBlock_ = savedRegBlock;

    // ---- the slow arm: exactly the per-member emission, in one block -------
    //
    // The proofs are dead in here: this arm was selected because one of the
    // ladders failed, and no proof can survive the ladders that follow.
    // `values_` and `regBlock_` are put back afterwards because the registers
    // this arm writes live in blocks that reach the join and nothing past it —
    // the join's phis are what the rest of the function reads.
    builder_.SetInsertPoint(slowBb);
    // What the arm is about to put at risk, into the slots that will forward it.
    // A value the rest of the function keeps in its slot is already there — this
    // is the price for the ones that are NOT, the results of an earlier group
    // whose fast arm wrote nothing at all, and it is paid on the arm that
    // collects rather than on the arm that does not.
    for (il::ValueId v : group.restore) {
        if (slotOf_[v] != kNoSlot && values_[v] != nullptr) {
            builder_.CreateStore(values_[v], slotAddr(slotOf_[v]));
        }
    }
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
    slowResult.reserve(group.result.size());
    for (il::ValueId v : group.result) slowResult.push_back(currentValue(v));
    std::vector<llvm::Value*> slowRestore;
    slowRestore.reserve(group.restore.size());
    for (il::ValueId v : group.restore) slowRestore.push_back(currentValue(v));
    llvm::BasicBlock* slowExit = builder_.GetInsertBlock();
    builder_.CreateBr(joinBb);
    values_ = std::move(savedValues);
    regBlock_ = std::move(savedRegBlock);

    // ---- the join ----------------------------------------------------------
    builder_.SetInsertPoint(joinBb);
    auto join = [&](llvm::Value* fast, llvm::Value* slow, il::ValueId v, const std::string& name,
                    bool defined) {
        if (fast == nullptr || slow == nullptr) return;
        // A value both arms hand over unchanged is a value neither arm could
        // have moved — it has no root slot, so no collection forwards it and no
        // reload rewrites it, and the phi would be its own operand twice. A
        // value that entered the group keeps the register it entered with; one
        // the SPAN defined has no such register, and the two arms agreeing about
        // it is exactly what makes either answer legal after the join. That is
        // the `const.f64` a duplicated instruction reads, which LLVM hands both
        // arms as one Constant.
        if (fast == slow) {
            if (defined) {
                values_[v] = fast;
                regBlock_[v] = group.block;
            }
            return;
        }
        llvm::PHINode* phi = builder_.CreatePHI(fast->getType(), 2, name + std::to_string(v));
        phi->addIncoming(fast, fastExit);
        phi->addIncoming(slow, slowExit);
        values_[v] = phi;
        regBlock_[v] = group.block;
    };
    for (size_t m = 0; m < group.result.size(); ++m) {
        join(fastResult[m], slowResult[m], group.result[m], tag + "v", /*defined=*/true);
    }
    for (size_t k = 0; k < group.restore.size(); ++k) {
        join(fastRestore[k], slowRestore[k], group.restore[k], tag + "live", /*defined=*/false);
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
    // A proof the group ESTABLISHED is the group's own and lives on for a later
    // block that continues its run; one the group did not establish belongs to
    // somebody else's run and is carried exactly as a single member would have
    // carried it. Where the group established several of a kind the last one
    // stands, because a proof is a single live value and the run the block
    // continues into can only be the one that ended last.
    recvProof_ = enteringRead;
    storeProof_ = enteringStore;
    arrayStoreProof_ = enteringArrayStore;
    for (size_t p = 0; p < group.proofs.size(); ++p) {
        switch (group.proofs[p].kind) {
            case RunArmProof::Kind::Read:
                recvProof_ = reads[p];
                break;
            case RunArmProof::Kind::ArrayStore:
                arrayStoreProof_ = arrayStores[p];
                break;
            case RunArmProof::Kind::TypedStore:
                storeProof_ = typedStores[p];
                break;
        }
    }
    rejoinReceiverProof(builder_, recvProof_, fastExit, joinBb);
    rejoinStoreProof(storeProof_, fastExit, joinBb);
    rejoinArrayStoreProof(arrayStoreProof_, fastExit, joinBb);
    proofsCarried_ = true;
    currentILInst_ = group.last;
    return true;
}

}  // namespace bronze::codegen_llvm
