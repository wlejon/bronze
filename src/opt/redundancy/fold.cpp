#include "opt/redundancy/fold.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace bronze::opt::redundancy {

namespace {

using il::Op;
using il::Type;
using Kind = FoldOutcome::Kind;

std::optional<int32_t> constI32(const ValueView& values, ValueId v) {
    const il::Instruction* d = values.defIs(v, Op::ConstI32);
    if (d == nullptr || d->type != Type::I32) return std::nullopt;
    return d->immI32;
}

std::optional<double> constF64(const ValueView& values, ValueId v) {
    const il::Instruction* d = values.defIs(v, Op::ConstF64);
    if (d == nullptr || d->type != Type::F64) return std::nullopt;
    return d->immF64;
}

bool isPositiveZero(std::optional<double> c) { return c && *c == 0.0 && !std::signbit(*c); }
bool isNegativeZero(std::optional<double> c) { return c && *c == 0.0 && std::signbit(*c); }

// Two's-complement wrapping, computed in unsigned arithmetic so it is defined.
int32_t wrap(uint32_t v) { return static_cast<int32_t>(v); }
int32_t wrapAdd(int32_t a, int32_t b) { return wrap(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
int32_t wrapSub(int32_t a, int32_t b) { return wrap(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
int32_t wrapMul(int32_t a, int32_t b) { return wrap(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }

// ECMA-262 7.1.6 ToInt32 of a Number: NaN and the infinities are 0; else
// truncate and reduce modulo 2^32 into the signed range.
int32_t jsToInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return wrap(static_cast<uint32_t>(m));
}

FoldOutcome constant() { return FoldOutcome{Kind::Constant, kNoValue}; }
FoldOutcome identity(ValueId v) { return FoldOutcome{Kind::Identity, v}; }
FoldOutcome none() { return {}; }

// add / sub / mul at type i32: wrapping.
FoldOutcome foldI32Arith(il::Instruction& inst, const ValueView& values) {
    const ValueId a = inst.operands[0];
    const ValueId b = inst.operands[1];
    const std::optional<int32_t> ca = constI32(values, a);
    const std::optional<int32_t> cb = constI32(values, b);
    if (ca && cb) {
        const int32_t r = inst.op == Op::Add   ? wrapAdd(*ca, *cb)
                          : inst.op == Op::Sub ? wrapSub(*ca, *cb)
                                               : wrapMul(*ca, *cb);
        makeI32Constant(inst, r);
        return constant();
    }
    switch (inst.op) {
        case Op::Add:
            if (cb == 0) return identity(a);
            if (ca == 0) return identity(b);
            return none();
        case Op::Sub:
            if (cb == 0) return identity(a);
            if (a == b) {
                makeI32Constant(inst, 0);
                return constant();
            }
            return none();
        case Op::Mul:
            if (cb == 1) return identity(a);
            if (ca == 1) return identity(b);
            // An i32 has no -0, so a zero factor is a zero product.
            if (ca == 0 || cb == 0) {
                makeI32Constant(inst, 0);
                return constant();
            }
            return none();
        default:
            return none();
    }
}

// add / sub / mul / div / mod at type f64: IEEE-754, `%` being fmod (whose
// sign follows the dividend, as 6.1.6.1.6 Number::remainder's does).
FoldOutcome foldF64Arith(il::Instruction& inst, const ValueView& values) {
    const ValueId a = inst.operands[0];
    const ValueId b = inst.operands[1];
    const std::optional<double> ca = constF64(values, a);
    const std::optional<double> cb = constF64(values, b);
    if (ca && cb) {
        double r = 0;
        switch (inst.op) {
            case Op::Add: r = *ca + *cb; break;
            case Op::Sub: r = *ca - *cb; break;
            case Op::Mul: r = *ca * *cb; break;
            case Op::Div: r = *ca / *cb; break;
            case Op::Mod: r = std::fmod(*ca, *cb); break;
            default: return none();
        }
        makeF64Constant(inst, r);
        return constant();
    }
    const auto isOne = [](std::optional<double> c) { return c && *c == 1.0; };
    switch (inst.op) {
        case Op::Add:  // x + (-0) is x for every x; x + 0 is not (-0 + 0 = +0)
            if (isNegativeZero(cb)) return identity(a);
            if (isNegativeZero(ca)) return identity(b);
            return none();
        case Op::Sub:  // x - (+0) is x for every x, -0 included
            return isPositiveZero(cb) ? identity(a) : none();
        case Op::Mul:
            if (isOne(cb)) return identity(a);
            if (isOne(ca)) return identity(b);
            return none();
        case Op::Div:
            return isOne(cb) ? identity(a) : none();
        default:
            return none();
    }
}

// The bitwise family: i32 operands, the result the JS Number of the int32
// (or, for `>>>`, the uint32) the operator computes.
FoldOutcome foldBitwise(il::Instruction& inst, const ValueView& values) {
    if (inst.type != Type::F64 && inst.type != Type::I32) return none();
    const std::optional<int32_t> ca = constI32(values, inst.operands[0]);
    const std::optional<int32_t> cb = constI32(values, inst.operands[1]);
    if (!ca || !cb) return none();
    const uint32_t count = static_cast<uint32_t>(*cb) & 31u;
    double r = 0;
    switch (inst.op) {
        case Op::BitAnd: r = *ca & *cb; break;
        case Op::BitOr: r = *ca | *cb; break;
        case Op::BitXor: r = *ca ^ *cb; break;
        case Op::Shl: r = wrap(static_cast<uint32_t>(*ca) << count); break;
        case Op::Shr: r = *ca >> count; break;  // arithmetic (C++20)
        case Op::UShr: r = static_cast<double>(static_cast<uint32_t>(*ca) >> count); break;
        default: return none();
    }
    if (inst.type == Type::F64) {
        makeF64Constant(inst, r);
        return constant();
    }
    if (r > std::numeric_limits<int32_t>::max()) return none();  // a `>>>` past int32
    makeI32Constant(inst, static_cast<int32_t>(r));
    return constant();
}

// cmp.* at a typed operand type. Bool admits only == and !=.
FoldOutcome foldCompare(il::Instruction& inst, const ValueView& values) {
    if (inst.type != Type::Bool) return none();
    const ValueId a = inst.operands[0];
    const ValueId b = inst.operands[1];
    const Type t = values.type(a);
    if (t != values.type(b)) return none();
    const Op op = inst.op;
    auto decide = [&](auto x, auto y) {
        switch (op) {
            case Op::CmpLt: return x < y;
            case Op::CmpGt: return x > y;
            case Op::CmpLe: return x <= y;
            case Op::CmpGe: return x >= y;
            case Op::CmpEq: return x == y;
            default: return x != y;  // CmpNe: unordered on doubles, NaN != NaN
        }
    };
    if (t == Type::I32) {
        const auto ca = constI32(values, a);
        const auto cb = constI32(values, b);
        if (ca && cb) {
            makeBoolConstant(inst, decide(*ca, *cb));
            return constant();
        }
    } else if (t == Type::F64) {
        const auto ca = constF64(values, a);
        const auto cb = constF64(values, b);
        if (ca && cb) {
            makeBoolConstant(inst, decide(*ca, *cb));
            return constant();
        }
    } else if (t == Type::Bool && (op == Op::CmpEq || op == Op::CmpNe)) {
        const auto ca = constBool(values, a);
        const auto cb = constBool(values, b);
        if (ca && cb) {
            makeBoolConstant(inst, decide(*ca, *cb));
            return constant();
        }
        // `x == true` and `x != false` are x.
        const bool keep = op == Op::CmpEq;
        if (cb == keep) return identity(a);
        if (ca == keep) return identity(b);
    } else {
        return none();
    }
    if (a != b) return none();
    // x against itself. On doubles only < and > are decided: NaN is not
    // equal to itself, and so neither <= nor >= nor == holds for it.
    const bool exact = t != Type::F64;
    switch (op) {
        case Op::CmpLt:
        case Op::CmpGt: makeBoolConstant(inst, false); return constant();
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
            if (!exact) return none();
            makeBoolConstant(inst, true);
            return constant();
        case Op::CmpNe:
            if (!exact) return none();
            makeBoolConstant(inst, false);
            return constant();
        default:
            return none();
    }
}

// add/sub/mul.would_ovf: whether the exact result is not an int32 Number
// (for mul, -0 counts: il_op.h).
FoldOutcome foldWouldOverflow(il::Instruction& inst, const ValueView& values) {
    const ValueId a = inst.operands[0];
    const ValueId b = inst.operands[1];
    const auto ca = constI32(values, a);
    const auto cb = constI32(values, b);
    if (ca && cb) {
        const int64_t x = *ca;
        const int64_t y = *cb;
        const int64_t r = inst.op == Op::AddWouldOvf ? x + y : inst.op == Op::SubWouldOvf ? x - y : x * y;
        bool ovf = r < std::numeric_limits<int32_t>::min() || r > std::numeric_limits<int32_t>::max();
        if (inst.op == Op::MulWouldOvf && r == 0 && (x < 0 || y < 0)) ovf = true;
        makeBoolConstant(inst, ovf);
        return constant();
    }
    bool never = false;
    switch (inst.op) {
        case Op::AddWouldOvf: never = ca == 0 || cb == 0; break;
        case Op::SubWouldOvf: never = cb == 0; break;
        case Op::MulWouldOvf: never = ca == 1 || cb == 1; break;
        default: break;
    }
    if (!never) return none();
    makeBoolConstant(inst, false);
    return constant();
}

FoldOutcome foldUnary(il::Instruction& inst, const ValueView& values) {
    const ValueId x = inst.operands[0];
    const il::Instruction* d = values.def(x);
    switch (inst.op) {
        case Op::Neg:
            if (inst.type != Type::F64) return none();
            if (const auto c = constF64(values, x)) {
                makeF64Constant(inst, -*c);
                return constant();
            }
            if (d != nullptr && d->op == Op::Neg && d->type == Type::F64 && d->operands.size() == 1) {
                return identity(d->operands[0]);
            }
            return none();
        case Op::I32ToF64:
            if (const auto c = constI32(values, x)) {
                makeF64Constant(inst, static_cast<double>(*c));
                return constant();
            }
            return none();
        case Op::ToInt32: {
            if (inst.type != Type::I32) return none();
            const Type from = values.type(x);
            if (from == Type::I32) return identity(x);
            if (from == Type::F64) {
                if (const auto c = constF64(values, x)) {
                    makeI32Constant(inst, jsToInt32(*c));
                    return constant();
                }
            } else if (from == Type::Bool) {
                if (const auto c = constBool(values, x)) {
                    makeI32Constant(inst, *c ? 1 : 0);
                    return constant();
                }
            }
            return none();
        }
        case Op::NumTruthy:
            if (const auto c = constF64(values, x)) {
                makeBoolConstant(inst, *c != 0.0 && !std::isnan(*c));
                return constant();
            }
            return none();
        // An unbox of the box of a value of the same type is the value: the
        // checked forms (the conversion is exact on its own type) and the
        // unchecked ones alike.
        case Op::Unbox:
        case Op::UnboxI32:
        case Op::UnboxF64:
            if (d != nullptr && d->op == Op::Box && d->boxType == inst.type && d->operands.size() == 1 &&
                values.type(d->operands[0]) == inst.type) {
                return identity(d->operands[0]);
            }
            // `unbox.bool` is ToBoolean: undefined and null are false.
            if (inst.op == Op::Unbox && inst.type == Type::Bool && d != nullptr &&
                (d->op == Op::ConstUndefined || d->op == Op::ConstNull)) {
                makeBoolConstant(inst, false);
                return constant();
            }
            return none();
        default:
            return none();
    }
}

FoldOutcome foldBinary(il::Instruction& inst, const ValueView& values) {
    switch (inst.op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
            if (inst.type == Type::I32) return foldI32Arith(inst, values);
            if (inst.type == Type::F64) return foldF64Arith(inst, values);
            return none();
        case Op::Div:
        case Op::Mod:
            return inst.type == Type::F64 ? foldF64Arith(inst, values) : none();
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
            return foldBitwise(inst, values);
        case Op::CmpLt:
        case Op::CmpGt:
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
            return foldCompare(inst, values);
        case Op::AddWouldOvf:
        case Op::SubWouldOvf:
        case Op::MulWouldOvf:
            return foldWouldOverflow(inst, values);
        case Op::InBounds: {
            const auto ci = constI32(values, inst.operands[0]);
            const auto cn = constI32(values, inst.operands[1]);
            if (ci && cn) {
                makeBoolConstant(inst, static_cast<uint32_t>(*ci) < static_cast<uint32_t>(*cn));
                return constant();
            }
            if (inst.operands[0] == inst.operands[1]) {  // (u)i < (u)i
                makeBoolConstant(inst, false);
                return constant();
            }
            return none();
        }
        case Op::PtrEq:
            if (inst.operands[0] != inst.operands[1]) return none();
            makeBoolConstant(inst, true);
            return constant();
        case Op::StrictEq: {
            // undefined and null are each equal to themselves and not to
            // each other; nothing else is decided here.
            const il::Instruction* da = values.def(inst.operands[0]);
            const il::Instruction* db = values.def(inst.operands[1]);
            const auto oddball = [](const il::Instruction* d) {
                return d != nullptr && (d->op == Op::ConstUndefined || d->op == Op::ConstNull);
            };
            if (!oddball(da) || !oddball(db)) return none();
            makeBoolConstant(inst, da->op == db->op);
            return constant();
        }
        default:
            return none();
    }
}

void rewriteAsConstant(il::Instruction& inst, Op op, Type type) {
    il::Instruction c;
    c.op = op;
    c.type = type;
    c.result = inst.result;
    c.span = inst.span;
    inst = std::move(c);
}

}  // namespace

std::optional<bool> constBool(const ValueView& values, ValueId v) {
    const il::Instruction* d = values.defIs(v, Op::ConstBool);
    if (d == nullptr) return std::nullopt;
    return d->immI32 != 0;
}

void makeBoolConstant(il::Instruction& inst, bool value) {
    rewriteAsConstant(inst, Op::ConstBool, Type::Bool);
    inst.immI32 = value ? 1 : 0;
}

void makeI32Constant(il::Instruction& inst, int32_t value) {
    rewriteAsConstant(inst, Op::ConstI32, Type::I32);
    inst.immI32 = value;
}

void makeF64Constant(il::Instruction& inst, double value) {
    rewriteAsConstant(inst, Op::ConstF64, Type::F64);
    inst.immF64 = value;
}

FoldOutcome foldInstruction(il::Instruction& inst, const ValueView& values) {
    if (inst.result == kNoValue) return none();
    // A Dynamic arithmetic op is ToPrimitive and user code: never folded.
    if (inst.type == Type::Dynamic) return none();
    FoldOutcome out;
    if (inst.operands.size() == 1) {
        out = foldUnary(inst, values);
    } else if (inst.operands.size() == 2) {
        out = foldBinary(inst, values);
    }
    // An identity must not change a value's type.
    if (out.kind == Kind::Identity && values.type(out.value) != inst.type) return none();
    return out;
}

}  // namespace bronze::opt::redundancy
