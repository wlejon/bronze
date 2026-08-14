#include "runtime/builtin_array_internal.h"

namespace bronze::runtime {

uint64_t arrayIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "indexOf")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    Value needle = args[0];
    uint32_t from =
        args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), arr->length) : 0;
    for (uint32_t i = from; i < arr->length; ++i) {
        if (!arr->hasElem(i)) continue;
        if (bronze_strict_eq(arr->getElem(i).rawBits(), needle.rawBits())) {
            return Value::fromDouble(i).rawBits();
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t arrayLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "lastIndexOf")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    const uint32_t len = arr->length;
    if (len == 0) return Value::fromDouble(-1.0).rawBits();

    double fromNum = args.count() > 1 && !args[1].isUndefined()
                         ? toInteger(rtToNumber(args[1]))
                         : static_cast<double>(len - 1);
    double k = fromNum >= 0 ? std::min(fromNum, static_cast<double>(len - 1))
                            : static_cast<double>(len) + fromNum;
    if (k < 0) return Value::fromDouble(-1.0).rawBits();

    Value needle = args[0];
    for (int64_t i = static_cast<int64_t>(k); i >= 0; --i) {
        if (!arr->hasElem(static_cast<uint32_t>(i))) continue;
        if (bronze_strict_eq(arr->getElem(static_cast<uint32_t>(i)).rawBits(), needle.rawBits())) {
            return Value::fromDouble(static_cast<double>(i)).rawBits();
        }
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t arrayIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "includes")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    const uint32_t len = arr->length;
    uint32_t from = args.count() > 1 ? relativeIndex(toInteger(rtToNumber(args[1])), len) : 0;
    for (uint32_t i = from; i < len; ++i) {
        if (sameValueZero(arr->getElem(i), args[0])) return Value::fromBool(true).rawBits();
    }
    return Value::fromBool(false).rawBits();
}

uint64_t arrayAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self(thisBits);
    if (!requireArray(self, "at")) return Value::fromUndefined().rawBits();
    ArrayHeader* arr = self.asObject<ArrayHeader>();
    double rel = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    double idx = rel < 0 ? static_cast<double>(arr->length) + rel : rel;
    if (idx < 0 || idx >= static_cast<double>(arr->length)) {
        return Value::fromUndefined().rawBits();
    }
    return arr->getElem(static_cast<uint32_t>(idx)).rawBits();
}

template <bool Reverse, bool WantIndex>
static uint64_t arrayFindImpl(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "find")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};
    const uint32_t len = lengthOf(self.get());
    for (uint32_t n = 0; n < len; ++n) {
        uint32_t i = Reverse ? len - 1 - n : n;
        Rooted<Value> elem{elemOf(self.get(), i)};
        Rooted<Value> found{callBack(fn, thisArg, elem, i, self)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(found.get().rawBits())) {
            return WantIndex ? Value::fromDouble(i).rawBits() : elem.get().rawBits();
        }
    }
    return WantIndex ? Value::fromDouble(-1.0).rawBits() : Value::fromUndefined().rawBits();
}

uint64_t arrayFind(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayFindImpl<false, false>(env, thisBits, argc, argv);
}

uint64_t arrayFindIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayFindImpl<false, true>(env, thisBits, argc, argv);
}

uint64_t arrayFindLast(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayFindImpl<true, false>(env, thisBits, argc, argv);
}

uint64_t arrayFindLastIndex(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return arrayFindImpl<true, true>(env, thisBits, argc, argv);
}

}  // namespace bronze::runtime
