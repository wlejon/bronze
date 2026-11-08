#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/builtin_array_internal.h"
#include "runtime/fatal.h"
#include "runtime/number_format.h"
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
    // A hole is copied as a hole unless the chain answers it (23.1.3.28 step
    // 15.b is HasProperty), which the generic walk asks.
    if (isArray(self.get()) && !rtArrayChainHoldsIndex(self.get())) {
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

namespace {

// The text a join is building, in the code units the result will hold: Latin-1
// until the first unit past 0xFF arrives, UTF-16 from then on. Each piece is
// copied straight out of its StringHeader (or a number's digits out of a stack
// buffer), so a join allocates its result and nothing else per element —
// `getTextureCacheKey` and `getProgramCacheKey` join a dozen numbers, booleans
// and strings per draw. The Latin-1 text starts in an inline buffer, so a join
// that short mallocs nothing; it is plain C++ memory, so appending never
// collects and an element read just before an append stays valid.
class JoinBuffer {
public:
    JoinBuffer() = default;
    JoinBuffer(const JoinBuffer&) = delete;
    JoinBuffer& operator=(const JoinBuffer&) = delete;
    ~JoinBuffer() {
        if (lat_ != inline_) std::free(lat_);
    }

    void appendAscii(const char* s, size_t n) {
        if (wide_) {
            for (size_t i = 0; i < n; ++i) u16_.push_back(static_cast<unsigned char>(s[i]));
            return;
        }
        if (n > latCap_ - latLen_) grow(n);
        std::memcpy(lat_ + latLen_, s, n);
        latLen_ += n;
    }
    void appendComma() {
        if (wide_) {
            u16_.push_back(u',');
            return;
        }
        if (latLen_ == latCap_) grow(1);
        lat_[latLen_++] = ',';
    }
    // ToString of an integral Number whose magnitude is below 2^53: its
    // decimal digits, with a sign for a negative one (no -0 reaches here: the
    // caller sends 0 for both zeros).
    void appendInteger(int64_t v) {
        char buf[24];
        char* end = buf + sizeof(buf);
        char* p = end;
        uint64_t u = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
        do {
            *--p = static_cast<char>('0' + u % 10);
            u /= 10;
        } while (u != 0);
        if (v < 0) *--p = '-';
        appendAscii(p, static_cast<size_t>(end - p));
    }
    void append(const StringHeader* s) {
        const uint32_t n = s->getLength();
        if (s->isLatin1()) {
            appendAscii(s->latin1Data(), n);
            return;
        }
        const uint16_t* d = s->utf16Data();
        if (!wide_) {
            uint32_t i = 0;
            while (i < n && d[i] <= 0xFF) ++i;
            if (i == n) {
                if (n > latCap_ - latLen_) grow(n);
                for (uint32_t k = 0; k < n; ++k) lat_[latLen_++] = static_cast<char>(d[k]);
                return;
            }
            widen();
        }
        u16_.insert(u16_.end(), d, d + n);
    }
    Value finish() {
        if (wide_) {
            return Value::fromString(StringHeader::createUTF16(
                rtHeap(), u16_.data(), static_cast<uint32_t>(u16_.size())));
        }
        return Value::fromString(
            StringHeader::createLatin1(rtHeap(), lat_, static_cast<uint32_t>(latLen_)));
    }

private:
    void grow(size_t need) {
        size_t cap = latCap_ * 2;
        if (cap < latLen_ + need) cap = latLen_ + need;
        char* next = static_cast<char*>(std::malloc(cap));
        if (!next) fatal("out of memory in Array.prototype.join");
        std::memcpy(next, lat_, latLen_);
        if (lat_ != inline_) std::free(lat_);
        lat_ = next;
        latCap_ = cap;
    }
    void widen() {
        wide_ = true;
        u16_.reserve(latLen_ * 2 + 16);
        for (size_t i = 0; i < latLen_; ++i) u16_.push_back(static_cast<unsigned char>(lat_[i]));
        latLen_ = 0;
    }
    bool wide_ = false;
    char inline_[256];
    char* lat_ = inline_;
    size_t latLen_ = 0;
    size_t latCap_ = sizeof(inline_);
    std::vector<uint16_t> u16_;
};

// Below 2^53 every integral double is exact, and its ToString is its decimal
// digits (number_format.cpp says why): the join writes those itself.
constexpr double kExactIntegerBound = 9007199254740992.0;

// One element's text, for the primitives without a heap string of their own
// to make: a number formats into a stack buffer, a boolean is its literal.
// Answers false for anything else, which takes ToString. Never allocates on
// the JS heap, so `v` may be an unrooted read.
bool appendPrimitive(JoinBuffer& out, Value v) {
    if (v.isString()) {
        out.append(v.asString<StringHeader>());
        return true;
    }
    if (v.isInt32()) {
        out.appendInteger(static_cast<int32_t>(v.payload()));
        return true;
    }
    if (v.isNumber()) {
        const double d = v.asNumber();
        // -0 is integral and compares equal to 0, so it joins as "0".
        if (d > -kExactIntegerBound && d < kExactIntegerBound) {
            const auto whole = static_cast<int64_t>(d);
            if (static_cast<double>(whole) == d) {
                out.appendInteger(whole);
                return true;
            }
        }
        char buf[64];
        out.appendAscii(buf, formatJsNumber(d, buf));
        return true;
    }
    if (v.isBool()) {
        if (v.asBool()) out.appendAscii("true", 4);
        else out.appendAscii("false", 5);
        return true;
    }
    return false;
}

}  // namespace

uint64_t arrayJoin(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{toObject(Value(thisBits), "join")};
    if (self.get().isUndefined()) return Value::fromUndefined().rawBits();
    const uint32_t len = isArray(self.get()) ? lengthOf(self.get()) : rtArrayLikeLength(self);
    if (len == 0) return rtMakeString("").rawBits();

    // The default separator is appended as its byte, with no string made.
    const bool comma = args.count() == 0 || args[0].isUndefined();
    Rooted<Value> sep{Value::fromUndefined()};
    if (!comma) sep.set(rtValueToString(args[0]));

    // An array's own elements are read straight out of it while no prototype
    // holds an index (a hole then reads as undefined, which joins as ""). Only
    // script can change that answer — an element's toString or join, a getter
    // on the generic path — so it is asked again after each element that ran
    // one, and a primitive element costs no question and no root.
    const bool selfIsArray = isArray(self.get());
    bool direct = selfIsArray && !rtArrayChainHoldsIndex(self.get());
    JoinBuffer result;
    for (uint32_t i = 0; i < len; ++i) {
        if (i > 0) {
            if (comma) result.appendComma();
            else result.append(sep.get().asString<StringHeader>());
        }
        if (direct) {
            // getElem answers undefined past a length script has shortened.
            const Value raw = self.get().asObject<ArrayHeader>()->getElem(i);
            if (raw.isNull() || raw.isUndefined()) continue;
            if (appendPrimitive(result, raw)) continue;
        }
        Rooted<Value> elem{Value::fromUndefined()};
        if (direct) {
            elem.set(elemOf(self.get(), i));
        } else {
            if (!rtArrayLikeHasElement(self, i)) continue;
            elem.set(rtArrayLikeGetElement(self, i));
            if (elem.get().isNull() || elem.get().isUndefined()) continue;
            if (appendPrimitive(result, elem.get())) continue;
        }
        if (isArray(elem.get())) {
            Rooted<Value> joinKey{rtMakeString("join")};
            Rooted<Value> joinMethod{Value(bronze_elem_get(elem.get().rawBits(), joinKey.get().rawBits()))};
            if (isCallable(joinMethod.get())) {
                uint64_t res = bronze_dynamic_call(joinMethod.get().rawBits(), elem.get().rawBits(), 0, nullptr);
                Rooted<Value> resStr{rtValueToString(Value(res))};
                result.append(resStr.get().asString<StringHeader>());
                direct = selfIsArray && !rtArrayChainHoldsIndex(self.get());
                continue;
            }
        }
        Rooted<Value> s{rtValueToString(elem.get())};
        result.append(s.get().asString<StringHeader>());
        direct = selfIsArray && !rtArrayChainHoldsIndex(self.get());
    }
    return result.finish().rawBits();
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
