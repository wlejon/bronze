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

bool Lowerer::emitPinGuard(Value val, const std::string& pinText, il::PinBarrier kind,
                           il::Function& ilFn) {
    if (pinSatisfiedStatically(val, kind)) return false;
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
    return true;
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

}  // namespace

// An IL function's name as the MANIFEST spells it: the module linker's
// `modN.` prefix dropped, everything else kept. The pin's message exists to be
// grepped for in the manifest, so it has to read back as a line of the file.
std::string Lowerer::manifestOwnerName(const std::string& ilName) {
    const auto dot = ilName.find('.');
    if (dot == std::string::npos || dot < 4 || ilName.compare(0, 3, "mod") != 0) return ilName;
    for (size_t i = 3; i < dot; ++i) {
        if (ilName[i] < '0' || ilName[i] > '9') return ilName;
    }
    return ilName.substr(dot + 1);
}

// Give one function's IL skeleton the parameter and return types a `--pins`
// SIGNATURE entry declares for it (types/pins.h, the `param` and `return`
// forms). Returns false only after diagnosing. It only ever moves a slot from
// `Dynamic` to `F64`.
//
// A refused POSITION is a hard error rather than a silent skip: a `param` entry
// names one function and one parameter of it, so quietly compiling the boxed
// version would hide the fact that the manifest is wrong.
bool Lowerer::applySignaturePins(const std::vector<ast::Param>& params, Span span,
                                 il::Function& fn) {
    if (pins_ == nullptr) return true;

    const size_t base = fn.firstSourceParam();
    for (size_t i = 0; i < params.size() && i + base < fn.params.size(); ++i) {
        const ast::Param& p = params[i];
        if (p.name.empty() || !pins_->paramPinned(fn.name, p.name)) continue;
        // The three positions the language may fill with a value the caller
        // never wrote, and there is no `undefined` in an f64.
        const char* why = p.isRest       ? "a rest parameter is an array, not a number"
                          : p.pattern    ? "a destructuring pattern binds no single slot"
                          : p.defaultValue ? "a parameter with a default is `undefined` on a "
                                             "short call, which no f64 slot can hold"
                                           : nullptr;
        if (why != nullptr) {
            diags_.error(span, "pin 'param " + fn.name + "(" + p.name +
                                   "): number' cannot be honoured: " + std::string(why));
            return false;
        }
        fn.params[i + base].type = il::Type::F64;
        // The barrier's licence: this slot is f64 because a manifest said so.
        fn.params[i + base].pinned = true;
        fn.params[i + base].pinKeyIndex = getKeyConstantIndex(
            "param " + Lowerer::manifestOwnerName(fn.name) + "(" + p.name + "): number");
    }

    if (pins_->returnPinned(fn.name)) {
        // A generator's factory returns the GENERATOR OBJECT and an async
        // function's the PROMISE, whatever the body returns — so a return pin
        // on either is a claim about the wrong value.
        if ((fn.fnFlags & (BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC)) != 0) {
            diags_.error(span, "pin 'return " + fn.name +
                                   ": number' cannot be honoured: this function returns a "
                                   "generator object or a promise, not its body's value");
            return false;
        }
        if (fn.returnType == il::Type::Dynamic) {
            fn.returnType = il::Type::F64;
            fn.returnPinned = true;
            fn.returnPinKeyIndex =
                getKeyConstantIndex("return " + Lowerer::manifestOwnerName(fn.name) + ": number");
        } else if (fn.returnType != il::Type::F64) {
            diags_.error(span, "pin 'return " + fn.name +
                                   ": number' cannot be honoured: the convention already types "
                                   "this return " +
                                   il::typeName(fn.returnType));
            return false;
        }
    }
    return true;
}

// The pin a store of `key` is held to. Only the key is known about the
// receiver, so the manifest is asked by field name.
const types::PinKind* Lowerer::pinnedFieldAt(const ast::Expr& /*receiver*/, const std::string& key,
                                             std::string* pinTextOut) const {
    if (pins_ == nullptr) return nullptr;
    std::string matchedClass;
    if (const types::PinKind* pin = pins_->lookupField(key, &matchedClass)) {
        if (pinTextOut != nullptr) {
            *pinTextOut = matchedClass + "." + key + ": " + pinKindWord(*pin);
        }
        return pin;
    }
    return nullptr;
}

void Lowerer::emitPinFieldBarrier(const ast::Expr& receiver, const std::string& key, Value val,
                                  il::Function& ilFn) {
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
