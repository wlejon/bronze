// NEVER-ENTERED BLOCKS: the blocks of G the harvested run never entered,
// which the optimistic copy S+ leaves at the top of (optimistic.h).
//
// THE SIGNAL. A build made to be harvested marks words of its slow sites
// (il/feedback_site.h, docs/type-feedback.md "Slow paths") and records which
// words its code marks at all (the ARMED bitmap: bit i set exactly when the
// image holds code that writes word i, il2mir il_lowering_types.cpp). A word
// that is armed and still zero at the end of the run is a place in the code
// that never executed. Three kinds of instruction mark a word on EVERY
// execution, before anything of theirs can branch away:
//   - a call (`call`, `call.dynamic`, `method.call`, construct): its call
//     count (kCallCountArm), one more as it starts;
//   - a named `prop.get` / `prop.set` with an inline cache (not `length`, not
//     an index key, which lower to the element and length forms): its shape
//     turn count (kShapeTurnArm), which its first execution makes one;
//   - an element access or a numeric, compare or truthiness op with a slow
//     site: every path through its lowering marks an arm word or counts its
//     slow arm (the arms evidence.h names), so all its words zero, one of them
//     armed, is an instruction that never ran.
// A block holding such an instruction that never ran was never entered,
// unless an instruction in front of it threw every time the block was
// entered: then S+ leaves where G would have thrown anyway, which is exact
// and costs a leave, bounded by S+'s retirement. Checked against the exact
// kinds on the bench scenes: no block an extended-kind instruction calls
// unentered holds a call or named access that ran. A function the run never
// entered (its entry count zero, or not counted) gives no such evidence.
//
// WHICH BLOCKS. Only those where leaving pays: the block holds an instruction
// that may call anything (il/effects.h callsUnknown), which at its join with
// the main line costs S every value and shape it had loaded; and the code the
// block dominates REJOINS code it does not dominate (an arm that ends the
// function, a throw path, joins nothing). A block another such block
// dominates goes with it. The block must admit a resume point at its top
// (speculate.h mayPlaceResumePoint, il/resume_rules.h).

#include <algorithm>

#include "il/effects.h"
#include "il/feedback_site.h"
#include "il/resume_rules.h"
#include "opt/core/cfg.h"
#include "opt/core/shape_key.h"
#include "opt/speculate/evidence.h"

namespace bronze::opt::speculate {

namespace {

bool isCall(il::Op op) {
    return op == il::Op::Call || op == il::Op::DynamicCall || op == il::Op::MethodCall || op == il::Op::Construct;
}

bool isNamedCachedKey(const std::string& k) {
    return isNamedKey(k) && k != "length" && !k.empty() && !(k[0] >= '0' && k[0] <= '9');
}

}  // namespace

bool Evidence::neverRan(const il::Instruction& inst) const {
    if (inst.slowSite == il::Instruction::kNoSlowSite) return false;
    auto zero = [&](uint32_t index) { return slowCount(index) == 0 && harvest_->last.count(index) == 0; };
    if (isCall(inst.op)) return slowArmed(inst.slowSite + il::kCallCountArm) && zero(inst.slowSite + il::kCallCountArm);
    if (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) {
        if (inst.keyIndex >= module_.keyConstants.size() || !isNamedCachedKey(module_.keyConstants[inst.keyIndex])) {
            return false;
        }
        return slowArmed(inst.slowSite + il::kShapeTurnArm) && zero(inst.slowSite + il::kShapeTurnArm);
    }
    if (inst.op != il::Op::ElemGet && inst.op != il::Op::ElemSet && !il::observesOperandTags(inst)) return false;
    bool armed = false;
    for (uint32_t a = 0; a < il::kSlowSiteArms; ++a) {
        if (!zero(inst.slowSite + a)) return false;
        armed = armed || slowArmed(inst.slowSite + a);
    }
    return armed;
}

std::vector<BlockId> Evidence::neverEnteredBlocks(uint32_t fnIndex) const {
    std::vector<BlockId> out;
    const il::Function& fn = module_.functions[fnIndex];
    if (harvest_ == nullptr || fn.entrySlowSite == il::Instruction::kNoSlowSite || !slowArmed(fn.entrySlowSite) ||
        slowCount(fn.entrySlowSite) == 0) {
        return out;
    }
    const Cfg cfg(fn);
    const DomTree dom(cfg);
    std::vector<BlockId> candidates;
    for (BlockId b = 1; b < fn.blocks.size(); ++b) {
        if (!cfg.reachable(b) || !mayPlaceResumePoint(fn, b) || !il::canResumeBefore(fn.blocks[b], 0)) continue;
        const auto& insts = fn.blocks[b].instructions;
        const bool never = std::any_of(insts.begin(), insts.end(), [&](const il::Instruction& i) { return neverRan(i); });
        if (!never) continue;
        const bool calls = std::any_of(insts.begin(), insts.end(),
                                       [](const il::Instruction& i) { return il::effectsOf(i).callsUnknown; });
        if (!calls) continue;
        // Rejoins: an edge from the code b dominates to code it does not.
        bool rejoins = false;
        for (BlockId x = 0; x < fn.blocks.size() && !rejoins; ++x) {
            if (!cfg.reachable(x) || !dom.dominates(b, x)) continue;
            for (BlockId y : cfg.succs(x)) {
                if (!dom.dominates(b, y)) {
                    rejoins = true;
                    break;
                }
            }
        }
        if (rejoins) candidates.push_back(b);
    }
    for (BlockId b : candidates) {
        const bool inner = std::any_of(candidates.begin(), candidates.end(),
                                       [&](BlockId a) { return a != b && dom.dominates(a, b); });
        if (!inner) out.push_back(b);
    }
    return out;
}

}  // namespace bronze::opt::speculate
