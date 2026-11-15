// ARITHMETIC AND COMPARES on Dynamic operands.
//
// The generic forms (lower_expr_binary.cpp, lower_update.cpp) keep their
// operands boxed because a boxed value may be a String, a BigInt or an object
// with valueOf, and each of those is a different algorithm. Evidence
// (evidence.cpp planArith): the recorded VALUE site of every Dynamic operand
// saw Numbers only — `i32` (an int32-valued Number in either box) and/or
// `f64` (any other) — at least kMinSiteObservations times. A typed operand
// counts as its type.
//
// INT FORM — every operand held int32 values only, and the operation has an
// exact int32 form:
//     guard is.i32 %a ; x = unbox.i32.unchecked %a     (per Dynamic operand)
//     guard !add.would_ovf x, y                         (add, sub, mul, ++/--)
//     s = add x, y          : i32 (wrapping, exact on the guard's edge)
//     r = box.i32 s
// DOUBLE FORM — every operand held Numbers:
//     f64 only:   guard is.number %a ; x = unbox.f64.unchecked %a
//     i32 only:   guard is.i32 %a    ; x = i32.to_f64 (unbox.i32.unchecked %a)
//     both:       br is.number %a, D, I
//                 D: unbox.f64.unchecked -> J ; I: guard is.i32, convert -> J
//     s = add x, y          : f64
//     r = box.f64 s
// (`is.number` tests the double box only, so a value in the Int32 box needs
// the second arm.) The result is boxed again so the rest of S, written
// against the Dynamic result, is unchanged; a representation pass removes the
// box/unbox pairs later.
//
// Why exact: on Numbers each generic operation IS the typed one — 13.15.3's
// ApplyStringOrNumericBinaryOperator with two Numbers is Number::add and its
// siblings (IEEE-754 double arithmetic, `%` the truncating remainder the typed
// `mod` computes), IsLessThan with two Numbers is the ordered compare
// (cmp.le / cmp.ge answer false for NaN as 13.10 does), IsStrictlyEqual and
// IsLooselyEqual of two Numbers are Number::equal, ToBoolean is `num.truthy`,
// ToNumber and ToNumeric of a Number are the Number. In the int form the
// int32 result equals the double one exactly when the overflow test says it is
// an int32 (mul's also excludes -0); `-x` has no int form (-0, and -(-2^31)),
// nor do `/` and `%`. Every guard is a pure test and every instruction before
// the last guard pure, so a failure re-runs the generic operation in G.
//
// Refused (evidence.cpp gives no plan): any operand that saw another tag (a
// String makes `+` a concatenation), `**` (its operands are unboxed by a
// checked `unbox.f64` first, which this does speculate), the bitwise binary
// operators (their operands are already `to.int32`'d i32 values), and an
// operation with no Dynamic operand.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

bool isIntOnly(uint8_t tags) { return tags == kTagInt; }

il::Op compareFor(il::Op op) {
    switch (op) {
        case il::Op::RelLt: return il::Op::CmpLt;
        case il::Op::RelGt: return il::Op::CmpGt;
        case il::Op::RelLe: return il::Op::CmpLe;
        case il::Op::RelGe: return il::Op::CmpGe;
        default: return il::Op::CmpEq;  // strict.eq, loose.eq
    }
}

il::Op overflowTestFor(il::Op op) {
    switch (op) {
        case il::Op::Add: return il::Op::AddWouldOvf;
        case il::Op::Sub: return il::Op::SubWouldOvf;
        default: return il::Op::MulWouldOvf;
    }
}

// Whether the operation has an exact int32 form.
bool hasIntForm(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul:
        case il::Op::BitNot: case il::Op::ToNumeric: case il::Op::NumericStep: case il::Op::ToInt32:
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe: case il::Op::RelGe:
        case il::Op::StrictEq: case il::Op::LooseEq: case il::Op::Unbox:
            return true;
        default:
            return false;  // div, mod, neg
    }
}

// The int32 a Dynamic or I32 operand holds, guarded.
ValueId asInt(GuardBuilder& b, ValueId v) {
    if (b.typeOf(v) == il::Type::I32) return v;
    assert(b.typeOf(v) == il::Type::Dynamic);
    b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {v}));
    return b.emit(il::Op::UnboxI32, il::Type::I32, {v});
}

// The double a Number operand holds, guarded by the tags it was seen with.
ValueId asDouble(GuardBuilder& b, ValueId v, uint8_t tags) {
    switch (b.typeOf(v)) {
        case il::Type::F64: return v;
        case il::Type::I32: return b.emit(il::Op::I32ToF64, il::Type::F64, {v});
        default: break;
    }
    assert(b.typeOf(v) == il::Type::Dynamic && tags != 0);
    if (tags == kTagDouble) {
        b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {v}));
        return b.emit(il::Op::UnboxF64, il::Type::F64, {v});
    }
    if (tags == kTagInt) {
        return b.emit(il::Op::I32ToF64, il::Type::F64, {asInt(b, v)});
    }
    // Both: the double box, or the Int32 box, or leave.
    const ValueId inDoubleBox = b.emit(il::Op::IsNumber, il::Type::Bool, {v});
    const BlockId dbl = b.newBlock();
    const BlockId other = b.newBlock();
    const BlockId join = b.newBlock();
    const ValueId joined = b.addParam(join, il::Type::F64);
    b.branch(inDoubleBox, dbl, {}, other, {});
    b.moveTo(dbl);
    b.jump(join, {b.emit(il::Op::UnboxF64, il::Type::F64, {v})});
    b.moveTo(other);
    b.jump(join, {b.emit(il::Op::I32ToF64, il::Type::F64, {asInt(b, v)})});
    b.moveTo(join);
    return joined;
}

// Guards that a Dynamic operand is a Number of the tags seen, converting
// nothing (ToNumeric of a Number is the Number itself).
void guardNumber(GuardBuilder& b, ValueId v, uint8_t tags) {
    if (tags == kTagInt) {
        b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {v}));
        return;
    }
    if (tags == kTagDouble) {
        b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {v}));
        return;
    }
    const ValueId inDoubleBox = b.emit(il::Op::IsNumber, il::Type::Bool, {v});
    const BlockId other = b.newBlock();
    const BlockId join = b.newBlock();
    b.branch(inDoubleBox, join, {}, other, {});
    b.moveTo(other);
    b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {v}));
    b.jump(join);
    b.moveTo(join);
}

// Defines the site's result as `v` through a join parameter: the IL has no
// copy, and a later pass folds the trivial parameter away.
void bindResult(GuardBuilder& b, ValueId v) {
    const BlockId join = b.newBlock();
    b.resultParam(join);
    b.jump(join, {v});
    b.moveTo(join);
}

void emitResultOp(GuardBuilder& b, il::Op op, il::Type type, std::vector<ValueId> operands,
                  il::Type boxType = il::Type::Void) {
    il::Instruction inst;
    inst.op = op;
    inst.type = type;
    inst.operands = std::move(operands);
    inst.boxType = boxType;
    b.emitResult(std::move(inst));
}

void boxResult(GuardBuilder& b, ValueId v) { emitResultOp(b, il::Op::Box, il::Type::Dynamic, {v}, b.typeOf(v)); }

}  // namespace

bool arithSupported(const il::Instruction& inst) {
    const size_t n = inst.operands.size();
    switch (inst.op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul: case il::Op::Div: case il::Op::Mod:
            return inst.type == il::Type::Dynamic && n == 2;
        case il::Op::Neg: case il::Op::BitNot: case il::Op::ToNumeric: case il::Op::NumericStep:
            return inst.type == il::Type::Dynamic && n == 1;
        case il::Op::ToInt32:
            return inst.type == il::Type::I32 && n == 1;
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe: case il::Op::RelGe:
        case il::Op::StrictEq: case il::Op::LooseEq:
            return inst.type == il::Type::Bool && n == 2;
        case il::Op::Unbox:
            // Truthiness and ToNumber; the int32 unbox reads a number already
            // tested for and has nothing to speculate.
            return (inst.type == il::Type::Bool || inst.type == il::Type::F64) && n == 1;
        default:
            return false;
    }
}

std::optional<ArithForm> chooseArithForm(const il::Instruction& inst, const std::vector<uint8_t>& operandTags) {
    if (!arithSupported(inst) || operandTags.size() != inst.operands.size()) return std::nullopt;
    bool allInt = true;
    for (uint8_t t : operandTags) {
        if (t == 0 || (t & ~(kTagInt | kTagDouble)) != 0) return std::nullopt;
        allInt = allInt && isIntOnly(t);
    }
    if (allInt && hasIntForm(inst)) return ArithForm::Int;
    return ArithForm::Double;
}

void rewriteArith(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::Arith);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(arithSupported(site) && plan.operandTags.size() == site.operands.size());
    const bool asInts = plan.form == ArithForm::Int;
    assert(!asInts || hasIntForm(site));

    if (site.op == il::Op::ToNumeric) {
        assert(b.typeOf(site.operands[0]) == il::Type::Dynamic);
        guardNumber(b, site.operands[0], asInts ? kTagInt : plan.operandTags[0]);
        bindResult(b, site.operands[0]);
        b.finish();
        ++ctx.stats.arithmetic;
        return;
    }

    std::vector<ValueId> xs;
    for (size_t i = 0; i < site.operands.size(); ++i) {
        xs.push_back(asInts ? asInt(b, site.operands[i]) : asDouble(b, site.operands[i], plan.operandTags[i]));
    }
    const il::Type num = asInts ? il::Type::I32 : il::Type::F64;

    switch (site.op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul: case il::Op::Div: case il::Op::Mod: {
            if (asInts) b.guard(b.emit(overflowTestFor(site.op), il::Type::Bool, {xs[0], xs[1]}), false);
            boxResult(b, b.emit(site.op, num, {xs[0], xs[1]}));
            break;
        }
        case il::Op::Neg:
            assert(!asInts);
            boxResult(b, b.emit(il::Op::Neg, il::Type::F64, {xs[0]}));
            break;
        case il::Op::NumericStep: {
            const int32_t step = site.immI32;
            assert(step == 1 || step == -1);
            if (asInts) {
                const ValueId one = b.constI32(step);
                b.guard(b.emit(il::Op::AddWouldOvf, il::Type::Bool, {xs[0], one}), false);
                boxResult(b, b.emit(il::Op::Add, il::Type::I32, {xs[0], one}));
            } else {
                boxResult(b, b.emit(il::Op::Add, il::Type::F64, {xs[0], b.constF64(step)}));
            }
            break;
        }
        case il::Op::BitNot: {
            // `~x` on a Number is ToInt32(x) ^ -1 (13.5.6.1 on 6.1.6.1.2).
            ValueId i = xs[0];
            if (!asInts) {
                il::Instruction conv;
                conv.op = il::Op::ToInt32;
                conv.type = il::Type::I32;
                conv.boxType = il::Type::F64;
                conv.operands = {xs[0]};
                i = b.emitInst(std::move(conv));
            }
            boxResult(b, b.emit(il::Op::BitXor, il::Type::F64, {i, b.constI32(-1)}));
            break;
        }
        case il::Op::ToInt32:
            if (asInts) {
                bindResult(b, xs[0]);
            } else {
                emitResultOp(b, il::Op::ToInt32, il::Type::I32, {xs[0]}, il::Type::F64);
            }
            break;
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe: case il::Op::RelGe:
        case il::Op::StrictEq: case il::Op::LooseEq:
            emitResultOp(b, compareFor(site.op), il::Type::Bool, {xs[0], xs[1]});
            break;
        case il::Op::Unbox:
            if (site.type == il::Type::Bool) {
                if (asInts) {
                    emitResultOp(b, il::Op::CmpNe, il::Type::Bool, {xs[0], b.constI32(0)});
                } else {
                    emitResultOp(b, il::Op::NumTruthy, il::Type::Bool, {xs[0]});
                }
            } else {
                assert(site.type == il::Type::F64);
                bindResult(b, asInts ? b.emit(il::Op::I32ToF64, il::Type::F64, {xs[0]}) : xs[0]);
            }
            break;
        default:
            assert(false && "arithSupported admitted an operation rewriteArith does not know");
            break;
    }
    b.finish();
    ++ctx.stats.arithmetic;
}

}  // namespace bronze::opt::speculate
