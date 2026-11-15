// Rule 2, the application of a plan (params_solve.cpp makes it).
//
//   1. Each retyped parameter p of type T gets `pb = box.T p` as the first
//      instruction of its block, and every use of p reads pb instead. The
//      block's own instructions follow pb, and every other use of p is in a
//      block p's block dominates, so pb dominates them all. Uses that fold
//      (a test, an unbox, a conversion) are rule 1's next round; the rest
//      read a box made once per entry to the block, where before each edge
//      made one.
//   2. Every edge into the block passes, for p, the typed value of what it
//      passed before: the operand of a box, an existing unchecked unbox of
//      the value that dominates the edge, an `i32.to_f64` of an int32 for an
//      F64 parameter, or a `const.i32` for a `box.f64` of an int32 constant.
//      Conversions are placed in front of the source block's terminator,
//      where everything they read is available (the box or unbox they replace
//      was), and run on both arms of a branch, which costs nothing wrong:
//      they are pure.
//
// WHY THE VALUE IS THE SAME. p held, on each path, the box of a Number (or
// Boolean) v; it now holds v typed and pb boxes it again. The JS value is
// the same on every path. The box can differ in ENCODING from the one the
// edge made: an F64 parameter fed `box.i32 x` on one edge now holds
// `box.f64 (i32.to_f64 x)`; an I32 parameter fed `box.f64 0.0` now holds
// `box.i32 0`; a value the speculation step unboxed with `unbox.i32` may have
// arrived as an integral double and leaves as an Int32 box. encoding.h
// (INTERCHANGEABILITY) is why no consumer can tell: G already makes both
// boxes for one Number, and inside S the tests that read the encoding read
// the same SSA value as the unboxes they license. -0 cannot arise from an
// int32, so no widened value is a -0 that was a 0.

#include <cassert>
#include <utility>

#include "opt/core/cfg_edit.h"
#include "opt/repr/params.h"
#include "opt/repr/phases.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

using il::Op;
using il::Type;

namespace {

// The conversions one source block's terminator needs, shared between its
// edges: a value converted for two edges is converted once.
class EdgeConversions {
public:
    EdgeConversions(il::Function& fn, BlockId from) : fn_(fn), span_(fn.blocks[from].instructions.back().span) {}

    ValueId toF64(ValueId x) {
        for (const auto& [from, to] : f64_) {
            if (from == x) return to;
        }
        il::Instruction conv = makeI32ToF64(fn_, x, span_);
        const ValueId v = conv.result;
        f64_.emplace_back(x, v);
        made_.push_back(std::move(conv));
        return v;
    }

    ValueId constI32(int32_t c) {
        for (const auto& [value, id] : consts_) {
            if (value == c) return id;
        }
        il::Instruction k = makeConstI32(fn_, c, span_);
        const ValueId v = k.result;
        consts_.emplace_back(c, v);
        made_.push_back(std::move(k));
        return v;
    }

    std::vector<il::Instruction> take() { return std::move(made_); }

private:
    il::Function& fn_;
    Span span_;
    std::vector<std::pair<ValueId, ValueId>> f64_;
    std::vector<std::pair<int32_t, ValueId>> consts_;
    std::vector<il::Instruction> made_;
};

class PlanApplier {
public:
    PlanApplier(il::Function& fn, const std::vector<ParamPlan>& plan, const DomTree& dom,
                const UnboxIndex& unboxes, ReprStats& stats)
        : fn_(fn), plan_(plan), dom_(dom), unboxes_(unboxes), stats_(stats) {}

    void run();

private:
    void boxAtBlockTops();
    void rewriteEdges();
    // The typed value of type `t` an edge from `from` passes where it passed
    // the dynamic `a`.
    ValueId typedArg(const DefUse& defs, ValueId a, BlockId from, Type t, EdgeConversions& conv) const;

    il::Function& fn_;
    const std::vector<ParamPlan>& plan_;
    const DomTree& dom_;
    const UnboxIndex& unboxes_;
    ReprStats& stats_;
};

void PlanApplier::run() {
    boxAtBlockTops();
    rewriteEdges();
    stats_.paramsUnboxed += plan_.size();
    for (const ParamPlan& p : plan_) {
        if (p.widened) ++stats_.paramsWidened;
    }
}

void PlanApplier::boxAtBlockTops() {
    // Fresh ids first, so the use map covers them; then every use of p,
    // edge arguments included, reads pb; then the boxes themselves, whose
    // operand is the only remaining mention of p.
    std::vector<ValueId> boxed(plan_.size());
    for (size_t k = 0; k < plan_.size(); ++k) boxed[k] = newValue(fn_);
    std::vector<ValueId> map(fn_.valueCount, kNoValue);
    for (size_t k = 0; k < plan_.size(); ++k) map[plan_[k].id] = boxed[k];
    replaceAllUses(fn_, map);

    // The plan is in block then parameter order, so each block's boxes are
    // one run, placed in parameter order.
    for (size_t k = 0; k < plan_.size();) {
        const BlockId b = plan_[k].block;
        il::Block& block = fn_.blocks[b];
        assert(!block.instructions.empty());
        const Span span = block.instructions.front().span;
        std::vector<il::Instruction> boxes;
        for (; k < plan_.size() && plan_[k].block == b; ++k) {
            const ParamPlan& p = plan_[k];
            assert(block.params[p.index].id == p.id && block.params[p.index].type == Type::Dynamic);
            block.params[p.index].type = p.type;
            il::Instruction box;
            box.op = Op::Box;
            box.type = Type::Dynamic;
            box.boxType = p.type;
            box.result = boxed[k];
            box.operands = {p.id};
            box.span = span;
            boxes.push_back(std::move(box));
        }
        block.instructions.insert(block.instructions.begin(), std::make_move_iterator(boxes.begin()),
                                  std::make_move_iterator(boxes.end()));
    }
}

ValueId PlanApplier::typedArg(const DefUse& defs, ValueId a, BlockId from, Type t,
                              EdgeConversions& conv) const {
    if (const std::optional<BoxSource> s = boxSource(fn_, defs, a)) {
        if (s->type == t) return s->value;
        if (s->type == Type::I32 && t == Type::F64) return conv.toF64(s->value);
        if (s->type == Type::F64 && t == Type::I32) {
            // The solve only gave an I32 parameter a double box that is
            // exactly an int32.
            const std::optional<Int32Source> k = exactInt32(fn_, defs, s->value);
            assert(k.has_value());
            return k->isConst ? conv.constI32(k->constant) : k->value;
        }
        assert(false && "a box the solve did not admit");
        return kNoValue;
    }
    // Not a box: the solve admitted it through an unchecked unbox that
    // dominates the edge, preferring the int32 one as it does here.
    if (t == Type::F64) {
        const ValueId f = unboxes_.available(a, Type::F64, from, dom_);
        if (f != kNoValue) return f;
        const ValueId i = unboxes_.available(a, Type::I32, from, dom_);
        assert(i != kNoValue);
        return conv.toF64(i);
    }
    assert(t == Type::I32);
    const ValueId i = unboxes_.available(a, Type::I32, from, dom_);
    assert(i != kNoValue);
    return i;
}

void PlanApplier::rewriteEdges() {
    // The retyped parameters of each block by position, Void where kept.
    std::vector<std::vector<Type>> retyped(fn_.blocks.size());
    for (const ParamPlan& p : plan_) {
        auto& slots = retyped[p.block];
        if (slots.size() <= p.index) slots.resize(fn_.blocks[p.block].params.size(), Type::Void);
        slots[p.index] = p.type;
    }

    const DefUse defs(fn_);
    for (size_t b = 0; b < fn_.blocks.size(); ++b) {
        const BlockId from = static_cast<BlockId>(b);
        il::Block& block = fn_.blocks[b];
        if (block.instructions.empty()) continue;
        const Op termOp = block.instructions.back().op;
        if (termOp != Op::Jump && termOp != Op::Branch) continue;

        EdgeConversions conv(fn_, from);
        for (int arm = 0; arm < (termOp == Op::Branch ? 2 : 1); ++arm) {
            // Re-read the terminator: nothing below inserts into this block
            // until both arms are done, but the reference must not outlive
            // that.
            il::BlockTarget& target =
                arm == 0 ? fn_.blocks[b].instructions.back().target : fn_.blocks[b].instructions.back().elseTarget;
            if (target.block >= retyped.size()) continue;
            const std::vector<Type>& slots = retyped[target.block];
            for (size_t s = 0; s < slots.size() && s < target.args.size(); ++s) {
                if (slots[s] == Type::Void) continue;
                const ValueId typed = typedArg(defs, target.args[s], from, slots[s], conv);
                assert(typed != kNoValue);
                target.args[s] = typed;
            }
        }
        std::vector<il::Instruction> made = conv.take();
        if (made.empty()) continue;
        stats_.edgeConversions += made.size();
        auto& insts = fn_.blocks[b].instructions;
        insts.insert(insts.end() - 1, std::make_move_iterator(made.begin()), std::make_move_iterator(made.end()));
    }
}

}  // namespace

void applyParamPlan(il::Function& fn, const std::vector<ParamPlan>& plan, const DomTree& dom,
                    const UnboxIndex& unboxes, ReprStats& stats) {
    if (plan.empty()) return;
    PlanApplier applier(fn, plan, dom, unboxes, stats);
    applier.run();
}

bool unboxBlockParams(PhaseContext& ctx) {
    il::Function& fn = ctx.fn;
    if (fn.blocks.empty()) return false;
    // The solve reads only reachable edges: an edge from a block no path
    // reaches could pass anything and would hold every parameter it feeds
    // dynamic.
    const bool removed = removeUnreachableBlocks(fn) != 0;
    const Cfg cfg(fn);
    const DomTree dom(cfg);
    const DefUse defs(fn);
    const UnboxIndex unboxes(fn);
    const std::vector<std::vector<InEdge>> inEdges = collectInEdges(fn);
    const std::vector<ParamPlan> plan = planParams(fn, defs, dom, unboxes, inEdges);
    if (plan.empty()) return removed;
    applyParamPlan(fn, plan, dom, unboxes, ctx.stats);
    return true;
}

}  // namespace bronze::opt::repr
