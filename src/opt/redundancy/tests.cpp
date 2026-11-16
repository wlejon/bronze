#include "opt/redundancy/tests.h"

namespace bronze::opt::redundancy {

namespace {

using il::Op;
using il::Type;

// What a value's definition alone says about its tag.
enum class DefTag : uint8_t {
    Unknown,
    Object,     // a fresh object, array or closure
    Nullish,    // the undefined or null constant
    Int32,      // the box of an i32: a Number holding an int32
    Number,     // the box of an f64: a Number, in whichever box
    Primitive,  // the box of a bool or a string
};

DefTag tagByDefinition(const ValueView& values, ValueId v) {
    const il::Instruction* d = values.def(v);
    if (d == nullptr) return DefTag::Unknown;
    switch (d->op) {
        case Op::CreateObject:
        case Op::CreateArray:
        case Op::CreateFunction:
            return DefTag::Object;
        case Op::ConstUndefined:
        case Op::ConstNull:
            return DefTag::Nullish;
        case Op::Box:
            switch (d->boxType) {
                case Type::I32: return DefTag::Int32;
                case Type::F64: return DefTag::Number;
                case Type::Bool:
                case Type::Str: return DefTag::Primitive;
                default: return DefTag::Unknown;
            }
        default:
            return DefTag::Unknown;
    }
}

// The subject is certainly not a heap object.
bool knownNotObject(const FactEnv& facts, DefTag def, ValueId x) {
    if (def != DefTag::Unknown && def != DefTag::Object) return true;
    if (facts.known(FactKind::Object, x) == false) return true;
    const ValueTag tag = facts.tagOf(x);
    if (tag == ValueTag::I32 || tag == ValueTag::Number || tag == ValueTag::Nullish) return true;
    return facts.known(FactKind::Hole, x) == true;
}

bool knownObject(const FactEnv& facts, DefTag def, ValueId x) {
    return def == DefTag::Object || facts.isObject(x);
}

// The answer of `fact` when the facts hold one, else of `fallback`.
std::optional<bool> either(std::optional<bool> fact, std::optional<bool> fallback) {
    return fact ? fact : fallback;
}

std::optional<bool> isObjectTest(const FactEnv& facts, DefTag def, ValueId x) {
    if (knownObject(facts, def, x)) return true;
    if (knownNotObject(facts, def, x)) return false;
    return std::nullopt;
}

std::optional<bool> isI32Test(const FactEnv& facts, DefTag def, ValueId x) {
    if (const auto k = facts.known(FactKind::I32, x)) return k;
    if (def == DefTag::Int32) return true;
    if (def == DefTag::Primitive || def == DefTag::Nullish) return false;
    if (knownObject(facts, def, x) || facts.known(FactKind::Nullish, x) == true ||
        facts.known(FactKind::Hole, x) == true) {
        return false;
    }
    return std::nullopt;
}

// `is.number` is the DOUBLE box only (il_op.h), so neither an I32 fact nor
// the box of an i32 decides it.
std::optional<bool> isNumberTest(const FactEnv& facts, DefTag def, ValueId x) {
    if (const auto k = facts.known(FactKind::Number, x)) return k;
    if (def == DefTag::Primitive || def == DefTag::Nullish) return false;
    if (knownObject(facts, def, x) || facts.known(FactKind::Nullish, x) == true ||
        facts.known(FactKind::Hole, x) == true) {
        return false;
    }
    return std::nullopt;
}

std::optional<bool> isNullishTest(const FactEnv& facts, DefTag def, ValueId x) {
    if (const auto k = facts.known(FactKind::Nullish, x)) return k;
    if (def == DefTag::Nullish) return true;
    if (def != DefTag::Unknown) return false;
    if (knownObject(facts, def, x) || facts.isI32(x) || facts.isNumber(x) ||
        facts.known(FactKind::Hole, x) == true) {
        return false;
    }
    return std::nullopt;
}

// Only `elem.load` produces the hole, so any value with a known tag or a
// defining constant, box or allocation is not it.
std::optional<bool> isHoleTest(const FactEnv& facts, DefTag def, ValueId x) {
    if (const auto k = facts.known(FactKind::Hole, x)) return k;
    if (def != DefTag::Unknown) return false;
    if (facts.tagOf(x) != ValueTag::Unknown) return false;
    return std::nullopt;
}

std::optional<bool> fnCodeTest(const FactEnv& facts, const ValueView& values, DefTag def,
                               const il::Instruction& inst) {
    const ValueId x = inst.operands[0];
    if (const auto k = facts.known(FactKind::Callee, x, inst.calleeIndex)) return k;
    // A closure made here runs the code it was made from.
    if (const il::Instruction* d = values.defIs(x, Op::CreateFunction)) return d->calleeIndex == inst.calleeIndex;
    if (knownNotObject(facts, def, x)) return false;
    return std::nullopt;
}

// `ptr.eq (load.shape x), (shape.cell n)` either way round, answered by the
// shape fact about x when the load still reads the present shape.
std::optional<bool> ptrEqTest(const il::Instruction& inst, const FactEnv& facts, const ExprTable& exprs,
                              const ValueView& values) {
    const ValueId a = inst.operands[0];
    const ValueId b = inst.operands[1];
    for (int flip = 0; flip < 2; ++flip) {
        const ValueId loadId = flip ? b : a;
        const ValueId cellId = flip ? a : b;
        const il::Instruction* load = values.defIs(loadId, Op::LoadShape);
        const il::Instruction* cell = values.defIs(cellId, Op::ShapeCell);
        if (load == nullptr || cell == nullptr || load->operands.size() != 1) continue;
        // The table entry of the load dies at any write of Shapes, so finding
        // the load itself there means the shape it read is the shape now.
        if (exprs.lookup(exprKeyOf(*load)).value != loadId) return std::nullopt;
        return facts.known(FactKind::Shape, load->operands[0], cell->immI32);
    }
    return facts.known(FactKind::PtrEq, a, b);
}

}  // namespace

std::optional<bool> knownTestResult(const il::Instruction& inst, const FactEnv& facts,
                                    const ExprTable& exprs, const ValueView& values) {
    if (inst.result == kNoValue || inst.type != Type::Bool || inst.operands.empty()) return std::nullopt;
    const ValueId x = inst.operands[0];
    const DefTag def = tagByDefinition(values, x);
    switch (inst.op) {
        case Op::IsObject:
            return isObjectTest(facts, def, x);
        case Op::IsI32:
            return isI32Test(facts, def, x);
        case Op::IsNumber:
            return isNumberTest(facts, def, x);
        case Op::IsNullish:
            return isNullishTest(facts, def, x);
        case Op::IsHole:
            return isHoleTest(facts, def, x);
        case Op::IsDenseArray:
            return either(facts.known(FactKind::DenseArray, x, inst.immI32),
                          knownNotObject(facts, def, x) ? std::optional<bool>(false) : std::nullopt);
        case Op::IsPlainArray:
            return either(facts.known(FactKind::PlainArray, x),
                          knownNotObject(facts, def, x) ? std::optional<bool>(false) : std::nullopt);
        case Op::IsTypedArray:
            return either(facts.known(FactKind::TypedArray, x, inst.immI32),
                          knownNotObject(facts, def, x) ? std::optional<bool>(false) : std::nullopt);
        case Op::IsArray:
            if (facts.isArray(x)) return true;
            return either(facts.known(FactKind::Array, x),
                          knownNotObject(facts, def, x) ? std::optional<bool>(false) : std::nullopt);
        case Op::FnCodeIs:
            return fnCodeTest(facts, values, def, inst);
        case Op::InBounds:
            if (inst.operands.size() != 2) return std::nullopt;
            return facts.known(FactKind::InBounds, inst.operands[0], 0, inst.operands[1]);
        case Op::PtrEq:
            if (inst.operands.size() != 2) return std::nullopt;
            return ptrEqTest(inst, facts, exprs, values);
        default:
            return std::nullopt;
    }
}

}  // namespace bronze::opt::redundancy
