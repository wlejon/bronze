#include "opt/redundancy/tests.h"

#include "opt/core/shape_key.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

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
        case Op::ObjectLiteral:
        case Op::ArrayLiteral:
        case Op::CreateThis:
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
        // Under `is.dense_array` with kDenseNumbers: a Number in double form.
        case Op::ElemLoad:
            return (d->immI32 & il::kElemLoadNumber) != 0 ? DefTag::Number : DefTag::Unknown;
        // A double slot's read (il_op.h SlotLoad, `number`).
        case Op::SlotLoad:
            return d->boxType == Type::F64 ? DefTag::Number : DefTag::Unknown;
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
    if (def == DefTag::Number) return true;
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

// The cells shape word `loadId` (the `load.shape` `load`) may be: what the
// facts say of the word, met with what they say of the object when the load
// still reads the present shape (the table entry of the load dies at any
// write of Shapes, so finding the load itself there means the shape it read
// is the shape now).
std::optional<std::vector<uint32_t>> shapeSetOfLoad(const il::Instruction& load, ValueId loadId,
                                                   const FactEnv& facts, const ExprTable& exprs) {
    std::optional<std::vector<uint32_t>> word = facts.shapeSet(loadId);
    if (exprs.lookup(exprKeyOf(load)).value != loadId) return word;
    std::optional<std::vector<uint32_t>> object = facts.shapeSet(load.operands[0]);
    if (!word) return object;
    if (!object) return word;
    std::vector<uint32_t> both;
    std::set_intersection(word->begin(), word->end(), object->begin(), object->end(), std::back_inserter(both));
    return both;
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
        const std::optional<std::vector<uint32_t>> set = shapeSetOfLoad(*load, loadId, facts, exprs);
        if (!set) return facts.known(FactKind::Shape, loadId, cell->immI32);
        const auto c = static_cast<uint32_t>(cell->immI32);
        if (std::find(set->begin(), set->end(), c) == set->end()) return false;
        if (set->size() == 1) return true;
        return facts.known(FactKind::Shape, loadId, cell->immI32);
    }
    return facts.known(FactKind::PtrEq, a, b);
}

// `shape.in (load.shape x), (shape.cell n1), ...`: true when every shape
// the word may be is among the cells, false when none is.
std::optional<bool> shapeInTest(const il::Instruction& inst, const FactEnv& facts, const ExprTable& exprs,
                                const ValueView& values) {
    const il::Instruction* load = values.defIs(inst.operands[0], Op::LoadShape);
    if (load == nullptr || load->operands.size() != 1) return std::nullopt;
    std::vector<uint32_t> cells;
    for (size_t i = 1; i < inst.operands.size(); ++i) {
        const il::Instruction* cell = values.defIs(inst.operands[i], Op::ShapeCell);
        if (cell == nullptr) return std::nullopt;
        cells.push_back(static_cast<uint32_t>(cell->immI32));
    }
    bool none = true;
    for (uint32_t c : cells) {
        if (facts.known(FactKind::Shape, inst.operands[0], c) != false) none = false;
    }
    if (none) return false;
    const std::optional<std::vector<uint32_t>> set = shapeSetOfLoad(*load, inst.operands[0], facts, exprs);
    if (!set) return std::nullopt;
    bool all = true;
    bool any = false;
    for (uint32_t c : *set) {
        const bool in = std::find(cells.begin(), cells.end(), c) != cells.end();
        all = all && in;
        any = any || in;
    }
    if (all) return true;
    if (!any) return false;
    return std::nullopt;
}

std::pair<ValueId, int32_t> offsetOf(const ValueView& values, ValueId v) {
    return offsetForm(v, [&](ValueId x) { return values.def(x); });
}

// `in.bounds x, n, +span`: a test of the same x and n answers a plain one; an
// InRange fact about x's base answers any whose offsets it covers.
std::optional<bool> inBoundsTest(const il::Instruction& inst, const FactEnv& facts, const ValueView& values) {
    const ValueId x = inst.operands[0];
    const ValueId n = inst.operands[1];
    if (inst.immI32 == 0) {
        if (const auto k = facts.known(FactKind::InBounds, x, 0, n)) return k;
    }
    const auto [base, c] = offsetOf(values, x);
    if (static_cast<int64_t>(c) + inst.immI32 > INT32_MAX) return std::nullopt;
    if (facts.inRange(base, n, c, c + inst.immI32)) return true;
    return std::nullopt;
}

// `add.would_ovf x, k` with k a non-negative constant: an InRange fact
// putting x + lo .. x + hi in [0, n), n an int32 length, with lo <= k <=
// hi + 1 says the sum is an int32 (FactEnv::sumFits). The hi + 1 is the
// increment of a loop index under its i < n (index_facts.h).
std::optional<bool> addOverflowTest(const il::Instruction& inst, const FactEnv& facts, const ValueView& values) {
    for (int side = 0; side < 2; ++side) {
        const il::Instruction* k = values.defIs(inst.operands[side], Op::ConstI32);
        if (k == nullptr || k->immI32 < 0) continue;
        if (facts.sumFits(inst.operands[1 - side], k->immI32)) return false;
    }
    return std::nullopt;
}

}  // namespace

std::optional<bool> knownHasOwnResult(const il::Instruction& inst, const FactEnv& facts, const ExprTable& exprs,
                                      const ValueView& values, const il::Module& module) {
    if (inst.op != Op::ShapeHasOwn || inst.operands.size() != 1) return std::nullopt;
    const il::Instruction* load = values.defIs(inst.operands[0], Op::LoadShape);
    if (load == nullptr || load->operands.size() != 1) return std::nullopt;
    const std::optional<std::vector<uint32_t>> cells = shapeSetOfLoad(*load, inst.operands[0], facts, exprs);
    if (!cells || cells->empty() || inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    const std::string& key = module.keyConstants[inst.keyIndex];
    // Every shape the word may be answers alike, or the test stays.
    std::optional<bool> answer;
    for (uint32_t cell : *cells) {
        if (cell >= module.shapeCells.size()) return std::nullopt;
        const std::optional<ShapeName> shape = parseShapeName(module.shapeCells[cell].key);
        if (!shape) return std::nullopt;
        const bool write = (inst.immI32 & il::kHasOwnWrite) != 0;
        const bool number = (inst.immI32 & il::kHasOwnNumber) != 0;
        const std::optional<int32_t> slot = write ? writableSlot(*shape, key) : ownSlot(*shape, key);
        // The representation the mode asks for (il_op.h ShapeHasOwn).
        const bool repr = number == write || numberSlot(*shape, key) == number;
        const bool here = slot && *slot == (inst.immI32 >> il::kHasOwnSlotShift) && repr;
        if (answer && *answer != here) return std::nullopt;
        answer = here;
    }
    return answer;
}

ValueId knownConstructResult(const il::Instruction& inst, const FactEnv& facts, const ValueView& values) {
    if (inst.op != Op::ConstructResult || inst.operands.size() != 2) return kNoValue;
    const ValueId r = inst.operands[0];
    const std::optional<bool> object = isObjectTest(facts, tagByDefinition(values, r), r);
    if (!object) return kNoValue;
    return *object ? r : inst.operands[1];
}

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
        // Only an env read produces the marker: a value with a known tag or
        // a defining constant, box or allocation is not it.
        case Op::IsUninit:
            if (def != DefTag::Unknown || facts.tagOf(x) != ValueTag::Unknown) return false;
            return std::nullopt;
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
            return inBoundsTest(inst, facts, values);
        case Op::AddWouldOvf:
            if (inst.operands.size() != 2) return std::nullopt;
            return addOverflowTest(inst, facts, values);
        case Op::PtrEq:
            if (inst.operands.size() != 2) return std::nullopt;
            return ptrEqTest(inst, facts, exprs, values);
        case Op::ShapeIn:
            return shapeInTest(inst, facts, exprs, values);
        default:
            return std::nullopt;
    }
}

bool knownNumber(const FactEnv& facts, const ValueView& values, ValueId x) {
    const DefTag def = tagByDefinition(values, x);
    if (def == DefTag::Number || def == DefTag::Int32) return true;
    const ValueTag tag = facts.tagOf(x);
    return tag == ValueTag::Number || tag == ValueTag::I32;
}

}  // namespace bronze::opt::redundancy
