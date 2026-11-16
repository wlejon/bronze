// Rule 2, the solution (params.h says what qualifies a parameter).
//
// An optimistic fixpoint: every dynamic block parameter starts at Top and
// takes the meet of what its edges pass; a parameter passed as another's
// argument contributes its current value, so the parameters of a loop nest
// settle together and a parameter fed only by itself and boxes is typed. A
// value that only descends (Top -> I32 -> F64 -> Bottom, or -> Bool ->
// Bottom) makes the sweep end.
//
// Around it, an outer loop EXCLUDES parameters that end Top (fed by nothing
// but one another: unreachable or dead cycles), Bottom, or with nothing to
// gain (every input an existing unbox and no use that would fold), and
// solves again: an excluded parameter stays dynamic, so it is an ordinary
// dynamic argument to the others, readable only through an available unbox.
// Exclusion only grows, so the loop ends.

#include <cassert>

#include "opt/repr/params.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

using il::Op;
using il::Type;

Lat meet(Lat a, Lat b) {
    if (a == Lat::Top) return b;
    if (b == Lat::Top) return a;
    if (a == b) return a;
    // An int32 is exactly a double; see params_apply.cpp for why a parameter
    // that held either box may hold the double.
    if ((a == Lat::I32 && b == Lat::F64) || (a == Lat::F64 && b == Lat::I32)) return Lat::F64;
    return Lat::Bottom;
}

Type latType(Lat l) {
    switch (l) {
        case Lat::I32: return Type::I32;
        case Lat::F64: return Type::F64;
        case Lat::Bool: return Type::Bool;
        default: return Type::Void;
    }
}

UnboxIndex::UnboxIndex(const il::Function& fn, const Cfg& cfg) : byValue_(fn.valueCount), cfg_(cfg) {
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if (inst.op != Op::UnboxI32 && inst.op != Op::UnboxF64) continue;
            if (inst.operands.size() != 1 || inst.result == kNoValue) continue;
            const ValueId v = inst.operands[0];
            if (v >= byValue_.size()) byValue_.resize(static_cast<size_t>(v) + 1);
            byValue_[v].push_back(Entry{static_cast<BlockId>(b), inst.type, inst.result});
        }
    }
}

ValueId UnboxIndex::available(ValueId v, Type type, BlockId from, const DomTree& dom) const {
    if (v >= byValue_.size()) return kNoValue;
    for (const Entry& e : byValue_[v]) {
        if (e.type == type && (e.block == from || valueReaches(cfg_, dom, e.block, from))) return e.result;
    }
    return kNoValue;
}

std::vector<std::vector<InEdge>> collectInEdges(const il::Function& fn) {
    std::vector<std::vector<InEdge>> in(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        if (insts.empty()) continue;
        const il::Instruction& term = insts.back();
        const BlockId from = static_cast<BlockId>(b);
        if (term.op == Op::Jump || term.op == Op::Branch) {
            if (term.target.block < in.size()) in[term.target.block].push_back(InEdge{from, false});
        }
        if (term.op == Op::Branch && term.elseTarget.block < in.size()) {
            in[term.elseTarget.block].push_back(InEdge{from, true});
        }
    }
    return in;
}

const std::vector<ValueId>& inEdgeArgs(const il::Function& fn, const InEdge& e) {
    const il::Instruction& term = fn.blocks[e.from].instructions.back();
    return e.isElse ? term.elseTarget.args : term.target.args;
}

std::vector<ValueId>& inEdgeArgs(il::Function& fn, const InEdge& e) {
    il::Instruction& term = fn.blocks[e.from].instructions.back();
    return e.isElse ? term.elseTarget.args : term.target.args;
}

namespace {

constexpr uint32_t kNotCandidate = UINT32_MAX;

// Whether replacing a dynamic operand by a box of a typed value lets rule 1
// answer this use.
bool foldableUse(Op op) {
    switch (op) {
        case Op::IsI32:
        case Op::IsNumber:
        case Op::IsObject:
        case Op::IsNullish:
        case Op::IsHole:
        case Op::IsUninit:
        case Op::UnboxI32:
        case Op::UnboxF64:
        case Op::Unbox:
        case Op::ToNumeric:
        case Op::ToInt32:
        case Op::StrictEq:
        case Op::LooseEq:
        case Op::RelLt:
        case Op::RelGt:
        case Op::RelLe:
        case Op::RelGe:
            return true;
        default:
            return false;
    }
}

class ParamSolver {
public:
    ParamSolver(const il::Function& fn, const DefUse& defs, const DomTree& dom, const UnboxIndex& unboxes,
                const std::vector<std::vector<InEdge>>& inEdges)
        : fn_(fn), defs_(defs), dom_(dom), unboxes_(unboxes), inEdges_(inEdges),
          candIndex_(defs.valueCount(), kNotCandidate) {
        for (size_t b = 1; b < fn.blocks.size(); ++b) {
            const auto& params = fn.blocks[b].params;
            for (size_t p = 0; p < params.size(); ++p) {
                if (params[p].type != Type::Dynamic || params[p].id >= candIndex_.size()) continue;
                candIndex_[params[p].id] = static_cast<uint32_t>(cands_.size());
                cands_.push_back(Cand{static_cast<BlockId>(b), static_cast<uint32_t>(p), params[p].id});
            }
        }
    }

    std::vector<ParamPlan> solve();

private:
    struct Cand {
        BlockId block;
        uint32_t index;
        ValueId id;
        bool excluded = false;
        Lat lat = Lat::Top;
    };

    const Cand* live(ValueId v) const {
        if (v >= candIndex_.size() || candIndex_[v] == kNotCandidate) return nullptr;
        const Cand& c = cands_[candIndex_[v]];
        return c.excluded ? nullptr : &c;
    }
    Lat argLat(ValueId a, BlockId from) const;
    Lat inputLat(const Cand& c) const;
    void sweepToFixpoint();
    bool worthwhile(const Cand& c) const;
    bool widened(const Cand& c) const;

    const il::Function& fn_;
    const DefUse& defs_;
    const DomTree& dom_;
    const UnboxIndex& unboxes_;
    const std::vector<std::vector<InEdge>>& inEdges_;
    std::vector<uint32_t> candIndex_;
    std::vector<Cand> cands_;
};

Lat ParamSolver::argLat(ValueId a, BlockId from) const {
    if (const Cand* c = live(a)) return c->lat;
    if (const std::optional<BoxSource> s = boxSource(fn_, defs_, a)) {
        switch (s->type) {
            case Type::I32: return Lat::I32;
            // A double that is exactly an int32 serves either kind.
            case Type::F64: return exactInt32(fn_, defs_, s->value) ? Lat::I32 : Lat::F64;
            case Type::Bool: return Lat::Bool;
            default: return Lat::Bottom;
        }
    }
    if (unboxes_.available(a, Type::I32, from, dom_) != kNoValue) return Lat::I32;
    if (unboxes_.available(a, Type::F64, from, dom_) != kNoValue) return Lat::F64;
    return Lat::Bottom;
}

Lat ParamSolver::inputLat(const Cand& c) const {
    Lat l = Lat::Top;
    for (const InEdge& e : inEdges_[c.block]) {
        const std::vector<ValueId>& args = inEdgeArgs(fn_, e);
        assert(c.index < args.size());
        l = meet(l, argLat(args[c.index], e.from));
    }
    return l;
}

void ParamSolver::sweepToFixpoint() {
    for (Cand& c : cands_) {
        if (!c.excluded) c.lat = Lat::Top;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (Cand& c : cands_) {
            if (c.excluded) continue;
            const Lat l = inputLat(c);
            if (l != c.lat) {
                c.lat = l;
                changed = true;
            }
        }
    }
}

// Something is gained: a box on some edge stops being needed there, the
// parameter feeds another typed one, or a use of it folds.
bool ParamSolver::worthwhile(const Cand& c) const {
    for (const InEdge& e : inEdges_[c.block]) {
        const ValueId a = inEdgeArgs(fn_, e)[c.index];
        if (a == c.id) continue;
        if (boxSource(fn_, defs_, a)) return true;
        if (const Cand* q = live(a); q != nullptr && latType(q->lat) != Type::Void) return true;
    }
    for (const UseSite& use : defs_.uses(c.id)) {
        const il::Instruction& inst = fn_.blocks[use.block].instructions[use.inst];
        if (use.kind == UseSite::Kind::Operand) {
            if (foldableUse(inst.op)) return true;
            continue;
        }
        const BlockId to = use.kind == UseSite::Kind::ThenArg ? inst.target.block : inst.elseTarget.block;
        const auto& params = fn_.blocks[to].params;
        if (use.slot < params.size()) {
            const Cand* q = live(params[use.slot].id);
            if (q != nullptr && q != &c && latType(q->lat) != Type::Void) return true;
        }
    }
    return false;
}

bool ParamSolver::widened(const Cand& c) const {
    if (c.lat != Lat::F64) return false;
    for (const InEdge& e : inEdges_[c.block]) {
        if (argLat(inEdgeArgs(fn_, e)[c.index], e.from) == Lat::I32) return true;
    }
    return false;
}

std::vector<ParamPlan> ParamSolver::solve() {
    // Each round excludes at least one more candidate or stops.
    for (size_t round = 0; round <= cands_.size(); ++round) {
        sweepToFixpoint();
        bool grew = false;
        for (Cand& c : cands_) {
            if (c.excluded) continue;
            if (latType(c.lat) == Type::Void || !worthwhile(c)) {
                c.excluded = true;
                grew = true;
            }
        }
        if (!grew) break;
    }
    std::vector<ParamPlan> plan;
    for (const Cand& c : cands_) {
        if (c.excluded) continue;
        const Type t = latType(c.lat);
        assert(t != Type::Void);
        plan.push_back(ParamPlan{c.block, c.index, c.id, t, widened(c)});
    }
    return plan;
}

}  // namespace

std::vector<ParamPlan> planParams(const il::Function& fn, const DefUse& defs, const DomTree& dom,
                                  const UnboxIndex& unboxes,
                                  const std::vector<std::vector<InEdge>>& inEdges) {
    ParamSolver solver(fn, defs, dom, unboxes, inEdges);
    return solver.solve();
}

}  // namespace bronze::opt::repr
