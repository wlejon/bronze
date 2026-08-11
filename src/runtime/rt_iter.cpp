// for-of, as an index walk (docs/0012 decision 2).
//
// bronze has no Symbol and no iterator protocol, so for-of is not "call
// [Symbol.iterator]": it walks indices. That covers every iterable bronze can
// build — arrays, strings and typed arrays — and anything else is a hard error
// naming itself rather than an empty loop, which is what a missing-protocol
// fallback would silently produce.
//
// A string iterates by CODE POINT, not by code unit, which is why the step is
// a helper rather than an `add 1` in the IL: a surrogate pair is one iteration
// yielding a two-unit string.

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

constexpr uint16_t kHighSurrogateFirst = 0xD800;
constexpr uint16_t kHighSurrogateLast = 0xDBFF;
constexpr uint16_t kLowSurrogateFirst = 0xDC00;
constexpr uint16_t kLowSurrogateLast = 0xDFFF;

bool isSurrogatePair(uint16_t high, uint16_t low) {
    return high >= kHighSurrogateFirst && high <= kHighSurrogateLast &&
           low >= kLowSurrogateFirst && low <= kLowSurrogateLast;
}

bool iterableLength(Value v, uint32_t& outLength) {
    if (v.isString()) {
        outLength = v.asString<StringHeader>()->getLength();
        return true;
    }
    if (v.isObject()) {
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr->flags == 1) {
            outLength = reinterpret_cast<ArrayHeader*>(hdr)->length;
            return true;
        }
        if (hdr->flags == 3) {
            outLength = reinterpret_cast<Float32ArrayHeader*>(hdr)->length;
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" {

double bronze_iter_length(uint64_t vBits) {
    uint32_t length = 0;
    if (!iterableLength(Value(vBits), length)) {
        fatal("for-of over a value that is not an array, string or typed array");
    }
    return static_cast<double>(length);
}

uint64_t bronze_iter_at(uint64_t vBits, double index) {
    Value v(vBits);
    const auto i = static_cast<uint32_t>(index);
    if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        const uint32_t len = str->getLength();
        if (i >= len) return Value::fromUndefined().rawBits();
        uint16_t unit = str->charCodeAt(i);
        // A high surrogate followed by a low one is ONE character.
        if (i + 1 < len && isSurrogatePair(unit, str->charCodeAt(i + 1))) {
            const uint16_t pair[2] = {unit, str->charCodeAt(i + 1)};
            return Value::fromString(StringHeader::createUTF16(rtHeap(), pair, 2)).rawBits();
        }
        if (unit < 0x100) {
            const char byte = static_cast<char>(unit);
            return Value::fromString(StringHeader::createLatin1(rtHeap(), &byte, 1)).rawBits();
        }
        return Value::fromString(StringHeader::createUTF16(rtHeap(), &unit, 1)).rawBits();
    }
    if (v.isObject()) {
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr->flags == 1) {
            return reinterpret_cast<ArrayHeader*>(hdr)->getElem(i).rawBits();
        }
        if (hdr->flags == 3) {
            auto* view = reinterpret_cast<Float32ArrayHeader*>(hdr);
            if (i >= view->length) return Value::fromUndefined().rawBits();
            return Value::fromDouble(static_cast<double>(view->data()[i])).rawBits();
        }
    }
    fatal("for-of over a value that is not an array, string or typed array");
}

double bronze_iter_advance(uint64_t vBits, double index) {
    Value v(vBits);
    const auto i = static_cast<uint32_t>(index);
    if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        if (i + 1 < str->getLength() &&
            isSurrogatePair(str->charCodeAt(i), str->charCodeAt(i + 1))) {
            return index + 2;
        }
    }
    return index + 1;
}

}  // extern "C"

}  // namespace bronze::runtime
