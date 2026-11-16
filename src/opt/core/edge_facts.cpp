#include "opt/core/edge_facts.h"

#include <cstdint>

namespace bronze::opt {

const char* factKindName(FactKind kind) {
    switch (kind) {
        case FactKind::BoolValue: return "bool";
        case FactKind::I32: return "i32";
        case FactKind::Number: return "number";
        case FactKind::Object: return "object";
        case FactKind::Nullish: return "nullish";
        case FactKind::Hole: return "hole";
        case FactKind::Shape: return "shape";
        case FactKind::DenseArray: return "dense-array";
        case FactKind::PlainArray: return "plain-array";
        case FactKind::TypedArray: return "typed-array";
        case FactKind::Array: return "array";
        case FactKind::Callee: return "callee";
        case FactKind::InBounds: return "in-bounds";
        case FactKind::InRange: return "in-range";
        case FactKind::PtrEq: return "ptr-eq";
    }
    return "?";
}

namespace {

// How far a chain of `cmp.eq %b, false` negations is followed back. Lowering
// makes one per `!`; four covers anything a program writes on purpose.
constexpr int kMaxNegationDepth = 4;

class Deriver {
public:
    Deriver(const il::Function& fn, const DefUse& defs, BlockId branchBlock)
        : fn_(fn), defs_(defs), branchBlock_(branchBlock) {}

    void derive(ValueId cond, bool truth, int depth, std::vector<Fact>& out) const {
        out.push_back(Fact{FactKind::BoolValue, true, cond, kNoValue, truth ? 1 : 0, {}});
        const il::Instruction* d = defs_.defInst(fn_, cond);
        // Every test below reads at least one operand; the two-operand ones
        // check for the second themselves.
        if (d == nullptr || d->operands.empty()) return;
        auto valueFact = [&](FactKind kind, int64_t imm = 0) {
            out.push_back(Fact{kind, truth, d->operands[0], kNoValue, imm, {}});
        };
        switch (d->op) {
            case il::Op::IsI32: valueFact(FactKind::I32); return;
            case il::Op::IsNumber: valueFact(FactKind::Number); return;
            case il::Op::IsObject: valueFact(FactKind::Object); return;
            case il::Op::IsNullish: valueFact(FactKind::Nullish); return;
            case il::Op::IsHole: valueFact(FactKind::Hole); return;
            case il::Op::FnCodeIs: valueFact(FactKind::Callee, d->calleeIndex); return;
            case il::Op::IsArray: valueFact(FactKind::Array); return;
            case il::Op::InBounds: {
                if (d->operands.size() != 2) return;
                out.push_back(Fact{FactKind::InBounds, truth, d->operands[0], d->operands[1], 0, {}});
                // A widened test (il_op.h) also bounds its neighbours: with x
                // = base + c0, every base + c for c in c0..c0+span.
                if (!truth || d->immI32 <= 0) return;
                const auto [base, c0] =
                    offsetForm(d->operands[0], [&](ValueId v) { return defs_.defInst(fn_, v); });
                if (static_cast<int64_t>(c0) + d->immI32 > INT32_MAX) return;
                out.push_back(Fact{FactKind::InRange, true, base, d->operands[1], packRange(c0, c0 + d->immI32), {}});
                return;
            }
            case il::Op::IsDenseArray:
                memoryFact(cond, FactKind::DenseArray, truth, d->operands[0], d->immI32,
                           Loc::Shapes | Loc::ArrayLen | Loc::Elements, out);
                return;
            case il::Op::IsPlainArray:
                memoryFact(cond, FactKind::PlainArray, truth, d->operands[0], 0, Loc::Shapes, out);
                return;
            case il::Op::IsTypedArray: {
                // The bare kind test reads nothing that changes, so its fact
                // is about the value; the pristine form also reads the
                // instance shape and the prototype epoch.
                const bool pristine = (d->immI32 & il::kTypedArrayPristineLength) != 0;
                if (pristine) {
                    memoryFact(cond, FactKind::TypedArray, truth, d->operands[0], d->immI32,
                               Loc::Shapes | Loc::Heap, out);
                } else {
                    valueFact(FactKind::TypedArray, d->immI32);
                }
                return;
            }
            case il::Op::PtrEq:
                ptrEq(*d, truth, out);
                return;
            case il::Op::CmpEq:
            case il::Op::CmpNe:
                if (depth < kMaxNegationDepth) negation(*d, truth, depth, out);
                return;
            default:
                return;
        }
    }

private:
    using Loc = il::Loc;

    // Whether the instruction defining `tested` sits in the branch's block
    // with no write of `deps` between it and the terminator.
    bool freshAtBranch(ValueId tested, il::LocSet deps) const {
        const DefSite& site = defs_.def(tested);
        if (site.kind != DefSite::Kind::Instruction || site.block != branchBlock_) return false;
        const auto& insts = fn_.blocks[branchBlock_].instructions;
        for (size_t i = site.index + 1; i + 1 < insts.size(); ++i) {
            if (il::effectsOf(insts[i]).writes.intersects(deps)) return false;
        }
        return true;
    }

    void memoryFact(ValueId tested, FactKind kind, bool truth, ValueId subject, int64_t imm,
                    il::LocSet deps, std::vector<Fact>& out) const {
        if (!freshAtBranch(tested, deps)) return;
        out.push_back(Fact{kind, truth, subject, kNoValue, imm, deps});
    }

    // `ptr.eq (load.shape %x), (shape.cell n)`, either way round, is the
    // shape test; any other pair is a plain pointer equality.
    void ptrEq(const il::Instruction& d, bool truth, std::vector<Fact>& out) const {
        if (d.operands.size() != 2) return;
        const ValueId a = d.operands[0];
        const ValueId b = d.operands[1];
        for (int flip = 0; flip < 2; ++flip) {
            const ValueId loadId = flip ? b : a;
            const ValueId cellId = flip ? a : b;
            const il::Instruction* load = defs_.defInstIs(fn_, loadId, il::Op::LoadShape);
            const il::Instruction* cell = defs_.defInstIs(fn_, cellId, il::Op::ShapeCell);
            if (load == nullptr || cell == nullptr) continue;
            // The shape word was read at the load, so that is the read that
            // has to be fresh, not the compare.
            if (freshAtBranch(loadId, Loc::Shapes)) {
                out.push_back(Fact{FactKind::Shape, truth, load->operands[0], kNoValue, cell->immI32,
                                   Loc::Shapes});
            }
            return;
        }
        out.push_back(Fact{FactKind::PtrEq, truth, a, b, 0, {}});
    }

    // `cmp.eq %o, <const bool k>` (lowering's `!`) and `cmp.ne`, either way
    // round: on this edge %o is known, and whatever %o was tested from is
    // known with it.
    void negation(const il::Instruction& d, bool truth, int depth, std::vector<Fact>& out) const {
        if (d.operands.size() != 2) return;
        for (int flip = 0; flip < 2; ++flip) {
            const ValueId constId = d.operands[flip ? 0 : 1];
            const ValueId other = d.operands[flip ? 1 : 0];
            const il::Instruction* k = defs_.defInstIs(fn_, constId, il::Op::ConstBool);
            if (k == nullptr || defs_.type(other) != il::Type::Bool) continue;
            const bool equal = d.op == il::Op::CmpEq ? truth : !truth;
            const bool kValue = k->immI32 != 0;
            derive(other, equal ? kValue : !kValue, depth + 1, out);
            return;
        }
    }

    const il::Function& fn_;
    const DefUse& defs_;
    BlockId branchBlock_;
};

}  // namespace

EdgeFacts branchFacts(const il::Function& fn, const DefUse& defs, BlockId b) {
    EdgeFacts facts;
    const il::Block& block = fn.blocks[b];
    if (block.instructions.empty()) return facts;
    const il::Instruction& term = block.instructions.back();
    if (term.op != il::Op::Branch || term.operands.empty()) return facts;
    // A `br` whose two arms reach the same block proves nothing on either;
    // the verifier refuses it, and this does not rely on that.
    if (term.target.block == term.elseTarget.block) return facts;
    const Deriver deriver(fn, defs, b);
    deriver.derive(term.operands[0], true, 0, facts.onTrue);
    deriver.derive(term.operands[0], false, 0, facts.onFalse);
    return facts;
}

std::vector<Fact> entryFacts(const il::Function& fn, const Cfg& cfg, const DefUse& defs, BlockId b) {
    const std::optional<Edge> edge = cfg.uniqueInEdge(b);
    if (!edge || (edge->kind != EdgeKind::BranchThen && edge->kind != EdgeKind::BranchElse)) return {};
    EdgeFacts facts = branchFacts(fn, defs, edge->from);
    return edge->kind == EdgeKind::BranchThen ? std::move(facts.onTrue) : std::move(facts.onFalse);
}

}  // namespace bronze::opt
