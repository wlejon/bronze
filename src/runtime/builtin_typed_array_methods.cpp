#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"

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
        // 23.2.5.1.17 step 5: a BigInt side and a Number side never mix — a
        // TypeError, not a conversion. BOTH-BigInt is the memmove below even
        // across the two kinds: a store wraps modulo 2^64 and a signed and an
        // unsigned element with the same mathematical residue hold the same
        // eight bytes, so the bytes already are the converted answer.
        const bool srcBig = isBigIntElementKind(srcView->elementKind());
        const bool dstBig = isBigIntElementKind(targetView->elementKind());
        if (srcBig != dstBig) {
            return rtThrowTypeError("Cannot mix BigInt and other types, use explicit conversions")
                .rawBits();
        }
        if (srcView->elementKind() == targetView->elementKind() || srcBig) {
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
    if (isBigIntView(self.get())) {
        // 23.2.5.1.18 over a BigInt target: each element through ToBigInt —
        // which can run user code, so the view is re-derived per store and the
        // maintained length guards a window that shrank mid-loop.
        for (uint32_t i = 0; i < srcLength; ++i) {
            Rooted<Value> elem{source.get().asObject<ArrayHeader>()->getElem(i)};
            uint64_t bits = 0;
            if (!rtBigIntToRawBits64(elem.get(), bits)) break;
            auto* live = self.get().asObject<TypedArrayHeader>();
            const uint32_t at = static_cast<uint32_t>(offset) + i;
            if (at < live->length) live->setRawBits64(at, bits);
        }
        return Value::fromUndefined().rawBits();
    }
    uint32_t i = 0;
    // The number-elements fast loop (seam: BRONZE_NO_TA_SET_FAST=1): while
    // every element IS a number, ToNumber is an identity and no user code can
    // run — so nothing allocates, the raw headers stay valid for the whole
    // run, and the per-element root the spec-shaped loop below opens defends
    // nothing. three.js reaches this path per draw, copying a matrix's plain
    // `elements` array into a uniform upload buffer. The first non-number
    // element (a hole reads as `undefined` and is one) resumes the rooted
    // loop AT that element with every spec'd conversion intact.
    if (rtTls()->ta_set_fast_enabled != 0) {
        const auto* srcArr = source.get().asObject<ArrayHeader>();
        auto* dst = self.get().asObject<TypedArrayHeader>();
        const Value* elems = srcArr->elementsData();
        if (elems != nullptr) {
            const uint32_t bound = srcLength < srcArr->length ? srcLength : srcArr->length;
            for (; i < bound; ++i) {
                const Value e = elems[i];
                if (!e.isNumber()) break;
                dst->set(static_cast<uint32_t>(offset) + i, e.asNumber());
            }
        }
    }
    for (; i < srcLength; ++i) {
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
    // 23.2.3.30 is the one prototype method with NO ValidateTypedArray: a
    // detached or stranded source still answers — its length clamps to 0 (the
    // maintained field is already there) and the CONSTRUCTION at the end is
    // the validator, exactly as the specification's species-create is. So
    // only the brand is required here, and requireTypedArray's throws are
    // deliberately not.
    if (!isTypedArray(self.get())) {
        return rtThrowTypeError(
                   "%TypedArray%.prototype.subarray called on a value that is not a typed array")
            .rawBits();
    }

    const uint32_t len = lengthOf(self.get());
    // The result of `subarray(begin)` on a tracking source is itself
    // tracking (23.2.3.30 step 13: [[ArrayLength]] auto and end undefined
    // construct without a length). Decided before the conversions below,
    // which can run user code.
    const bool trackingResult =
        self.get().asObject<TypedArrayHeader>()->isTracking() && args[1].isUndefined();
    uint32_t begin = 0;
    uint32_t end = len;
    relativeArg(args[0], len, begin, 0);
    relativeArg(args[1], len, end, len);
    const uint32_t count = end > begin ? end - begin : 0;

    // Re-derived after relativeArg's ToNumber, whose `valueOf` can allocate —
    // and can also detach or resize the buffer, which is why the window is
    // validated below against the buffer as it is NOW, with the constructor's
    // errors: the specification reaches them through TypedArraySpeciesCreate.
    auto* view = self.get().asObject<TypedArrayHeader>();
    const ElementKind kind = view->elementKind();
    const uint32_t bpe = view->bytesPerElement();
    const uint32_t byteOffset = view->byteOffset + begin * bpe;
    Rooted<Value> buffer{view->buffer};
    auto* buf = buffer.get().asObject<ArrayBufferHeader>();
    if (buf->isDetached()) {
        return rtThrowTypeError("ArrayBuffer is detached").rawBits();
    }
    if (byteOffset > buf->byteLength) {
        return rtThrowRangeError("Start offset " + std::to_string(byteOffset) +
                                 " is outside the bounds of the buffer")
            .rawBits();
    }
    if (trackingResult) {
        return Value::fromObject(TypedArrayHeader::createOverBuffer(
                                     rtHeap(), kind, buffer, byteOffset,
                                     (buf->byteLength - byteOffset) / bpe, /*tracking=*/true))
            .rawBits();
    }
    if (static_cast<uint64_t>(byteOffset) + static_cast<uint64_t>(count) * bpe >
        buf->byteLength) {
        return rtThrowRangeError("Invalid typed array length: " + std::to_string(count))
            .rawBits();
    }
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
    // Same kind on both sides, so the elements move as BYTES — which is also
    // the only road a BigInt element has: a double cannot carry it. The copy
    // is clamped to the source's window as it is NOW, because relativeArg's
    // ToNumber can run user code that shrank it; the tail of a clamped copy
    // stays the zero-fill the allocation gave it, as 23.2.3.27's re-validation
    // leaves it.
    auto* src = self.get().asObject<TypedArrayHeader>();
    auto* dst = out.get().asObject<TypedArrayHeader>();
    const uint32_t copyable =
        src->length > begin ? std::min(count, src->length - begin) : 0;
    if (copyable > 0) {
        const uint32_t bpe = src->bytesPerElement();
        std::memcpy(dst->bytes(), src->bytes() + static_cast<size_t>(begin) * bpe,
                    static_cast<size_t>(copyable) * bpe);
    }
    return out.get().rawBits();
}

uint64_t taFill(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "fill")) return Value::fromUndefined().rawBits();

    // 23.2.3.8 step 2: the VALUE converts first — ToBigInt for a BigInt view
    // (a Number is the TypeError 7.1.13 names), ToNumber for the rest — and
    // start/end after it.
    const bool big = isBigIntView(self.get());
    uint64_t bits = 0;
    double value = 0;
    if (big) {
        if (!rtBigIntToRawBits64(args[0], bits)) return Value::fromUndefined().rawBits();
    } else {
        value = rtToNumber(args[0]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }

    const uint32_t len = lengthOf(self.get());
    uint32_t start = 0;
    uint32_t end = len;
    relativeArg(args[1], len, start, 0);
    relativeArg(args[2], len, end, len);

    // Every conversion above can run user code that shrank the window;
    // 23.2.3.8 step 9 re-reads the length, and the maintained field IS that
    // re-read.
    auto* view = self.get().asObject<TypedArrayHeader>();
    end = std::min(end, view->length);
    if (big) {
        for (uint32_t i = start; i < end; ++i) view->setRawBits64(i, bits);
    } else {
        for (uint32_t i = start; i < end; ++i) view->set(i, value);
    }
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
    if (isBigIntElementKind(view->elementKind())) {
        for (uint32_t i = 0, j = len; i + 1 < j; ++i, --j) {
            const uint64_t vi = view->rawBits64(i);
            view->setRawBits64(i, view->rawBits64(j - 1));
            view->setRawBits64(j - 1, vi);
        }
        return self.get().rawBits();
    }
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
    if (isBigIntView(self.get())) {
        // Staged as the stored eight bytes — plain C++ memory the comparator's
        // allocations cannot move — and materialised as BigInt VALUES only for
        // the comparator's arguments. 23.2.4.7 orders BigInt elements by
        // mathematical value: the signed reading for BigInt64, the unsigned
        // for BigUint64, and the same eight bytes sort differently under the
        // two, which is why the kind is consulted and not just the width.
        const bool isSigned = kindOf(self.get()) == ElementKind::BigInt64;
        std::vector<uint64_t> elements(len);
        for (uint32_t i = 0; i < len; ++i) elements[i] = bitsOf(self.get(), i);

        if (compareFn.get().isUndefined()) {
            if (isSigned) {
                std::sort(elements.begin(), elements.end(), [](uint64_t a, uint64_t b) {
                    return static_cast<int64_t>(a) < static_cast<int64_t>(b);
                });
            } else {
                std::sort(elements.begin(), elements.end());
            }
        } else {
            std::stable_sort(elements.begin(), elements.end(), [&](uint64_t a, uint64_t b) {
                Rooted<Value> av{rtBigIntFromRawBits64(a, isSigned)};
                Rooted<Value> bv{rtBigIntFromRawBits64(b, isSigned)};
                Value block[2] = {av.get(), bv.get()};
                Value res(bronze_dynamic_call(compareFn.get().rawBits(),
                                              BRONZE_ABI_UNDEFINED_BITS, 2,
                                              reinterpret_cast<const uint64_t*>(block)));
                if (rtExceptionPending()) return false;
                const double v = rtToNumber(res);
                return !std::isnan(v) && v < 0;
            });
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        }

        auto* view = self.get().asObject<TypedArrayHeader>();
        // The comparator can shrink the window; writes past the live length
        // are the discards 10.4.5.16 makes them.
        const uint32_t writable = std::min(len, view->length);
        for (uint32_t i = 0; i < writable; ++i) view->setRawBits64(i, elements[i]);
        return self.get().rawBits();
    }
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
    const uint32_t writable = std::min(len, view->length);
    for (uint32_t i = 0; i < writable; ++i) view->set(i, elements[i]);
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
    // ToString of a BigInt element is its decimal digits with no `n` — and the
    // eight stored bytes fit a machine integer, so the digits come straight
    // from them with no BigInt value ever built.
    const bool big = isBigIntView(self.get());
    const bool isSigned = big && kindOf(self.get()) == ElementKind::BigInt64;
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) out += sep;
        if (big) {
            out += rtBigIntDecimalOfRawBits64(bitsOf(self.get(), i), isSigned);
        } else {
            size_t n = formatJsNumber(elemOf(self.get(), i), buf);
            out.append(buf, n);
        }
    }
    return rtMakeString(out).rawBits();
}

uint64_t taToReversed(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "toReversed")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    // Element bytes move as bytes, kind-agnostic — the only road a BigInt
    // element has. Nothing between the allocation and the loop runs user code
    // or allocates, so the two raw pointers stay true.
    auto* src = self.get().asObject<TypedArrayHeader>();
    auto* dst = out.get().asObject<TypedArrayHeader>();
    const uint32_t bpe = src->bytesPerElement();
    for (uint32_t i = 0; i < len; ++i) {
        std::memcpy(dst->bytes() + static_cast<size_t>(i) * bpe,
                    src->bytes() + static_cast<size_t>(len - 1 - i) * bpe, bpe);
    }
    return out.get().rawBits();
}

uint64_t taToSorted(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireTypedArray(self.get(), "toSorted")) return Value::fromUndefined().rawBits();

    const uint32_t len = lengthOf(self.get());
    Rooted<Value> out{newViewLike(self.get(), len)};
    {
        auto* src = self.get().asObject<TypedArrayHeader>();
        auto* dst = out.get().asObject<TypedArrayHeader>();
        std::memcpy(dst->bytes(), src->bytes(),
                    static_cast<size_t>(len) * src->bytesPerElement());
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
    // 23.2.3.36 converts the VALUE (step 6) before it judges the index (step
    // 7), so a throwing conversion wins over the RangeError — ToBigInt for a
    // BigInt view, ToNumber for the rest.
    const bool big = isBigIntView(self.get());
    uint64_t bits = 0;
    double val = 0;
    if (big) {
        if (!rtBigIntToRawBits64(args[1], bits)) return Value::fromUndefined().rawBits();
    } else {
        val = rtToNumber(args[1]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    if (k < 0 || k >= static_cast<double>(len)) {
        return rtThrowRangeError("Invalid index").rawBits();
    }

    Rooted<Value> out{newViewLike(self.get(), len)};
    auto* src = self.get().asObject<TypedArrayHeader>();
    auto* dst = out.get().asObject<TypedArrayHeader>();
    // The conversions can shrink the source's window; the clamped tail stays
    // the allocation's zero-fill.
    const uint32_t copyable = std::min(len, src->length);
    std::memcpy(dst->bytes(), src->bytes(),
                static_cast<size_t>(copyable) * src->bytesPerElement());
    if (big) {
        dst->setRawBits64(static_cast<uint32_t>(k), bits);
    } else {
        dst->set(static_cast<uint32_t>(k), val);
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
    return rtTypedArrayElement(self.get(), static_cast<uint32_t>(k)).rawBits();
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
