#include "runtime/builtin_array_internal.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"

namespace bronze::runtime {

namespace {

void flattenIntoArray(Rooted<Value>& out, Rooted<Value>& source, uint32_t sourceLen,
                      double depth, Rooted<Value>& mapperFn, Rooted<Value>& thisArg,
                      Rooted<Value>& originalThis) {
    for (uint32_t k = 0; k < sourceLen; ++k) {
        if (!rtArrayLikeHasElement(source, k)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(source, k)};
        if (!mapperFn.get().isUndefined()) {
            elem.set(callBack(mapperFn, thisArg, elem, k, originalThis));
        }
        bool shouldFlatten = false;
        if (depth > 0.0 && rtIsConcatSpreadable(elem)) {
            shouldFlatten = true;
        }
        if (shouldFlatten) {
            const uint32_t elemLen = isArray(elem.get()) ? lengthOf(elem.get()) : rtArrayLikeLength(elem);
            Rooted<Value> noMapper{Value::fromUndefined()};
            flattenIntoArray(out, elem, elemLen, depth - 1.0, noMapper, thisArg, originalThis);
        } else {
            appendTo(out, elem);
        }
    }
}

}  // namespace

uint64_t arraySlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "slice")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    uint32_t start = args.count() > 0 ? relativeIndex(toInteger(rtToNumber(args[0])), len) : 0;
    uint32_t end = args.count() > 1 && !args[1].isUndefined()
                       ? relativeIndex(toInteger(rtToNumber(args[1])), len)
                       : len;
    const uint32_t count = end > start ? end - start : 0;
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    if (isArray(self.get())) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t srcIdx = start + i;
            if (!hasIndex(self.get(), srcIdx)) {
                appendHole(out);
                continue;
            }
            Rooted<Value> elem{elemOf(self.get(), srcIdx)};
            appendTo(out, elem);
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t srcIdx = start + i;
            if (!rtArrayLikeHasElement(self, srcIdx)) {
                continue;
            }
            Rooted<Value> elem{rtArrayLikeGetElement(self, srcIdx)};
            rtCreateDataPropertyOrThrow(out, i, elem);
        }
        rtArrayLikeSetLength(out, count);
    }
    return out.get().rawBits();
}

uint64_t arrayConcat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "concat")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};

    uint32_t n = 0;
    auto appendItem = [&](Rooted<Value>& item) {
        if (rtIsConcatSpreadable(item)) {
            const uint32_t itemLen = isArray(item.get()) ? lengthOf(item.get()) : rtArrayLikeLength(item);
            for (uint32_t i = 0; i < itemLen; ++i) {
                if (rtArrayLikeHasElement(item, i)) {
                    Rooted<Value> elem{rtArrayLikeGetElement(item, i)};
                    rtCreateDataPropertyOrThrow(out, n, elem);
                }
                ++n;
            }
        } else {
            rtCreateDataPropertyOrThrow(out, n, item);
            ++n;
        }
    };

    appendItem(self);
    for (uint32_t a = 0; a < args.count(); ++a) {
        Rooted<Value> item{args[a]};
        appendItem(item);
    }
    rtArrayLikeSetLength(out, n);
    return out.get().rawBits();
}

uint64_t arrayJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "join")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    if (len == 0) return rtMakeString("").rawBits();

    std::string sep = ",";
    if (args.count() > 0 && !args[0].isUndefined()) {
        Rooted<Value> sepStr{rtValueToString(args[0])};
        sep = rtUtf8Chars(sepStr.get().asString<StringHeader>());
    }

    std::string result;
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) result += sep;
        if (!rtArrayLikeHasElement(self, i)) continue;
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        if (elem.get().isNull() || elem.get().isUndefined()) continue;
        if (isArray(elem.get())) {
            Rooted<Value> joinKey{rtMakeString("join")};
            Rooted<Value> joinMethod{Value(bronze_elem_get(elem.get().rawBits(), joinKey.get().rawBits()))};
            if (isCallable(joinMethod.get())) {
                uint64_t res = bronze_dynamic_call(joinMethod.get().rawBits(), elem.get().rawBits(), 0, nullptr);
                Rooted<Value> resStr{rtValueToString(Value(res))};
                result += rtUtf8Chars(resStr.get().asString<StringHeader>());
                continue;
            }
        }
        Rooted<Value> s{rtValueToString(elem.get())};
        result += rtUtf8Chars(s.get().asString<StringHeader>());
    }
    return rtMakeString(result).rawBits();
}

uint64_t arrayFlat(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "flat")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    double depth = 1.0;
    if (args.count() > 0 && !args[0].isUndefined()) {
        depth = toInteger(rtToNumber(args[0]));
        if (depth < 0.0) depth = 0.0;
    }
    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    Rooted<Value> noMapper{Value::fromUndefined()};
    Rooted<Value> thisArg{Value::fromUndefined()};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    flattenIntoArray(out, self, len, depth, noMapper, thisArg, self);
    return out.get().rawBits();
}

uint64_t arrayFlatMap(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "flatMap")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> fn{args[0]};
    if (!requireCallable(fn.get(), "flatMap")) return Value::fromUndefined().rawBits();
    Rooted<Value> thisArg{args[1]};

    Rooted<Value> out{rtArraySpeciesCreate(self, 0)};
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    flattenIntoArray(out, self, len, 1.0, fn, thisArg, self);
    return out.get().rawBits();
}

uint64_t arrayToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "toSorted")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    Rooted<Value> compareFn{args[0]};
    if (!compareFn.get().isUndefined() && !isCallable(compareFn.get())) {
        return rtThrowTypeError("The comparison function must be either a function or undefined")
            .rawBits();
    }
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        appendTo(out, elem);
    }
    uint64_t sortArg = compareFn.get().rawBits();
    rtArraySortBuiltin(env, out.get().rawBits(), 1, &sortArg);
    return out.get().rawBits();
}

uint64_t arrayToReversed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{toObject(Value(thisBits), "toReversed")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    Rooted<Value> out{newArray()};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{rtArrayLikeGetElement(self, len - 1 - i)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayToSpliced(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "toSpliced")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
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
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        appendTo(out, elem);
    }
    for (uint32_t i = 0; i < insertCount; ++i) {
        Rooted<Value> elem{args[i + 2]};
        appendTo(out, elem);
    }
    for (uint32_t i = start + deleteCount; i < len; ++i) {
        Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

uint64_t arrayWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "with")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
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
            Rooted<Value> elem{rtArrayLikeGetElement(self, i)};
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
        joinMethod = rtNativeFunction(arrayJoin, 0, "join", 1);
    } else if (self.get().isObject()) {
        // 23.1.3.36 step 2 is Get(array, "join") — for a typed array that is
        // the ordinary walk to `%TypedArray%.prototype.join`, or to whatever a
        // subclass put in its way. Through the generic funnel, which knows
        // every receiver kind — a function, a proxy — where a direct shape read
        // would trust a header this call has not checked.
        joinMethod = Value(bronze_elem_get(self.get().rawBits(), joinKey.get().rawBits()));
    }
    if (isCallable(joinMethod)) {
        return bronze_dynamic_call(joinMethod.rawBits(), self.get().rawBits(), 0, nullptr);
    }
    return rtObjectProtoToString(0, self.get().rawBits(), 0, nullptr);
}

}  // namespace bronze::runtime
