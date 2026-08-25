// THE WRITE BARRIERS FOR `--pins` (src/types/pins.h, stage B1).
//
// Everything in this file exists to make one sentence true: a program that
// contradicts a pin gets a TypeError naming the manifest line, and a program
// that keeps its promises pays nothing.
//
// The second half is what decides the shape of the code. A barrier is emitted
// only where lowering has NO static answer, so the callers below all hand over
// the value BEFORE it is boxed — an `il::Type::F64` is a Number by
// construction, and that one test removes the barrier from every arithmetic
// store, every pinned read fed back into a pinned slot, every typed parameter
// and every typed call result. On this campaign's four pinned kernels it
// removes all of them.
//
// The first half is where the barrier goes: at the STORE, the enumerated CALL
// SITE, and the boxed WRAPPER, never at the read. A pinned read spends its
// claim unconditionally and that is the whole performance model; a guard there
// would be the deoptimization the manifest was written to remove.

#include "lower/lowerer.h"

#include "types/result.h"

namespace bronze::lower {

bool Lowerer::pinSatisfiedStatically(Value val, il::PinBarrier kind) {
    switch (kind) {
        // An f64 IL value IS a Number: there is no other inhabitant of the
        // type, and the box on the way to the slot is the canonicalizing one.
        case il::PinBarrier::Number:
        case il::PinBarrier::NumberOrNullish:
            return val.type == il::Type::F64 || val.type == il::Type::I32;
        // Nothing in the IL types an array, so this claim never has a static
        // answer and the store always asks.
        case il::PinBarrier::DenseArray:
            return false;
    }
    return false;
}

void Lowerer::emitPinGuard(Value val, const std::string& pinText, il::PinBarrier kind,
                           il::Function& ilFn) {
    if (!types::pinBarriersEnabled()) return;
    if (pinSatisfiedStatically(val, kind)) return;
    // The barrier tests the BOXED form, because that is the one word the
    // shape questions are asked of — and because it is the word the store
    // about to follow will write, so the two cannot disagree about which value
    // was checked.
    Value boxed = boxValueIfNeeded(val, ilFn);
    il::Instruction inst;
    inst.op = il::Op::PinGuard;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {boxed.id};
    inst.keyIndex = getKeyConstantIndex(pinText);
    inst.immI32 = static_cast<int32_t>(kind);
    emitInst(ilFn, inst);
}

namespace {

const char* pinKindWord(types::PinKind kind) {
    switch (kind) {
        case types::PinKind::Number: return "number";
        case types::PinKind::NumericElements: return "numeric-elements";
        case types::PinKind::NumberOrNullish: return "number-or-nullish";
    }
    return "number";
}

// A cyclic `extends` chain is a TypeError at run time and must not be an
// infinite loop here — the same bound `FlowAnalyzer::pinnedField` uses, for
// the same reason.
constexpr uint32_t kMaxExtendsHops = 64;

}  // namespace

const types::PinKind* Lowerer::pinnedFieldAt(const ast::Expr& receiver, const std::string& key,
                                             std::string* pinTextOut) const {
    if (pins_ == nullptr || inference_ == nullptr) return nullptr;
    const types::Type recv = inferredType(receiver);
    if (!recv.is(types::TypeKind::Object) || recv.shapeClass() == types::kNoShapeClass) {
        return nullptr;
    }
    // Resolved exactly the way the READ resolves it (types/flow_expr.cpp
    // `pinnedField`): same shape class, same fallback to the interned
    // constructor name for a shape with no class layout, same walk up
    // `extends`. Two different answers here and there would be a barrier
    // holding a promise nobody spent, or worse, a promise spent with no
    // barrier — so this is a duplicate of that rule on purpose, and the tests
    // pin the pair.
    const types::ClassLayout* layout = inference_->classLayouts.byShapeClass(recv.shapeClass());
    std::string name = layout != nullptr ? layout->name
                                         : inference_->shapes.at(recv.shapeClass()).constructorName;
    if (name.empty()) return nullptr;
    for (uint32_t hop = 0; hop < kMaxExtendsHops; ++hop) {
        if (const types::PinKind* pin = pins_->lookup(name, key)) {
            if (pinTextOut != nullptr) {
                // The LAST dotted component, because the manifest matches on
                // it and spells it that way — the linker's `mod3.` prefix is
                // not in the file, and the message exists to be grepped for
                // in the file.
                const auto dot = name.rfind('.');
                *pinTextOut = (dot == std::string::npos ? name : name.substr(dot + 1)) + "." +
                              key + ": " + pinKindWord(*pin);
            }
            return pin;
        }
        if (layout == nullptr || layout->superName.empty()) return nullptr;
        layout = inference_->classLayouts.byName(layout->superName);
        if (layout == nullptr) return nullptr;
        name = layout->name;
    }
    return nullptr;
}

void Lowerer::emitPinnedElementBarrier(uint32_t elemKind, Value val, il::Function& ilFn) {
    if (elemKind != static_cast<uint32_t>(il::kElemKindPlainArrayF64)) return;
    // The NUMERIC half of `numeric-elements`, held one element at a time.
    //
    // The pin text names the kind rather than a (class, field) pair, and that
    // is not laziness: the claim rides on the receiver's TYPE, so `const te =
    // this.elements; te[0] = x` is a store through a local that no longer
    // remembers which field it came from. The kind is what a reader greps the
    // manifest for, and every entry of that kind is a candidate.
    emitPinGuard(val, "<numeric-elements element>: number", il::PinBarrier::Number, ilFn);
}

void Lowerer::emitPinFieldBarrier(const ast::Expr& receiver, const std::string& key, Value val,
                                  il::Function& ilFn) {
    if (!types::pinBarriersEnabled()) return;
    std::string pinText;
    const types::PinKind* pin = pinnedFieldAt(receiver, key, &pinText);
    if (pin == nullptr) return;
    switch (*pin) {
        case types::PinKind::Number:
            emitPinGuard(val, pinText, il::PinBarrier::Number, ilFn);
            return;
        case types::PinKind::NumberOrNullish:
            emitPinGuard(val, pinText, il::PinBarrier::NumberOrNullish, ilFn);
            return;
        // The FIELD half of the element claim. What it holds is "a plain dense
        // JS Array", which is what the raw element form walks; the numeric
        // half is held at each element store, where it is one compare instead
        // of an O(n) sweep. types/pins.h states the residue that leaves.
        case types::PinKind::NumericElements:
            emitPinGuard(val, pinText, il::PinBarrier::DenseArray, ilFn);
            return;
    }
}

}  // namespace bronze::lower
