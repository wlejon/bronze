// LOOP INDEX FACTS (index_facts.h says what and why).

#include "opt/redundancy/index_facts.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// The branch arm `b` is entered by when it is its only way in: the
// branch's condition and whether `b` is its true arm.
struct ArmEntry {
    ValueId cond = kNoValue;
    bool onTrue = false;
};

bool armEntry(const il::Function& fn, const Cfg& cfg, BlockId b, ArmEntry& out) {
    if (!cfg.reachable(b) || cfg.inEdges(b).size() != 1) return false;
    const Edge& edge = cfg.inEdges(b)[0];
    if (edge.kind != EdgeKind::BranchThen && edge.kind != EdgeKind::BranchElse) return false;
    const il::Instruction& term = fn.blocks[edge.from].instructions.back();
    if (term.op != Op::Branch || term.operands.empty() || term.target.block == term.elseTarget.block) return false;
    out.cond = term.operands[0];
    out.onTrue = edge.kind == EdgeKind::BranchThen;
    return true;
}

bool isI32(const DefUse& defs, ValueId v) { return v < defs.valueCount() && defs.type(v) == il::Type::I32; }

// The constant an i32 value is, if it is one.
const il::Instruction* constI32(const il::Function& fn, const DefUse& defs, ValueId v) {
    return defs.defInstIs(fn, v, Op::ConstI32);
}

}  // namespace

IndexFacts::IndexFacts(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs)
    : fn_(fn), cfg_(cfg), dom_(dom), defs_(defs), lessThanOf_(fn.blocks.size(), -1) {
    if (!fn.isSpeculated()) return;

    // The less-than edges.
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        ArmEntry arm;
        if (!armEntry(fn, cfg, b, arm)) continue;
        const il::Instruction* test = defs.defInst(fn, arm.cond);
        if (test == nullptr || test->operands.size() != 2) continue;
        const ValueId l = test->operands[0];
        const ValueId r = test->operands[1];
        if (!isI32(defs, l) || !isI32(defs, r)) continue;
        LessThan lt;
        lt.block = b;
        if ((test->op == Op::CmpLt && arm.onTrue) || (test->op == Op::CmpGe && !arm.onTrue)) {
            lt.x = l;
            lt.n = r;
        } else if ((test->op == Op::CmpGt && arm.onTrue) || (test->op == Op::CmpLe && !arm.onTrue)) {
            lt.x = r;
            lt.n = l;
        } else {
            continue;
        }
        lessThanOf_[b] = static_cast<int32_t>(lessThan_.size());
        lessThan_.push_back(lt);
    }

    // The candidates, all in to start with; constants are decided now.
    nonNeg_.assign(fn.valueCount, 0);
    std::vector<ValueId> params;
    std::vector<std::pair<BlockId, const il::Instruction*>> adds;
    for (const il::Block& block : fn.blocks) {
        if (!cfg.reachable(block.id)) continue;
        for (const il::BlockParam& p : block.params) {
            if (p.id < nonNeg_.size() && isI32(defs, p.id)) {
                nonNeg_[p.id] = 1;
                params.push_back(p.id);
            }
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result == kNoValue || inst.result >= nonNeg_.size() || inst.type != il::Type::I32) continue;
            if (inst.op == Op::ConstI32 && inst.immI32 >= 0) {
                nonNeg_[inst.result] = 1;
            } else if (inst.op == Op::Add && inst.operands.size() == 2 && addCannotWrap(block.id, inst)) {
                nonNeg_[inst.result] = 1;
                adds.emplace_back(block.id, &inst);
            }
        }
    }
    // Drop what its definition does not keep non-negative, to the fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [b, add] : adds) {
            if (!nonNeg_[add->result]) continue;
            if (!nonNegative(add->operands[0]) || !nonNegative(add->operands[1])) {
                nonNeg_[add->result] = 0;
                changed = true;
            }
        }
        for (const ValueId p : params) {
            if (!nonNeg_[p]) continue;
            const DefSite& site = defs.def(p);
            bool keep = site.kind == DefSite::Kind::BlockParam;
            for (const Edge& edge : keep ? cfg.inEdges(site.block) : std::vector<Edge>{}) {
                if (!cfg.reachable(edge.from)) continue;
                const std::vector<ValueId>& args = edgeArgs(fn, edge);
                if (edge.kind == EdgeKind::Throw || site.index >= args.size() || !nonNegative(args[site.index])) {
                    keep = false;
                    break;
                }
            }
            if (!keep) {
                nonNeg_[p] = 0;
                changed = true;
            }
        }
    }
}

bool IndexFacts::lessThanDominates(BlockId b, ValueId x) const {
    for (BlockId a = b; a != kNoBlock && a < lessThanOf_.size(); a = dom_.idom(a)) {
        const int32_t i = lessThanOf_[a];
        if (i >= 0 && lessThan_[static_cast<size_t>(i)].x == x) return true;
    }
    return false;
}

bool IndexFacts::addCannotWrap(BlockId b, const il::Instruction& add) const {
    const ValueId l = add.operands[0];
    const ValueId r = add.operands[1];
    // x + 0 or x + 1 under x < n.
    for (int side = 0; side < 2; ++side) {
        const il::Instruction* k = constI32(fn_, defs_, side == 0 ? r : l);
        if (k != nullptr && (k->immI32 == 0 || k->immI32 == 1) && lessThanDominates(b, side == 0 ? l : r)) {
            return true;
        }
    }
    // Under the false arm of its own overflow test.
    for (BlockId a = b; a != kNoBlock && a < fn_.blocks.size(); a = dom_.idom(a)) {
        ArmEntry arm;
        if (armEntry(fn_, cfg_, a, arm) && !arm.onTrue) {
            const il::Instruction* test = defs_.defInstIs(fn_, arm.cond, Op::AddWouldOvf);
            if (test != nullptr && test->operands.size() == 2 &&
                ((test->operands[0] == l && test->operands[1] == r) ||
                 (test->operands[0] == r && test->operands[1] == l))) {
                return true;
            }
        }
    }
    return false;
}

std::vector<Fact> IndexFacts::onEntry(BlockId b) const {
    if (b >= lessThanOf_.size() || lessThanOf_[b] < 0) return {};
    const LessThan& lt = lessThan_[static_cast<size_t>(lessThanOf_[b])];
    if (!nonNegative(lt.x)) return {};
    return {Fact{FactKind::InBounds, true, lt.x, lt.n, 0, {}},
            Fact{FactKind::InRange, true, lt.x, lt.n, packRange(0, 0), {}}};
}

}  // namespace bronze::opt::redundancy
