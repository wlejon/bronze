#include "runtime/builtin_array_internal.h"

namespace bronze::runtime {

namespace {

void flattenIntoArray(Rooted<Value>& out, Rooted<Value>& source, uint32_t sourceLen,
                      double depth, Rooted<Value>& mapperFn, Rooted<Value>& thisArg,
                      Rooted<Value>& originalThis) {
    for (uint32_t k = 0; k < sourceLen; ++k) {
        if (!hasIndex(source.get(), k)) continue;
        Rooted<Value> elem{elemOf(source.get(), k)};
        if (!mapperFn.get().isUndefined()) {
            elem.set(callBack(mapperFn, thisArg, elem, k, originalThis));
            if (rtExceptionPending()) return;
        }
        bool shouldFlatten = false;
        if (depth > 0.0 && rtIsConcatSpreadable(elem)) {
            shouldFlatten = true;
        }
        if (shouldFlatten) {
            const uint32_t elemLen = lengthOf(elem.get());
            Rooted<Value> noMapper{Value::fromUndefined()};
            flattenIntoArray(out, elem, elemLen, depth - 1.0, noMapper, thisArg, originalThis);
            if (rtExceptionPending()) return;
        } else {
            appendTo(out, elem);
        }
    }
}

}  // namespace

uint64_t arraySlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "slice")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    uint32_t start = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t end = args.count() > 1 && !args[1].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[1])), len)
                       : len;
    const uint32_t count = end > start ? end - start : 0;
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t srcIdx = start + i;
        if (!hasIndex(self.get(), srcIdx)) {
            appendHole(out);
            continue;
        }
        Rooted<Value> elem{elemOf(self.get(), srcIdx)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayConcat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "concat")) return Value::fromUndefined().rawBits();
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};

    auto appendItem = [&](Rooted<Value>& item) {
        if (rtIsConcatSpreadable(item)) {
            if (isArray(item.get())) {
                const uint32_t itemLen = lengthOf(item.get());
                for (uint32_t i = 0; i < itemLen; ++i) {
                    if (!hasIndex(item.get(), i)) {
                        appendHole(out);
                        continue;
                    }
                    Rooted<Value> elem{elemOf(item.get(), i)};
                    appendTo(out, elem);
                }
            } else {
                Rooted<Value> lenKey{rtMakeString("length")};
                Value lenVal(bronze_elem_get(item.get().rawBits(), lenKey.get().rawBits()));
                uint32_t itemLen = 0;
                if (lenVal.isNumber() || lenVal.isInt32()) {
                    itemLen = relativeIndex(toInteger(lenVal.asNumber()), 0xFFFFFFFFu);
                }
                for (uint32_t i = 0; i < itemLen; ++i) {
                    Rooted<Value> idxKey{Value::fromDouble(i)};
                    if (!bronze_has_property(idxKey.get().rawBits(), item.get().rawBits())) {
                        appendHole(out);
                        continue;
                    }
                    Rooted<Value> elem{
                        Value(bronze_elem_get(item.get().rawBits(), idxKey.get().rawBits()))};
                    appendTo(out, elem);
                }
            }
        } else {
            appendTo(out, item);
        }
    };

    appendItem(self);
    for (uint32_t a = 0; a < args.count(); ++a) {
        Rooted<Value> item{args[a]};
        appendItem(item);
    }
    return out.get().rawBits();
}

uint64_t arrayJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "join")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    if (len == 0) return rtMakeString("").rawBits();

    std::string sep = ",";
    if (args.count() > 0 && !args[0].isUndefined()) {
        Rooted<Value> sepStr{rtValueToString(args[0])};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        sep = rtUtf8Chars(sepStr.get().asString<StringHeader>());
    }

    std::string result;
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) result += sep;
        if (!hasIndex(self.get(), i)) continue;
        Rooted<Value> elem{elemOf(self.get(), i)};
        if (elem.get().isNull() || elem.get().isUndefined()) continue;
        if (isArray(elem.get())) {
            Rooted<Value> joinKey{rtMakeString("join")};
            Value joinMethod(bronze_elem_get(elem.get().rawBits(), joinKey.get().rawBits()));
            if (isCallable(joinMethod)) {
                uint64_t res = bronze_dynamic_call(joinMethod.rawBits(), elem.get().rawBits(), 0, nullptr);
                if (rtExceptionPending()) return Value::fromUndefined().rawBits();
                result += rtUtf8Chars(Value(res).asString<StringHeader>());
                continue;
            }
        }
        Rooted<Value> s{rtValueToString(elem.get())};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        result += rtUtf8Chars(s.get().asString<StringHeader>());
    }
    return rtMakeString(result).rawBits();
}

uint64_t arrayFlat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "flat")) return Value::fromUndefined().rawBits();
    double depth = 1.0;
    if (args.count() > 0 && !args[0].isUndefined()) {
        depth = toInteger(rtToNumber(args[0]));
        if (depth < 0.0) depth = 0.0;
    }
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    Rooted<Value> noMapper{Value::fromUndefined()};
    Rooted<Value> thisArg{Value::fromUndefined()};
    flattenIntoArray(out, self, lengthOf(self.get()), depth, noMapper, thisArg, self);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return out.get().rawBits();
}

uint64_t arrayFlatMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "flatMap")) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "flatMap")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    flattenIntoArray(out, self, lengthOf(self.get()), 1.0, fn, thisArg, self);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return out.get().rawBits();
}

uint64_t arrayToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "toSorted")) return Value::fromUndefined().rawBits();
    Rooted<Value> compareFn{args[0]};
    if (!compareFn.get().isUndefined() && !isCallable(compareFn.get())) {
        return rtThrowTypeError("The comparison function must be either a function or undefined")
            .rawBits();
    }
    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{elemOf(self.get(), i)};
        appendTo(out, elem);
    }
    uint64_t sortArg = compareFn.get().rawBits();
    rtArraySortBuiltin(env, out.get().rawBits(), 1, &sortArg);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return out.get().rawBits();
}

uint64_t arrayToReversed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "toReversed")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{elemOf(self.get(), len - 1 - i)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayToSpliced(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "toSpliced")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    const uint32_t start =
        args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t deleteCount = 0;
    if (args.count() == 1) {
        deleteCount = len - start;
    } else if (args.count() > 1) {
        double dc = toInteger(rtToNumber(args[1]));
        if (dc < 0) dc = 0;
        const double most = static_cast<double>(len - start);
        deleteCount = static_cast<uint32_t>(dc < most ? dc : most);
    }
    const uint32_t insertCount = args.count() > 2 ? args.count() - 2 : 0;

    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < start; ++i) {
        Rooted<Value> elem{elemOf(self.get(), i)};
        appendTo(out, elem);
    }
    for (uint32_t i = 0; i < insertCount; ++i) {
        Rooted<Value> elem{args[i + 2]};
        appendTo(out, elem);
    }
    for (uint32_t i = start + deleteCount; i < len; ++i) {
        Rooted<Value> elem{elemOf(self.get(), i)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireArray(self.get(), "with")) return Value::fromUndefined().rawBits();
    const uint32_t len = lengthOf(self.get());
    double rel = toInteger(rtToNumber(args[0]));
    double k = rel >= 0 ? rel : static_cast<double>(len) + rel;
    if (k < 0 || k >= static_cast<double>(len)) {
        return rtThrowRangeError("Invalid index").rawBits();
    }
    const uint32_t targetIdx = static_cast<uint32_t>(k);
    Rooted<Value> val{args[1]};
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < len; ++i) {
        if (i == targetIdx) {
            appendTo(out, val);
        } else {
            Rooted<Value> elem{elemOf(self.get(), i)};
            appendTo(out, elem);
        }
    }
    return out.get().rawBits();
}

uint64_t rtArrayToStringBuiltin(uint64_t, uint64_t thisBits, uint32_t argc,
                                const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (self.get().isNull() || self.get().isUndefined()) {
        return rtThrowTypeError("Cannot convert undefined or null to object").rawBits();
    }
    Rooted<Value> joinKey{rtMakeString("join")};
    Value joinMethod = Value::fromUndefined();
    if (isArray(self.get())) {
        joinMethod = rtNativeFunction(arrayJoin, 0);
    } else if (self.get().isObject() &&
               self.get().asObject<HeapObjectHeader>()->flags == TypedArrayHeader::kFlags) {
        joinMethod = rtTypedArrayMethod("join");
    } else if (self.get().isObject()) {
        joinMethod = self.get().asObject<ObjectHeader>()->getProp(rtHeap(), joinKey, nullptr,
                                                                  self.slot_ptr());
    }
    if (isCallable(joinMethod)) {
        return bronze_dynamic_call(joinMethod.rawBits(), self.get().rawBits(), 0, nullptr);
    }
    return rtObjectProtoToString(0, self.get().rawBits(), 0, nullptr);
}

}  // namespace bronze::runtime
