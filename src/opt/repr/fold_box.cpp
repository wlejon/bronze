// Rules 1 and 5: an operation whose dynamic operand is `box.<t> %x` with %x
// typed is answered from %x.
//
// Each rewrite, and why it is exact:
//   tag tests        the box's tags are fixed by its type (encoding.h
//                    `boxedForm`); `is.i32` of a double box is decided only
//                    when the double is exactly an int32 (rewrite.h
//                    `exactInt32`) or a constant that is not.
//   unbox.i32.unchecked (box.i32 x)            -> x
//   unbox.i32.unchecked (box.f64 y)            -> the int32 under y: legal only
//                    where `is.i32` held, i.e. y is an integral int32 double,
//                    and `to.int32 y` (7.1.6) is the identity on those; it is
//                    total and pure, so it no longer needs the licence.
//   unbox.f64.unchecked (box.f64 y)            -> y
//   unbox.f64.unchecked (box.i32 x)            -> i32.to_f64 x, as the
//                    encoding says that unbox reads an int32's box.
//   unbox.f64 (checked: ToNumber) of a Number  -> the Number as f64.
//   unbox.i32 (checked) of box.i32 x           -> x.
//   unbox.bool (ToBoolean, 7.1.2)              -> b for a Boolean, `num.truthy`
//                    for a double (false at +-0 and NaN), `cmp.ne x, 0` for an
//                    int32 (no NaN, no -0).
//   to.numeric of a Number                     -> the operand itself (7.1.3
//                    returns a Number unchanged).
//   to.int32 (boxed) of a Number or Boolean    -> to.int32 of the typed value:
//                    7.1.6 step 1 is ToNumber, a machine conversion there.
//   strict.eq / loose.eq of two Numbers        -> cmp.eq (Number::equal is the
//                    IEEE compare: NaN unequal, +0 equal to -0); of two
//                    Booleans -> cmp.eq on bool; strict.eq of a Boolean and a
//                    Number -> false (different types).
//   rel.* of two Numbers                       -> cmp.* (13.10.1 IsLessThan on
//                    Numbers; cmp.* answers false at NaN as 13.10 does).
// Mixed i32/f64 compare operands meet as f64 through `i32.to_f64`, which is
// exact.
//   cmp.* on f64 operands that are each exactly an int32 (rewrite.h
//   exactInt32: `i32.to_f64 x` or an int32 constant) -> cmp.* on the
//                    int32s: the conversion is exact and keeps order, so the
//                    double compare and the int32 one agree on every input.
//
// A replaced instruction is deleted explicitly rather than left to dead code:
// a checked unbox or a boxed conversion calls unknown code by the effect
// table, which no dead-code pass may delete, though on a Number it does
// nothing.

#include <cassert>
#include <optional>

#include "il/effects.h"
#include "opt/core/cfg.h"
#include "opt/core/uses.h"
#include "opt/redundancy/fold.h"
#include "opt/repr/encoding.h"
#include "opt/repr/phases.h"
#include "opt/repr/rewrite.h"

namespace bronze::opt::repr {

namespace {

using il::Op;
using il::Type;

bool isNumeric(Type t) { return t == Type::I32 || t == Type::F64; }

class BoxFolder {
public:
    explicit BoxFolder(PhaseContext& ctx)
        : ctx_(ctx), fn_(ctx.fn), defs_(ctx.fn), repl_(ctx.fn.valueCount, kNoValue) {}

    bool run();

private:
    ValueId resolve(ValueId v) const {
        while (v < repl_.size() && repl_[v] != kNoValue) v = repl_[v];
        return v;
    }
    std::optional<BoxSource> source(ValueId v) const { return boxSource(fn_, defs_, v); }

    void visit(BlockId b, size_t i);
    // The instruction at (b, i) computes `by`; it goes.
    void replaceBy(BlockId b, size_t i, ValueId by);

    bool foldTest(il::Instruction& inst, const BoxSource& s);
    bool foldUnboxI32(BlockId b, size_t i, const BoxSource& s);
    bool foldUnboxF64(BlockId b, size_t i, const BoxSource& s);
    bool foldCheckedUnbox(BlockId b, size_t i, const BoxSource& s);
    bool foldToInt32(BlockId b, size_t i, const BoxSource& s);
    bool foldEquality(BlockId b, size_t i, const BoxSource& l, const BoxSource& r);
    bool foldRelational(BlockId b, size_t i, const BoxSource& l, const BoxSource& r);
    bool narrowCompare(BlockId b, size_t i);
    // `x` as an f64 operand: itself, or an `i32.to_f64` planned before (b, i).
    ValueId asF64(BlockId b, size_t i, const BoxSource& s);

    PhaseContext& ctx_;
    il::Function& fn_;
    const DefUse defs_;
    std::vector<ValueId> repl_;
    EditPlan edits_;
    bool changed_ = false;
};

bool BoxFolder::run() {
    const Cfg cfg(fn_);
    // Reverse postorder: a box is seen before the uses it dominates, so a
    // chain of identities resolves in one walk. Back-edge uses are fixed by
    // the replaceAllUses below.
    for (BlockId b : cfg.rpo()) {
        const size_t n = fn_.blocks[b].instructions.size();
        for (size_t i = 0; i < n; ++i) visit(b, i);
    }
    if (!changed_) return false;
    replaceAllUses(fn_, repl_);
    edits_.apply(fn_);
    return true;
}

void BoxFolder::replaceBy(BlockId b, size_t i, ValueId by) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    assert(inst.result != kNoValue && by != inst.result);
    assert(defs_.type(by) == inst.type);
    repl_[inst.result] = by;
    edits_.remove(b, i);
    changed_ = true;
}

void BoxFolder::visit(BlockId b, size_t i) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    for (ValueId& v : inst.operands) v = resolve(v);
    for (ValueId& v : inst.target.args) v = resolve(v);
    for (ValueId& v : inst.elseTarget.args) v = resolve(v);
    if (inst.result == kNoValue || inst.operands.empty()) return;
    const std::optional<BoxSource> s = source(inst.operands[0]);

    switch (inst.op) {
        case Op::IsI32:
        case Op::IsNumber:
        case Op::IsObject:
        case Op::IsNullish:
        case Op::IsHole:
        case Op::IsUninit:
            if (s && inst.operands.size() == 1 && foldTest(inst, *s)) {
                ++ctx_.stats.testsFolded;
                changed_ = true;
            }
            return;
        case Op::UnboxI32:
            if (s && foldUnboxI32(b, i, *s)) ++ctx_.stats.unboxesFolded;
            return;
        case Op::UnboxF64:
            if (s && foldUnboxF64(b, i, *s)) ++ctx_.stats.unboxesFolded;
            return;
        case Op::Unbox:
            if (s && foldCheckedUnbox(b, i, *s)) ++ctx_.stats.conversionsFolded;
            return;
        case Op::ToNumeric:
            if (s && isNumeric(s->type) && inst.type == Type::Dynamic) {
                replaceBy(b, i, inst.operands[0]);
                ++ctx_.stats.conversionsFolded;
            }
            return;
        case Op::ToInt32:
            if (inst.boxType == Type::F64 && inst.type == Type::I32) {
                // ToInt32 of a double that is an exact int32 is that int32.
                if (const std::optional<Int32Source> k = exactInt32(fn_, defs_, inst.operands[0])) {
                    if (!k->isConst) {
                        replaceBy(b, i, k->value);
                    } else {
                        redundancy::makeI32Constant(inst, k->constant);
                        changed_ = true;
                    }
                    ++ctx_.stats.conversionsFolded;
                }
                return;
            }
            if (s && inst.boxType == Type::Dynamic && foldToInt32(b, i, *s)) ++ctx_.stats.conversionsFolded;
            return;
        case Op::CmpLt:
        case Op::CmpGt:
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
            if (narrowCompare(b, i)) ++ctx_.stats.comparesTyped;
            return;
        case Op::StrictEq:
        case Op::LooseEq:
        case Op::RelLt:
        case Op::RelGt:
        case Op::RelLe:
        case Op::RelGe: {
            if (!s || inst.operands.size() != 2 || inst.type != Type::Bool) return;
            const std::optional<BoxSource> r = source(inst.operands[1]);
            if (!r) return;
            const bool done = (inst.op == Op::StrictEq || inst.op == Op::LooseEq)
                                  ? foldEquality(b, i, *s, *r)
                                  : foldRelational(b, i, *s, *r);
            if (done) ++ctx_.stats.comparesTyped;
            return;
        }
        default:
            return;
    }
}

bool BoxFolder::foldTest(il::Instruction& inst, const BoxSource& s) {
    const std::optional<BoxedForm> form = boxedForm(s.type);
    if (!form || inst.type != Type::Bool) return false;
    std::optional<bool> answer;
    switch (inst.op) {
        case Op::IsI32:
            answer = form->isI32;
            // A double box passes exactly when its value is an int32 Number.
            if (!answer && s.type == Type::F64) {
                if (exactInt32(fn_, defs_, s.value)) {
                    answer = true;
                } else if (const std::optional<double> c = constF64(fn_, defs_, s.value)) {
                    answer = isInt32Double(*c);
                }
            }
            break;
        case Op::IsNumber: answer = form->isNumber; break;
        case Op::IsObject: answer = form->isObject; break;
        case Op::IsNullish: answer = form->isNullish; break;
        case Op::IsHole: answer = form->isHole; break;
        // No box is the uninitialized marker.
        case Op::IsUninit: answer = false; break;
        default: return false;
    }
    if (!answer) return false;
    redundancy::makeBoolConstant(inst, *answer);
    return true;
}

bool BoxFolder::foldUnboxI32(BlockId b, size_t i, const BoxSource& s) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (inst.type != Type::I32) return false;
    if (s.type == Type::I32) {
        replaceBy(b, i, s.value);
        return true;
    }
    if (s.type != Type::F64) return false;
    if (const std::optional<Int32Source> k = exactInt32(fn_, defs_, s.value)) {
        if (!k->isConst) {
            replaceBy(b, i, k->value);
        } else {
            redundancy::makeI32Constant(inst, k->constant);
            changed_ = true;
        }
        return true;
    }
    // Under its `is.i32` licence the double is an integral int32, which
    // to.int32 returns unchanged.
    rewriteInPlace(inst, Op::ToInt32, {s.value}, Type::F64);
    changed_ = true;
    return true;
}

bool BoxFolder::foldUnboxF64(BlockId b, size_t i, const BoxSource& s) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (inst.type != Type::F64) return false;
    if (s.type == Type::F64) {
        replaceBy(b, i, s.value);
        return true;
    }
    const std::optional<BoxedForm> form = boxedForm(s.type);
    if (s.type == Type::I32 && form && form->unboxF64ReadsValue) {
        rewriteInPlace(inst, Op::I32ToF64, {s.value});
        changed_ = true;
        return true;
    }
    return false;
}

bool BoxFolder::foldCheckedUnbox(BlockId b, size_t i, const BoxSource& s) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (inst.type == s.type && s.type != Type::Str) {
        // ToNumber of a Number, ToBoolean of a Boolean, the int32 of an int32.
        replaceBy(b, i, s.value);
        return true;
    }
    switch (inst.type) {
        case Type::F64:
            if (s.type != Type::I32) return false;
            rewriteInPlace(inst, Op::I32ToF64, {s.value});
            changed_ = true;
            return true;
        case Type::Bool:
            if (s.type == Type::F64) {
                rewriteInPlace(inst, Op::NumTruthy, {s.value});
                changed_ = true;
                return true;
            }
            if (s.type == Type::I32) {
                il::Instruction zero = makeConstI32(fn_, 0, inst.span);
                const ValueId z = zero.result;
                edits_.insertBefore(b, i, std::move(zero));
                rewriteInPlace(inst, Op::CmpNe, {s.value, z});
                changed_ = true;
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool BoxFolder::foldToInt32(BlockId b, size_t i, const BoxSource& s) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (inst.type != Type::I32) return false;
    switch (s.type) {
        case Type::I32:
            replaceBy(b, i, s.value);
            return true;
        case Type::F64:
        case Type::Bool:
            // The typed forms are machine conversions; ToNumber(true) is 1.
            rewriteInPlace(inst, Op::ToInt32, {s.value}, s.type);
            changed_ = true;
            return true;
        default:
            return false;
    }
}

ValueId BoxFolder::asF64(BlockId b, size_t i, const BoxSource& s) {
    if (s.type == Type::F64) return s.value;
    assert(s.type == Type::I32);
    il::Instruction conv = makeI32ToF64(fn_, s.value, fn_.blocks[b].instructions[i].span);
    const ValueId v = conv.result;
    edits_.insertBefore(b, i, std::move(conv));
    return v;
}

bool BoxFolder::foldEquality(BlockId b, size_t i, const BoxSource& l, const BoxSource& r) {
    il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (isNumeric(l.type) && isNumeric(r.type)) {
        if (l.type == r.type) {
            rewriteInPlace(inst, Op::CmpEq, {l.value, r.value});
        } else {
            const ValueId a = asF64(b, i, l);
            const ValueId c = asF64(b, i, r);
            // asF64 may have grown the block's plan, not the block: `inst`
            // is still valid.
            rewriteInPlace(fn_.blocks[b].instructions[i], Op::CmpEq, {a, c});
        }
        changed_ = true;
        return true;
    }
    if (l.type == Type::Bool && r.type == Type::Bool) {
        rewriteInPlace(inst, Op::CmpEq, {l.value, r.value});
        changed_ = true;
        return true;
    }
    // A Boolean is never strictly equal to a Number. (Loosely it may be:
    // `true == 1`, which converts; that is left alone.)
    const bool boolAndNumber = (l.type == Type::Bool && isNumeric(r.type)) ||
                               (r.type == Type::Bool && isNumeric(l.type));
    if (inst.op == Op::StrictEq && boolAndNumber) {
        redundancy::makeBoolConstant(inst, false);
        changed_ = true;
        return true;
    }
    return false;
}

bool BoxFolder::foldRelational(BlockId b, size_t i, const BoxSource& l, const BoxSource& r) {
    if (!isNumeric(l.type) || !isNumeric(r.type)) return false;
    Op op = Op::CmpLt;
    switch (fn_.blocks[b].instructions[i].op) {
        case Op::RelLt: op = Op::CmpLt; break;
        case Op::RelGt: op = Op::CmpGt; break;
        case Op::RelLe: op = Op::CmpLe; break;
        case Op::RelGe: op = Op::CmpGe; break;
        default: return false;
    }
    ValueId a = l.value;
    ValueId c = r.value;
    if (l.type != r.type) {
        a = asF64(b, i, l);
        c = asF64(b, i, r);
    }
    rewriteInPlace(fn_.blocks[b].instructions[i], op, {a, c});
    changed_ = true;
    return true;
}

bool BoxFolder::narrowCompare(BlockId b, size_t i) {
    const il::Instruction& inst = fn_.blocks[b].instructions[i];
    if (inst.operands.size() != 2 || defs_.type(inst.operands[0]) != Type::F64 ||
        defs_.type(inst.operands[1]) != Type::F64) {
        return false;
    }
    std::optional<Int32Source> sides[2];
    bool converted = false;
    for (int k = 0; k < 2; ++k) {
        sides[k] = exactInt32(fn_, defs_, inst.operands[k]);
        if (!sides[k]) return false;
        converted = converted || !sides[k]->isConst;
    }
    // Two constants are the fold's.
    if (!converted) return false;
    ValueId xs[2];
    for (int k = 0; k < 2; ++k) {
        if (!sides[k]->isConst) {
            xs[k] = sides[k]->value;
            continue;
        }
        il::Instruction c = makeConstI32(fn_, sides[k]->constant, fn_.blocks[b].instructions[i].span);
        xs[k] = c.result;
        edits_.insertBefore(b, i, std::move(c));
    }
    il::Instruction& site = fn_.blocks[b].instructions[i];
    rewriteInPlace(site, site.op, {xs[0], xs[1]});
    changed_ = true;
    return true;
}

}  // namespace

bool foldBoxedUses(PhaseContext& ctx) {
    if (ctx.fn.blocks.empty()) return false;
    BoxFolder folder(ctx);
    return folder.run();
}

}  // namespace bronze::opt::repr
