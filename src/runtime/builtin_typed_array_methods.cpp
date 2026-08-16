#include "runtime/builtin_typed_array_internal.h"

namespace bronze::runtime {

uint64_t taSet(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "set")) return Value::fromUndefined().rawBits();
    Rooted<Value> source{args[0]};

    const double offsetNum = toInteger(args[1].isUndefined() ? 0.0 : rtToNumber(args[1]));
    if (offsetNum < 0) {
        return rtThrowRangeError("offset is out of bounds").rawBits();
    }
    const uint32_t targetLength = lengthOf(self.get());
    const auto offset = static_cast<uint64_t>(offsetNum);

    if (isTypedArray(source.get())) {
        const uint32_t srcLength = lengthOf(source.get());
        if (offset + srcLength > targetLength) {
            return rtThrowRangeError("offset is out of bounds").rawBits();
        }
        auto* srcView = source.get().asObject<TypedArrayHeader>();
        auto* targetView = self.get().asObject<TypedArrayHeader>();
        if (srcView->elementKind() == targetView->elementKind()) {
            const uint32_t bpe = targetView->bytesPerElement();
            std::memmove(targetView->bytes() + static_cast<uint32_t>(offset) * bpe,
                         srcView->bytes(), static_cast<size_t>(srcLength) * bpe);
            return Value::fromUndefined().rawBits();
        }
        std::vector<double> staged(srcLength);
        for (uint32_t i = 0; i < srcLength; ++i) staged[i] = elemOf(source.get(), i);
        for (uint32_t i = 0; i < srcLength; ++i) {
            targetView->set(static_cast<uint32_t>(offset) + i, staged[i]);
        }
        return Value::fromUndefined().rawBits();
    }

    if (!isArray(source.get())) {
        return rtThrowTypeError("%TypedArray%.prototype.set takes a typed array or an array")
            .rawBits();
    }
    const uint32_t srcLength = source.get().asObject<ArrayHeader>()->length;
    if (offset + srcLength > targetLength) {
        return rtThrowRangeError("offset is out of bounds").rawBits();
    }
    for (uint32_t i = 0; i < srcLength; ++i) {
        Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
        const double v = rtToNumber(elem.get());
        if (rtExceptionPending()) break;
        self.get().asObject<TypedArrayHeader>()->set(static_cast<uint32_t>(offset) + i, v);
    }
    return Value::fromUndefined().rawBits();
}

uint64_t taSubarray(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "subarray")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, begin, 0);
    relativeArg(args[1], len, end, len);
    const uint32_t count = end > begin ? end - begin : 0;

    auto* view = self.get().asObject<TypedArrayHeader>();
    const ElementKind kind = view->elementKind();
    const uint32_t byteOffset = view->byteOffset + begin * view->bytesPerElement();
    Rooted<Value> buffer{view->buffer};
    return Value::fromObject(
               TypedArrayHeader::createOverBuffer(rtHeap(), kind, buffer, byteOffset, count))
        .rawBits();
}

uint64_t taSlice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "slice")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, begin, 0);
    relativeArg(args[1], len, end, len);
    const uint32_t count = end > begin ? end - begin : 0;

    Rooted<Value> out{newViewLike(self.get(), count)};
    for (uint32_t i = 0; i < count; ++i) {
        out.get().asObject<TypedArrayHeader>()->set(i, elemOf(self.get(), begin + i));
    }
    return out.get().rawBits();
}

uint64_t taFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "fill")) return Value::fromUndefined().rawBits();

    const double value = rtToNumber(args[0]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t start = 0;
    uint32_t end = len;
    relativeArg(args[1], len, start, 0);
    relativeArg(args[2], len, end, len);

    auto* view = self.get().asObject<TypedArrayHeader>();
    for (uint32_t i = start; i < end; ++i) view->set(i, value);
    return self.get().rawBits();
}

uint64_t taCopyWithin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "copyWithin")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    uint32_t to = 0;
    uint32_t from = 0;
    uint32_t final = len;
    relativeArg(args[0], len, to, 0);
    relativeArg(args[1], len, from, 0);
    relativeArg(args[2], len, final, len);

    const uint32_t count = final > from ? std::min(final - from, len - to) : 0;
    if (count == 0) return self.get().rawBits();

    auto* view = self.get().asObject<TypedArrayHeader>();
    const size_t bpe = view->bytesPerElement();
    uint8_t* dst = view->bytes() + static_cast<size_t>(to) * bpe;
    const uint8_t* src = view->bytes() + static_cast<size_t>(from) * bpe;
    std::memmove(dst, src, count * bpe);
    return self.get().rawBits();
}

uint64_t taReverse(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "reverse")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    auto* view = self.get().asObject<TypedArrayHeader>();
    for (uint32_t i = 0, j = len; i + 1 < j; ++i, --j) {
        double vi = view->get(i);
        double vj = view->get(j - 1);
        view->set(i, vj);
        view->set(j - 1, vi);
    }
    return self.get().rawBits();
}

uint64_t taSort(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "sort")) return Value::fromUndefined().rawBits();
    Rooted<Value> compareFn{args[0]};
    if (!compareFn.get().isUndefined() && !isCallable(compareFn.get())) {
        return rtThrowTypeError("The comparison function must be either a function or undefined")
            .rawBits();
    }

    const uint32_t len = lengthOf(self.get());
    std::vector<double> elements(len);
    for (uint32_t i = 0; i < len; ++i) elements[i] = elemOf(self.get(), i);

    if (compareFn.get().isUndefined()) {
        std::sort(elements.begin(), elements.end(), [](double a, double b) {
            if (std::isnan(a)) return false;
            if (std::isnan(b)) return true;
            if (a == 0.0 && b == 0.0) {
                return std::signbit(a) && !std::signbit(b);
            }
            return a < b;
        });
    } else {
        // Stable sort with custom comparator
        std::stable_sort(elements.begin(), elements.end(), [&](double a, double b) {
            if (std::isnan(a) && std::isnan(b)) return false;
            if (std::isnan(a)) return false;
            if (std::isnan(b)) return true;
            Value block[2] = {Value::fromDouble(a), Value::fromDouble(b)};
            Value res(bronze_dynamic_call(compareFn.get().rawBits(), BRONZE_ABI_UNDEFINED_BITS, 2,
                                          reinterpret_cast<const uint64_t*>(block)));
            if (rtExceptionPending()) return false;
            const double v = rtToNumber(res);
            return !std::isnan(v) && v < 0;
        });
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }

    auto* view = self.get().asObject<TypedArrayHeader>();
    for (uint32_t i = 0; i < len; ++i) view->set(i, elements[i]);
    return self.get().rawBits();
}

uint64_t taJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "join")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    if (len == 0) return rtMakeString("").rawBits();

    std::string sep = ",";
    if (args.count() > 0 && !args[0].isUndefined()) {
        Rooted<Value> sepVal{rtValueToString(args[0])};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        sep = rtUtf8Chars(sepVal.get().asString<StringHeader>());
    }

    std::string out;
    char buf[64];
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) out += sep;
        size_t n = formatJsNumber(elemOf(self.get(), i), buf);
        out.append(buf, n);
    }
    return rtMakeString(out).rawBits();
}

uint64_t taToReversed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "toReversed")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    for (uint32_t i = 0; i < len; ++i) {
        out.get().asObject<TypedArrayHeader>()->set(i, elemOf(self.get(), len - 1 - i));
    }
    return out.get().rawBits();
}

uint64_t taToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "toSorted")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    for (uint32_t i = 0; i < len; ++i) {
        out.get().asObject<TypedArrayHeader>()->set(i, elemOf(self.get(), i));
    }
    uint64_t cmp = args[0].rawBits();
    taSort(env, out.get().rawBits(), 1, &cmp);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return out.get().rawBits();
}

uint64_t taWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "with")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    double rel = toInteger(rtToNumber(args[0]));
    double k = rel >= 0 ? rel : static_cast<double>(len) + rel;
    if (k < 0 || k >= static_cast<double>(len)) {
        return rtThrowRangeError("Invalid index").rawBits();
    }
    const double val = rtToNumber(args[1]);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    Rooted<Value> out{newViewLike(self.get(), len)};
    for (uint32_t i = 0; i < len; ++i) {
        out.get().asObject<TypedArrayHeader>()->set(i, i == static_cast<uint32_t>(k)
                                                           ? val
                                                           : elemOf(self.get(), i));
    }
    return out.get().rawBits();
}

uint64_t taAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "at")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    double rel = toInteger(rtToNumber(args[0]));
    double k = rel >= 0 ? rel : static_cast<double>(len) + rel;
    if (k < 0 || k >= static_cast<double>(len)) {
        return Value::fromUndefined().rawBits();
    }
    return Value::fromDouble(elemOf(self.get(), static_cast<uint32_t>(k))).rawBits();
}

namespace {

struct Method {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const Method kMethods[] = {
    {"at", taAt, 1},
    {"copyWithin", taCopyWithin, 0},
    {"entries", taEntries, 0},
    {"every", taEvery, 1},
    {"fill", taFill, 0},
    {"filter", taFilter, 1},
    {"find", taFind, 1},
    {"findIndex", taFindIndex, 1},
    {"findLast", taFindLast, 1},
    {"findLastIndex", taFindLastIndex, 1},
    {"forEach", taForEach, 1},
    {"includes", taIncludes, 1},
    {"indexOf", taIndexOf, 1},
    {"join", taJoin, 0},
    {"keys", taKeys, 0},
    {"lastIndexOf", taLastIndexOf, 1},
    {"map", taMap, 1},
    {"reduce", taReduce, 0},
    {"reduceRight", taReduceRight, 0},
    {"reverse", taReverse, 0},
    {"set", taSet, 0},
    {"slice", taSlice, 0},
    {"some", taSome, 1},
    {"sort", taSort, 0},
    {"subarray", taSubarray, 0},
    {"toReversed", taToReversed, 0},
    {"toSorted", taToSorted, 0},
    {"toString", rtArrayToStringBuiltin, 0},
    {"values", taValues, 0},
    {"with", taWith, 2},
};

}  // namespace

Value rtTypedArrayMethod(const std::string& key) {
    for (const Method& m : kMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity);
    }
    return Value::fromUndefined();
}

bool rtTypedArrayHasMethod(const std::string& key) {
    for (const Method& m : kMethods) {
        if (key == m.name) return true;
    }
    return false;
}

}  // namespace bronze::runtime
