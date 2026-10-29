#include "lower/transition_runs.h"

#include "abi/bronze_abi.h"

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bronze::lower {

namespace {

// A key whose write on a plain object is an ordinary data-property add. An
// integer-like name is an element of an array receiver, and the run's guard
// would refuse every array anyway; `__proto__` is Object.prototype's accessor.
bool runKey(const std::string& key) {
    if (key.empty() || key == "__proto__") return false;
    for (char c : key) {
        if (c < '0' || c > '9') return true;
    }
    return false;
}

bool scalar(il::Type t) {
    return t == il::Type::F64 || t == il::Type::I32 || t == il::Type::Bool;
}

// What may stand between two writes of a run (transition_runs.h). The list is
// written out, as il_lowering_static_run.cpp's is, so an op added later is not
// a member until someone says it may be. codegen-brass's
// il_transition_run.cpp holds the same list and refuses a run it disagrees
// with by name.
bool between(const il::Instruction& inst, const std::unordered_set<il::ValueId>& defined) {
    switch (inst.op) {
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::IsNumber:
        case il::Op::NumTruthy:
        case il::Op::CreateObject:
        case il::Op::CreateArray:
            return true;
        case il::Op::Box:
            return scalar(inst.boxType);
        // A checked ToNumber can call `valueOf`; the run's guard proves its
        // operand a Number up front, which it can do only for a value from
        // before the run.
        case il::Op::Unbox:
            return inst.type == il::Type::F64 && !inst.operands.empty() &&
                   (inst.rawUnbox || inst.nullishUnbox || !defined.count(inst.operands[0]));
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Neg:
        case il::Op::CmpLt:
        case il::Op::CmpLe:
        case il::Op::CmpGt:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
            return scalar(inst.type);
        default:
            return false;
    }
}

bool usesValue(const il::Instruction& inst, il::ValueId v) {
    for (il::ValueId op : inst.operands) {
        if (op == v) return true;
    }
    for (il::ValueId a : inst.target.args) {
        if (a == v) return true;
    }
    for (il::ValueId a : inst.elseTarget.args) {
        if (a == v) return true;
    }
    return false;
}

struct FnFacts {
    // The op that defines each value: what tells a literal or an array
    // receiver from any other.
    std::unordered_map<il::ValueId, il::Op> defOp;
};

}  // namespace

void markTransitionRuns(il::Module& module) {
    // The A/B seam, read by the compiler: BRONZE_NO_TRANSITION_RUNS=1 marks
    // nothing, so every write lowers alone (the runtime reads it too, and
    // then never publishes).
    static const bool off = [] {
        const char* v = std::getenv("BRONZE_NO_TRANSITION_RUNS");
        return v && v[0] == '1';
    }();
    if (off) return;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        // The `this` parameter (il.h Function::params: [__env] [__this] ...)
        // of a function that can be a constructor. Only it: a constructible
        // `function` also writes other receivers, and those are mostly
        // inlined setters over keys the object already has.
        const bool constructs = (fn.fnFlags & BRONZE_ABI_FN_FLAG_CONSTRUCT) != 0;
        const il::ValueId thisValue =
            constructs && fn.needsThis ? static_cast<il::ValueId>(fn.needsEnv ? 1 : 0) : il::kNoValue;
        FnFacts facts;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.result != il::kNoValue) facts.defOp[inst.result] = inst.op;
            }
        }
        for (il::Block& block : fn.blocks) {
            auto& insts = block.instructions;
            for (size_t i = 0; i < insts.size(); ++i) {
                const il::Instruction& first = insts[i];
                auto isSiteOn = [&](const il::Instruction& inst, il::ValueId recv) {
                    // A store of the receiver into itself (`a.self = a`) ends
                    // the run: il_transition_run.cpp's stores are of values
                    // made before the shape is, which the receiver is not.
                    return inst.op == il::Op::PropSet && inst.operands.size() == 2 && inst.operands[0] == recv &&
                           inst.operands[1] != recv && inst.directTarget == il::Instruction::kNoDirectTarget &&
                           inst.keyIndex < module.keyConstants.size() && runKey(module.keyConstants[inst.keyIndex]);
                };
                if (first.op != il::Op::PropSet || first.operands.size() != 2) continue;
                const il::ValueId recv = first.operands[0];
                if (!isSiteOn(first, recv)) continue;
                const auto def = facts.defOp.find(recv);
                if (def != facts.defOp.end() && def->second == il::Op::CreateArray) continue;

                // A literal's keys start at slot 0 when nothing before the run
                // has touched it since it was made in this block.
                bool freshLiteral = false;
                if (def != facts.defOp.end() && def->second == il::Op::CreateObject) {
                    for (size_t d = i; d-- > 0;) {
                        if (insts[d].result == recv) {
                            freshLiteral = true;
                            break;
                        }
                        if (usesValue(insts[d], recv)) break;
                    }
                }
                if (recv != thisValue && !freshLiteral) continue;

                std::unordered_set<il::ValueId> defined;
                std::unordered_set<uint32_t> keys;
                std::vector<size_t> sites;
                size_t last = i;
                for (size_t j = i; j < insts.size(); ++j) {
                    const il::Instruction& inst = insts[j];
                    if (isSiteOn(inst, recv)) {
                        if (!keys.insert(inst.keyIndex).second) break;
                        sites.push_back(j);
                        last = j;
                    } else if (!between(inst, defined) || usesValue(inst, recv)) {
                        break;
                    }
                    if (inst.result != il::kNoValue) defined.insert(inst.result);
                }
                if (sites.size() < 2) continue;

                bool consecutive = true;
                for (size_t k = 0; k < sites.size(); ++k) {
                    const uint32_t slot = insts[sites[k]].staticSlot;
                    if (slot == il::Instruction::kNoStaticSlot || slot != insts[sites[0]].staticSlot + k) {
                        consecutive = false;
                        break;
                    }
                }
                if (!consecutive && !freshLiteral) continue;

                il::Instruction& head = insts[i];
                head.transitionRunCell = module.staticSiteCount;
                head.transitionRunLength = static_cast<uint32_t>(last - i + 1);
                head.transitionRunSlot = freshLiteral ? 0u : insts[sites[0]].staticSlot;
                module.staticSiteCount += il::Instruction::kTransitionRunCells;
                i = last;
            }
        }
    }
}

}  // namespace bronze::lower
