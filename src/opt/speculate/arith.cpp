// ARITHMETIC, BITWISE OPS AND COMPARES on Dynamic operands.
//
// The generic forms (lower_expr_binary.cpp, lower_update.cpp) keep their
// operands boxed because a boxed value may be a String, a BigInt or an object
// with valueOf, and each of those is a different algorithm. Evidence
// (evidence.cpp planArith): every Dynamic operand held Numbers only — a
// harvest's arms, or a recorded VALUE site — with per operand whether it was
// always an int32 (kTagInt) or any Number (kTagDouble), and for `+ - ++ --`
// whether the result stayed an int32. A typed operand counts as its type.
//
// A boxed Number is always the double's bits (runtime/value.h), so `is.number`
// is one compare and `unbox.f64.unchecked` a move, while `is.i32` converts and
// compares. The FORM is chosen so that an int32 test is made only where an
// int32 operation pays for it (chooseArithForm):
//
// INT — `+ -` and `++ --` whose operands and result were int32s, `%` whose
// int32 arm was the only one run, and the compares of two int32s:
//     guard is.i32 %a ; x = unbox.i32.unchecked %a     (per Dynamic operand)
//     guard !add.would_ovf x, y                         (add, sub, ++, --)
//     guard x >= 0 ; guard y > 0                         (`%`)
//     s = add x, y          : i32
//     r = box.i32 s
// DOUBLE — everything else on Numbers, `*` and `/` always (an int32 product
// overflows and has -0; a quotient is a fraction):
//     guard is.number %a ; x = unbox.f64.unchecked %a  (per Dynamic operand)
//     s = mul x, y          : f64
//     r = box.f64 s
// BITS — `& | ^ << >> >>> ~` and ToInt32, whose operands become int32s by
// ToInt32 whatever Number they hold, so a fraction is no reason to leave and
// an int32 test would buy nothing:
//     guard is.number %a ; f = unbox.f64.unchecked %a ; x = to.int32 f
//     s = and x, y          : i32   (`>>>`: f64, its result is a uint32)
//     r = box.i32 s
// The result is boxed again so the rest of S, written against the Dynamic
// result, is unchanged; representation selection (opt/repr) removes the
// box/unbox pairs between operations and unboxes loop-carried values.
//
// Why exact: on Numbers each generic operation IS the typed one — 13.15.3's
// ApplyStringOrNumericBinaryOperator with two Numbers is Number::add and its
// siblings (IEEE-754 double arithmetic; `%` on doubles the truncating
// remainder; the bitwise ops 6.1.6.1.17-19 on ToInt32 of each side, shifts on
// ToUint32 of the count masked to five bits), IsLessThan with two Numbers is
// the ordered compare (cmp.le / cmp.ge answer false for NaN as 13.10 does),
// IsStrictlyEqual and IsLooselyEqual of two Numbers are Number::equal,
// ToBoolean is `num.truthy`, ToNumeric of a Number is the Number, and unary
// `-` flips the sign. In the int form the int32 result equals the double one
// exactly when the overflow test says it is an int32, and `%` of a
// non-negative int32 by a positive one is the i32 remainder (no -0, no NaN, no
// division by zero). Every guard is a pure test and every instruction before
// the last guard pure, so a failure re-runs the generic operation in G.
//
// Refused (evidence.cpp gives no plan): any operand that saw another tag (a
// String makes `+` a concatenation), `**` (its operands are unboxed by a
// checked `unbox.f64` first, which this does speculate), and an operation
// with no Dynamic operand.

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

// The int32 a Dynamic or I32 operand holds, guarded.
ValueId asInt(GuardBuilder& b, ValueId v) {
    if (b.typeOf(v) == il::Type::I32) return v;
    assert(b.typeOf(v) == il::Type::Dynamic);
    b.guard(b.emit(il::Op::IsI32, il::Type::Bool, {v}));
    return b.emit(il::Op::UnboxI32, il::Type::I32, {v});
}

// The double a Number operand holds. A Number has one box, the double's bits
// (runtime/value.h), so `is.number` admits every Number whatever tags it was
// seen with, and the unbox reads it.
ValueId asDouble(GuardBuilder& b, ValueId v) {
    switch (b.typeOf(v)) {
        case il::Type::F64: return v;
        case il::Type::I32: return b.emit(il::Op::I32ToF64, il::Type::F64, {v});
        default: break;
    }
    assert(b.typeOf(v) == il::Type::Dynamic);
    b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {v}));
    return b.emit(il::Op::UnboxF64, il::Type::F64, {v});
}

ValueId toInt32(GuardBuilder& b, ValueId f) {
    il::Instruction conv;
    conv.op = il::Op::ToInt32;
    conv.type = il::Type::I32;
    conv.boxType = il::Type::F64;
    conv.operands = {f};
    return b.emitInst(std::move(conv));
}

// ToInt32 of a Number operand, truncated inline whatever Number it holds: a
// fraction is no reason to leave, and over an int32 box the test and both
// conversions fold away (opt/repr).
ValueId asInt32Bits(GuardBuilder& b, ValueId v) {
    switch (b.typeOf(v)) {
        case il::Type::I32: return v;
        case il::Type::F64: return toInt32(b, v);
        default: return toInt32(b, asDouble(b, v));
    }
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

void rewriteBits(GuardBuilder& b, const il::Instruction& site) {
    std::vector<ValueId> xs;
    for (ValueId v : site.operands) xs.push_back(asInt32Bits(b, v));
    switch (site.op) {
        case il::Op::ToInt32:
            bindResult(b, xs[0]);
            return;
        case il::Op::BitNot:
            // `~x` on a Number is ToInt32(x) ^ -1 (13.5.6.1 on 6.1.6.1.2).
            boxResult(b, b.emit(il::Op::BitXor, il::Type::I32, {xs[0], b.constI32(-1)}));
            return;
        default:
            // `>>>` is the one whose result is a uint32, a Number an i32
            // cannot hold.
            boxResult(b, b.emit(site.op, site.op == il::Op::UShr ? il::Type::F64 : il::Type::I32, {xs[0], xs[1]}));
            return;
    }
}

}  // namespace

bool arithSupported(const il::Instruction& inst) {
    const size_t n = inst.operands.size();
    switch (inst.op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul: case il::Op::Div: case il::Op::Mod:
        case il::Op::BitAnd: case il::Op::BitOr: case il::Op::BitXor:
        case il::Op::Shl: case il::Op::Shr: case il::Op::UShr:
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

bool numericArms(const il::Instruction& inst) {
    if (!arithSupported(inst)) return false;
    switch (inst.op) {
        case il::Op::ToInt32:
        case il::Op::StrictEq:
        case il::Op::Unbox:
            return false;
        default:
            return true;
    }
}

std::optional<ArithForm> chooseArithForm(const il::Instruction& inst, const std::vector<uint8_t>& operandTags,
                                         bool resultDouble) {
    if (!arithSupported(inst) || operandTags.size() != inst.operands.size()) return std::nullopt;
    bool allInt = true;
    for (uint8_t t : operandTags) {
        if (t == 0 || (t & ~(kTagInt | kTagDouble)) != 0) return std::nullopt;
        allInt = allInt && isIntOnly(t);
    }
    switch (inst.op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::NumericStep:
            return allInt && !resultDouble ? ArithForm::Int : ArithForm::Double;
        case il::Op::Mod:
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe: case il::Op::RelGe:
        case il::Op::StrictEq: case il::Op::LooseEq:
        case il::Op::Unbox:
            return allInt ? ArithForm::Int : ArithForm::Double;
        case il::Op::BitAnd: case il::Op::BitOr: case il::Op::BitXor:
        case il::Op::Shl: case il::Op::Shr: case il::Op::UShr:
        case il::Op::BitNot: case il::Op::ToInt32:
            return ArithForm::Bits;
        default:  // mul, div, neg, to.numeric
            return ArithForm::Double;
    }
}

void rewriteArith(RewriteContext& ctx, const SitePlan& plan) {
    assert(plan.kind == SiteKind::Arith);
    GuardBuilder b(ctx, plan.block, plan.index);
    const il::Instruction site = b.site();
    assert(arithSupported(site) && plan.operandTags.size() == site.operands.size());

    if (plan.form == ArithForm::Bits) {
        rewriteBits(b, site);
        b.finish();
        ++ctx.stats.arithmetic;
        return;
    }
    if (site.op == il::Op::ToNumeric) {
        assert(b.typeOf(site.operands[0]) == il::Type::Dynamic);
        b.guard(b.emit(il::Op::IsNumber, il::Type::Bool, {site.operands[0]}));
        bindResult(b, site.operands[0]);
        b.finish();
        ++ctx.stats.arithmetic;
        return;
    }

    const bool asInts = plan.form == ArithForm::Int;
    std::vector<ValueId> xs;
    for (ValueId v : site.operands) xs.push_back(asInts ? asInt(b, v) : asDouble(b, v));
    const il::Type num = asInts ? il::Type::I32 : il::Type::F64;

    switch (site.op) {
        case il::Op::Add: case il::Op::Sub:
            if (asInts) {
                const il::Op test = site.op == il::Op::Add ? il::Op::AddWouldOvf : il::Op::SubWouldOvf;
                b.guard(b.emit(test, il::Type::Bool, {xs[0], xs[1]}), false);
            }
            boxResult(b, b.emit(site.op, num, {xs[0], xs[1]}));
            break;
        case il::Op::Mod:
            if (asInts) {
                b.guard(b.emit(il::Op::CmpGe, il::Type::Bool, {xs[0], b.constI32(0)}));
                b.guard(b.emit(il::Op::CmpGt, il::Type::Bool, {xs[1], b.constI32(0)}));
            }
            boxResult(b, b.emit(il::Op::Mod, num, {xs[0], xs[1]}));
            break;
        case il::Op::Mul: case il::Op::Div:
            assert(!asInts);
            boxResult(b, b.emit(site.op, il::Type::F64, {xs[0], xs[1]}));
            break;
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
