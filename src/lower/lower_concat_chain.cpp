// A left-associative `+` spine of three or more operands, lowered as an
// ACCUMULATOR instead of as a fold of pairwise `add`s.
//
// WHY THE SPINE AND NOT THE OPERATOR. `a + b + c + d` parses as
// `((a + b) + c) + d`, and each of those `+` allocates a fresh flat string and
// copies the whole prefix into it — so building a 36-character UUID out of
// nineteen pieces copies about 380 characters through nineteen allocations to
// produce 36. The accumulator allocates once with room to grow and each step
// writes its piece into the slack; `concat.end` seals it.
//
// WHY IT IS STILL 13.15.3 IN ORDER. ECMA-262 13.15.3 fixes the order for
// `((a + b) + c) + d` as: evaluate a, evaluate b, ToPrimitive(a),
// ToPrimitive(b), decide String-or-numeric and convert, THEN evaluate c,
// ToPrimitive of the accumulator, ToPrimitive(c), and so on. Every one of those
// steps is observable — an operand can be an object with a `toString`, and a
// later operand's evaluation can change what an earlier one's `toString`
// reads — so an N-ary helper handed every operand at once would be the WRONG
// ALGORITHM: c would have been evaluated before a was converted.
//
// This lowering emits nothing of the kind. Each `concat.append` is called with
// exactly the two values the corresponding `+` had, at exactly the point that
// `+` ran, and its helper performs exactly that `+`. The only thing it relies
// on is that ToPrimitive of the accumulator is the identity, and it is: an
// accumulator is a String or a Number, and 7.1.1 step 1 returns a primitive
// unchanged without consulting anything on it.
//
// WHAT IT REFUSES, AND WHY REFUSING IS FREE. The accumulator must stay inside
// one basic block, because that is what makes its single-use property — the
// thing that licenses appending in place — checkable where it is established
// (il/verifier.cpp). So the spine is taken only when no operand's evaluation
// can split the block: no `?:`, no `&&`/`||`/`??`, no `?.`, no `await` or
// `yield`, and no call, whose position a later IL-to-IL pass is free to cut a
// guarded region at. Every refusal returns nullopt and the caller emits the
// fold it always emitted, so the predicate is free to be narrow and a case it
// misses is a case that is merely as fast as it was.
//
// THE SEAM is `BRONZE_NO_CONCAT_CHAIN=1`, read by the COMPILER once per
// process, because what it isolates is the emitted code.

#include <cstdlib>
#include <optional>
#include <vector>

#include "lower/lowerer.h"
#include "types/type.h"
#include "types/walk.h"

namespace bronze::lower {

namespace {

// Can evaluating this expression leave the block it started in?
//
// Answered by naming the constructs that BRANCH, and treating everything else
// as straight-line. Two of the entries are conservative rather than forced: a
// call does not split a block by itself, and neither does a function or class
// expression, but both are points a later pass may cut at or lift out of, and a
// spine that contains one is not a spine worth the risk of finding that out in
// a verifier failure on someone else's program.
class BlockSplitScan final : public types::Walker {
public:
    bool splits = false;

    // A nested function's BODY is lowered on its own, so nothing in it can
    // split the block this expression is being evaluated in. The expression
    // that MAKES the closure is refused above rather than here.
    void visit(const ast::FunctionExpr&) override { splits = true; }
    void visit(const ast::ClassExpr&) override { splits = true; }

    void visit(const ast::Ternary&) override { splits = true; }
    void visit(const ast::YieldExpr&) override { splits = true; }
    void visit(const ast::Call&) override { splits = true; }
    void visit(const ast::NewExpr&) override { splits = true; }
    void visit(const ast::SuperCall&) override { splits = true; }
    void visit(const ast::TaggedTemplate&) override { splits = true; }
    void visit(const ast::DynamicImportExpr&) override { splits = true; }

    // The short-circuiting operators and their assignment forms, which are
    // branches in the source however they are spelled.
    void visit(const ast::Binary& n) override {
        switch (n.op) {
            case ast::BinaryOp::LogicalAnd:
            case ast::BinaryOp::LogicalOr:
            case ast::BinaryOp::NullishCoalescing:
            case ast::BinaryOp::LogicalAndAssign:
            case ast::BinaryOp::LogicalOrAssign:
            case ast::BinaryOp::NullishAssign:
                splits = true;
                return;
            default:
                break;
        }
        types::Walker::visit(n);
    }

    // `a?.b`, `a?.[b]` — one link of an optional chain short-circuits the
    // whole chain (13.3.9), which is a branch.
    void visit(const ast::MemberAccess& n) override {
        if (n.optional) splits = true;
        types::Walker::visit(n);
    }
    void visit(const ast::IndexAccess& n) override {
        if (n.optional) splits = true;
        types::Walker::visit(n);
    }
};

bool mayLeaveBlock(const ast::Expr& e) {
    BlockSplitScan scan;
    e.accept(scan);
    return scan.splits;
}

// Is this operand a String on its face — a literal, or something inference
// typed? A spine with one of these in it produces a String from that operand
// on, by 13.15.3's own rule, so it is a spine that allocates an intermediate
// per `+` and is the one this lowering is for.
//
// The test is on the OPERANDS and not on the spine's own inferred type, because
// the answer has to be the same with and without inference for the shapes that
// matter: a `'-'` between two array reads is a String literal in both modes,
// where the spine's type is `string` in one and `dynamic` in the other.
bool looksLikeStringOperand(const ast::Expr& e, types::Type inferred) {
    if (dynamic_cast<const ast::StringLit*>(&e) != nullptr) return true;
    if (dynamic_cast<const ast::TemplateLit*>(&e) != nullptr) return true;
    return inferred.kind() == types::TypeKind::String;
}

// The operands of a left-associative `+` spine, in source order. Only the LEFT
// operand is descended into, because only the left is the same spine: `a + b +
// c` is `(a + b) + c` and has three operands, while `a + (b + c)` has two and
// the inner `+` is a spine of its own that lowers on its own terms.
//
// Parentheses around the left operand are not a boundary. `(a + b) + c` and `a
// + b + c` are the same tree with the same evaluation order, and 13.15.3 does
// not consult the source text.
void flattenAddSpine(const ast::Binary& bin, std::vector<const ast::Expr*>& out) {
    const auto* lhs = dynamic_cast<const ast::Binary*>(bin.lhs.get());
    if (lhs != nullptr && lhs->op == ast::BinaryOp::Add) {
        flattenAddSpine(*lhs, out);
    } else {
        out.push_back(bin.lhs.get());
    }
    out.push_back(bin.rhs.get());
}

// The seam, read once per compiler process: what it isolates is the emitted
// code, so a run-time flag could not change it. File-local rather than a
// `Lowerer` member because nothing outside this file consults it.
bool concatChainSeamDisabled() {
    static const bool off = std::getenv("BRONZE_NO_CONCAT_CHAIN") != nullptr;
    return off;
}

}  // namespace

std::optional<Lowerer::Value> Lowerer::lowerAddChain(const ast::Binary* bin,
                                                     il::Function& ilFn) {
    if (concatChainSeamDisabled()) return std::nullopt;

    std::vector<const ast::Expr*> operands;
    flattenAddSpine(*bin, operands);
    // Two operands is one `+`, which has no intermediate to save.
    if (operands.size() < 3) return std::nullopt;

    // A spine with no String in it is ARITHMETIC, whatever its operands turn
    // out to be, and arithmetic must not come through here. `e[0]*x + e[3]*y +
    // e[6]*z` is a three-operand spine, and its fold already lowers to unboxed
    // f64 adds over unboxed products — three.js's hot math is nothing else.
    // Boxing those to run them through an accumulator that would never mint
    // anything is a straight loss, so the accumulator is offered only where
    // 13.15.3 will certainly take its String branch.
    bool anyString = false;
    for (const ast::Expr* operand : operands) {
        if (looksLikeStringOperand(*operand, inferredType(*operand))) {
            anyString = true;
            break;
        }
    }
    if (!anyString) return std::nullopt;

    for (const ast::Expr* operand : operands) {
        if (mayLeaveBlock(*operand)) return std::nullopt;
    }

    auto accOpt = lowerExpr(*operands[0], ilFn);
    if (!accOpt) return std::nullopt;
    Value acc = boxValueIfNeeded(*accOpt, ilFn);

    for (size_t i = 1; i < operands.size(); ++i) {
        auto nextOpt = lowerExpr(*operands[i], ilFn);
        if (!nextOpt) return std::nullopt;
        Value next = boxValueIfNeeded(*nextOpt, ilFn);

        il::Instruction inst;
        inst.op = i == 1 ? il::Op::ConcatBegin : il::Op::ConcatAppend;
        inst.type = il::Type::Dynamic;
        inst.result = ilFn.valueCount++;
        inst.operands = {acc.id, next.id};
        // How many operands the accumulator still has to hold, which is what
        // sizes its first allocation. A hint and never a claim: a wrong count
        // costs a reallocation the growth policy would have made anyway.
        if (i == 1) inst.immI32 = static_cast<int32_t>(operands.size() - 2);
        emitInst(ilFn, inst);
        acc = Value{inst.result, il::Type::Dynamic};
    }

    il::Instruction end;
    end.op = il::Op::ConcatEnd;
    end.type = il::Type::Dynamic;
    end.result = ilFn.valueCount++;
    end.operands = {acc.id};
    emitInst(ilFn, end);
    return Value{end.result, il::Type::Dynamic};
}

}  // namespace bronze::lower
