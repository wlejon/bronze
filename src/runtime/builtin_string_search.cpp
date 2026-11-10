// String.prototype's members that read a string in place: the single-unit
// readers, the searches, and slice / substring / substr, beside the fast
// entries a compiled method site enters for the common ones (native_fast.h).
// The table that installs them is builtin_string.cpp's.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "runtime/builtin_string_internal.h"
#include "runtime/exception.h"
#include "runtime/native_fast.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime::string_detail {

namespace {

bool matchAtHeaders(const StringHeader* hay, const StringHeader* needle, uint32_t at) {
    const uint32_t hayLen = hay->getLength();
    const uint32_t needleLen = needle->getLength();
    if (needleLen > hayLen || at > hayLen - needleLen) return false;
    if (needleLen == 0) return true;
    if (hay->isLatin1() && needle->isLatin1()) {
        return std::memcmp(hay->latin1Data() + at, needle->latin1Data(), needleLen) == 0;
    }
    if (!hay->isLatin1() && !needle->isLatin1()) {
        return std::memcmp(hay->utf16Data() + at, needle->utf16Data(),
                           static_cast<size_t>(needleLen) * sizeof(uint16_t)) == 0;
    }
    if (hay->isLatin1()) {
        const auto* h = reinterpret_cast<const uint8_t*>(hay->latin1Data()) + at;
        const uint16_t* n = needle->utf16Data();
        for (uint32_t i = 0; i < needleLen; ++i) {
            if (static_cast<uint16_t>(h[i]) != n[i]) return false;
        }
        return true;
    }
    const uint16_t* h = hay->utf16Data() + at;
    const auto* n = reinterpret_cast<const uint8_t*>(needle->latin1Data());
    for (uint32_t i = 0; i < needleLen; ++i) {
        if (h[i] != static_cast<uint16_t>(n[i])) return false;
    }
    return true;
}

int64_t indexOfHeaders(const StringHeader* hay, const StringHeader* needle, uint32_t from) {
    const uint32_t hayLen = hay->getLength();
    const uint32_t needleLen = needle->getLength();
    if (needleLen > hayLen || from > hayLen - needleLen) return -1;
    if (needleLen == 0) return static_cast<int64_t>(from);
    if (hay->isLatin1() && needle->isLatin1()) {
        const std::string_view h(hay->latin1Data(), hayLen);
        const std::string_view n(needle->latin1Data(), needleLen);
        const size_t pos = h.find(n, from);
        return pos == std::string_view::npos ? -1 : static_cast<int64_t>(pos);
    }
    if (!hay->isLatin1() && !needle->isLatin1()) {
        const std::basic_string_view<uint16_t> h(hay->utf16Data(), hayLen);
        const std::basic_string_view<uint16_t> n(needle->utf16Data(), needleLen);
        const size_t pos = h.find(n, from);
        return pos == std::basic_string_view<uint16_t>::npos ? -1 : static_cast<int64_t>(pos);
    }
    if (hay->isLatin1()) {
        const uint16_t* n = needle->utf16Data();
        for (uint32_t i = 0; i < needleLen; ++i) {
            if (n[i] > 0xFF) return -1;
        }
    }
    const uint32_t limit = hayLen - needleLen;
    for (uint32_t i = from; i <= limit; ++i) {
        if (matchAtHeaders(hay, needle, i)) return static_cast<int64_t>(i);
    }
    return -1;
}

Value directSlice(const Rooted<Value>& strRoot, uint32_t start, uint32_t end) {
    if (end <= start) {
        return rtMakeString("");
    }
    const uint32_t sliceLen = end - start;
    const StringHeader* s = strRoot.get().asString<StringHeader>();
    if (s->isLatin1()) {
        StringHeader* res = StringHeader::createLatin1(rtHeap(), nullptr, sliceLen);
        const StringHeader* src = strRoot.get().asString<StringHeader>();
        std::memcpy(res->latin1Data(), src->latin1Data() + start, sliceLen);
        return Value::fromString(res);
    }
    const uint16_t* u16 = s->utf16Data() + start;
    bool allLatin1 = true;
    for (uint32_t i = 0; i < sliceLen; ++i) {
        if (u16[i] > 0xFF) {
            allLatin1 = false;
            break;
        }
    }
    if (allLatin1) {
        StringHeader* res = StringHeader::createLatin1(rtHeap(), nullptr, sliceLen);
        const StringHeader* src = strRoot.get().asString<StringHeader>();
        const uint16_t* srcU16 = src->utf16Data() + start;
        char* dst = res->latin1Data();
        for (uint32_t i = 0; i < sliceLen; ++i) {
            dst[i] = static_cast<char>(srcU16[i]);
        }
        return Value::fromString(res);
    }
    StringHeader* res = StringHeader::createUTF16(rtHeap(), nullptr, sliceLen);
    const StringHeader* src = strRoot.get().asString<StringHeader>();
    std::memcpy(res->utf16Data(), src->utf16Data() + start, sliceLen * sizeof(uint16_t));
    return Value::fromString(res);
}

}  // namespace

// ---- simple readers -------------------------------------------------------

extern "C" uint64_t bronze_string_char_at(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError("String.prototype.charAt called on null or undefined")
            .rawBits();
    }
    Rooted<Value> str{selfVal};
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    const uint32_t len = str.get().asString<StringHeader>()->getLength();
    if (idx < 0 || idx >= static_cast<double>(len)) {
        return rtMakeString("").rawBits();  // out of range is "", not undefined
    }
    const auto i = static_cast<uint32_t>(idx);
    return directSlice(str, i, i + 1).rawBits();
}

uint64_t stringAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError("String.prototype.at called on null or undefined")
            .rawBits();
    }
    Rooted<Value> str{selfVal};
    double rel = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    const uint32_t len = str.get().asString<StringHeader>()->getLength();
    double idx = rel < 0 ? static_cast<double>(len) + rel : rel;
    if (idx < 0 || idx >= static_cast<double>(len)) {
        return Value::fromUndefined().rawBits();  // out of range is undefined, unlike charAt
    }
    const auto i = static_cast<uint32_t>(idx);
    return directSlice(str, i, i + 1).rawBits();
}

// Out of range is NaN, not 0: 0 is a real code unit (NUL), so answering it
// for a missing position is indistinguishable from finding one.
//
// Reads the header directly rather than through `thisUnits`, because this is
// the one member that needs a single unit and not the whole string — copying
// the string to read one character of it is a cost `for (i…) s.charCodeAt(i)`
// would pay per iteration.
extern "C" uint64_t bronze_string_char_code_at(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self;
    if (!rtStringMethodThis(Value(thisBits), self)) {
        return rtThrowTypeError(
                   "String.prototype.charCodeAt called on null or undefined")
            .rawBits();
    }
    // The index conversion runs 7.1.4 and so may call user code, which
    // allocates and can move the string; the header is therefore taken AFTER
    // it, out of a rooted slot the collector updates.
    Rooted<Value> str{self};
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    const StringHeader* s = str.get().asString<StringHeader>();
    if (idx < 0 || idx >= static_cast<double>(s->getLength())) {
        return Value::fromDouble(std::numeric_limits<double>::quiet_NaN()).rawBits();
    }
    return Value::fromDouble(s->charCodeAt(static_cast<uint32_t>(idx))).rawBits();
}

// Out of range is `undefined` here and NaN in charCodeAt above. That is
// 22.1.3.4 step 4 rather than an inconsistency between the two: charCodeAt
// answers a Number at every position it accepts, so it has to answer one
// where it accepts none, and codePointAt is free to say "no code point here".
//
// The pairing rule is 11.1.4 CodePointAt. A leading surrogate followed by a
// trailing one is ONE code point; an unpaired surrogate is its own code unit,
// which covers three cases that all read the same way — a lead at the very end
// of the string, a lead not followed by a trail, and a trail read directly.
// Reading at the second half of a pair therefore answers that half and not the
// pair, which is what makes this the member that walks a string by code point.
uint64_t stringCodePointAt(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self;
    if (!rtStringMethodThis(Value(thisBits), self)) {
        return rtThrowTypeError(
                   "String.prototype.codePointAt called on null or undefined")
            .rawBits();
    }
    // As in charCodeAt: the header is only valid after the index conversion,
    // which may run user code and move the string.
    Rooted<Value> str{self};
    double idx = toInteger(rtToNumber(args.at(0, Value::fromDouble(0.0))));
    const StringHeader* s = str.get().asString<StringHeader>();
    const uint32_t size = s->getLength();
    if (idx < 0 || idx >= static_cast<double>(size)) return Value::fromUndefined().rawBits();
    const uint32_t at = static_cast<uint32_t>(idx);
    const uint32_t first = s->charCodeAt(at);
    if (first < 0xD800 || first > 0xDBFF || at + 1 == size) {
        return Value::fromDouble(first).rawBits();
    }
    const uint32_t second = s->charCodeAt(at + 1);
    if (second < 0xDC00 || second > 0xDFFF) return Value::fromDouble(first).rawBits();
    return Value::fromDouble((first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000).rawBits();
}

extern "C" uint64_t bronze_string_index_of(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError("String.prototype.indexOf called on null or undefined")
            .rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> needle{rtValueToString(args[0])};
    const double posArg = args.count() > 1 ? toInteger(rtToNumber(args[1])) : 0.0;
    const StringHeader* hayHdr = self.get().asString<StringHeader>();
    const StringHeader* needleHdr = needle.get().asString<StringHeader>();
    const uint32_t from = args.count() > 1 ? clampIndex(posArg, hayHdr->getLength()) : 0;
    return Value::fromDouble(static_cast<double>(indexOfHeaders(hayHdr, needleHdr, from)))
        .rawBits();
}

uint64_t stringLastIndexOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError(
                   "String.prototype.lastIndexOf called on null or undefined")
            .rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> needle{rtValueToString(args[0])};
    // 22.1.3.11 steps 4-7: a NaN position (absent included) searches from the
    // end; any other is truncated and clamped to [0, len]. The match found is
    // the largest start at or below that position.
    double pos = std::numeric_limits<double>::infinity();
    if (args.count() > 1) {
        double numPos = rtToNumber(args[1]);
        if (!std::isnan(numPos)) pos = toInteger(numPos);
    }
    const StringHeader* hayHdr = self.get().asString<StringHeader>();
    const StringHeader* needleHdr = needle.get().asString<StringHeader>();
    const uint32_t hayLen = hayHdr->getLength();
    const uint32_t needleLen = needleHdr->getLength();
    if (needleLen > hayLen) return Value::fromDouble(-1.0).rawBits();
    const uint32_t lastStart = hayLen - needleLen;
    uint32_t at = lastStart;
    if (pos <= 0) {
        at = 0;
    } else if (pos < static_cast<double>(lastStart)) {
        at = static_cast<uint32_t>(pos);
    }
    for (;;) {
        if (matchAtHeaders(hayHdr, needleHdr, at)) {
            return Value::fromDouble(static_cast<double>(at)).rawBits();
        }
        if (at == 0) break;
        --at;
    }
    return Value::fromDouble(-1.0).rawBits();
}

uint64_t stringIncludes(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError("String.prototype.includes called on null or undefined")
            .rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> needle{rtValueToString(args[0])};
    const double posArg = args.count() > 1 ? toInteger(rtToNumber(args[1])) : 0.0;
    const StringHeader* hayHdr = self.get().asString<StringHeader>();
    const StringHeader* needleHdr = needle.get().asString<StringHeader>();
    const uint32_t from = args.count() > 1 ? clampIndex(posArg, hayHdr->getLength()) : 0;
    return Value::fromBool(indexOfHeaders(hayHdr, needleHdr, from) >= 0).rawBits();
}

extern "C" uint64_t bronze_string_starts_with(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError(
                   "String.prototype.startsWith called on null or undefined")
            .rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> needle{rtValueToString(args[0])};
    const double posArg = args.count() > 1 ? toInteger(rtToNumber(args[1])) : 0.0;
    const StringHeader* hayHdr = self.get().asString<StringHeader>();
    const StringHeader* needleHdr = needle.get().asString<StringHeader>();
    const uint32_t from = args.count() > 1 ? clampIndex(posArg, hayHdr->getLength()) : 0;
    return Value::fromBool(matchAtHeaders(hayHdr, needleHdr, from)).rawBits();
}

uint64_t stringEndsWith(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value selfVal;
    if (!rtStringMethodThis(Value(thisBits), selfVal)) {
        return rtThrowTypeError("String.prototype.endsWith called on null or undefined")
            .rawBits();
    }
    Rooted<Value> self{selfVal};
    Rooted<Value> needle{rtValueToString(args[0])};
    const bool hasEnd = args.count() > 1 && !args[1].isUndefined();
    const double endArg = hasEnd ? toInteger(rtToNumber(args[1])) : 0.0;
    const StringHeader* hayHdr = self.get().asString<StringHeader>();
    const StringHeader* needleHdr = needle.get().asString<StringHeader>();
    const uint32_t hayLen = hayHdr->getLength();
    const uint32_t needleLen = needleHdr->getLength();
    const uint32_t end = hasEnd ? clampIndex(endArg, hayLen) : hayLen;
    const bool ok = needleLen <= end && matchAtHeaders(hayHdr, needleHdr, end - needleLen);
    return Value::fromBool(ok).rawBits();
}

extern "C" uint64_t bronze_string_slice(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value strVal;
    if (!rtStringMethodThis(Value(thisBits), strVal)) {
        return rtThrowTypeError("String.prototype.slice called on null or undefined").rawBits();
    }
    Rooted<Value> strRoot{strVal};
    double startArg = 0.0;
    if (args.count() > 0) {
        startArg = toInteger(rtToNumber(args[0]));
    }
    bool hasEnd = args.count() > 1 && !args[1].isUndefined();
    double endArg = 0.0;
    if (hasEnd) {
        endArg = toInteger(rtToNumber(args[1]));
    }
    const StringHeader* s = strRoot.get().asString<StringHeader>();
    const uint32_t len = s->getLength();
    uint32_t start = args.count() > 0 ? relativeIndex(startArg, len) : 0;
    uint32_t end = hasEnd ? relativeIndex(endArg, len) : len;
    if (end < start) end = start;
    return directSlice(strRoot, start, end).rawBits();
}

extern "C" uint64_t bronze_string_substring(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value strVal;
    if (!rtStringMethodThis(Value(thisBits), strVal)) {
        return rtThrowTypeError("String.prototype.substring called on null or undefined").rawBits();
    }
    Rooted<Value> strRoot{strVal};
    double startArg = 0.0;
    if (args.count() > 0) {
        startArg = toInteger(rtToNumber(args[0]));
    }
    bool hasEnd = args.count() > 1 && !args[1].isUndefined();
    double endArg = 0.0;
    if (hasEnd) {
        endArg = toInteger(rtToNumber(args[1]));
    }
    const StringHeader* s = strRoot.get().asString<StringHeader>();
    const uint32_t len = s->getLength();
    uint32_t a = args.count() > 0 ? clampIndex(startArg, len) : 0;
    uint32_t b = hasEnd ? clampIndex(endArg, len) : len;
    if (a > b) std::swap(a, b);
    return directSlice(strRoot, a, b).rawBits();
}

// Annex B.2.3.1 String.prototype.substr(start, length)
uint64_t stringSubstr(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value strVal;
    if (!rtStringMethodThis(Value(thisBits), strVal)) {
        return rtThrowTypeError("String.prototype.substr called on null or undefined").rawBits();
    }
    Rooted<Value> strRoot{strVal};
    double startArg = 0.0;
    if (args.count() > 0) {
        startArg = toInteger(rtToNumber(args[0]));
    }
    bool hasLen = args.count() > 1 && !args[1].isUndefined();
    double lenArg = 0.0;
    if (hasLen) {
        lenArg = toInteger(rtToNumber(args[1]));
    }
    const StringHeader* s = strRoot.get().asString<StringHeader>();
    const size_t size = s->getLength();
    double start = startArg;
    if (std::isinf(start) && start < 0.0) {
        start = 0.0;
    } else if (start < 0.0) {
        start = std::max(static_cast<double>(size) + start, 0.0);
    } else {
        start = std::min(start, static_cast<double>(size));
    }
    double len = hasLen ? lenArg : static_cast<double>(size);
    if (len <= 0.0 || std::isnan(len)) return rtMakeString("").rawBits();
    uint32_t intStart = static_cast<uint32_t>(start);
    uint32_t intLength = static_cast<uint32_t>(std::min(len, static_cast<double>(size - intStart)));
    return directSlice(strRoot, intStart, intStart + intLength).rawBits();
}

// ---- fast entries (native_fast.h) -----------------------------------------
//
// Each answers its member for a String receiver and arguments whose
// conversions run no user code -- a position a Number or undefined (whose
// ToIntegerOrInfinity is 0), a search string a String -- with the helpers the
// member uses, and hands every other input to the member. An argument the site
// did not pass arrives as undefined, which each member below reads as it reads
// an absent one.

namespace {

// ToIntegerOrInfinity(ToNumber(v)) when it runs no user code.
bool plainPosition(Value v, double& out) {
    if (v.isNumber()) {
        out = toInteger(v.asNumber());
        return true;
    }
    if (v.isUndefined()) {
        out = 0.0;
        return true;
    }
    return false;
}

}  // namespace

extern "C" uint64_t bronze_fast_string_char_code_at(uint64_t thisBits, uint64_t posBits) {
    const Value self(thisBits);
    double idx = 0.0;
    if (self.isString() && plainPosition(Value(posBits), idx)) {
        const StringHeader* s = self.asString<StringHeader>();
        if (idx < 0 || idx >= static_cast<double>(s->getLength())) {
            return Value::fromDouble(std::numeric_limits<double>::quiet_NaN()).rawBits();
        }
        return Value::fromDouble(s->charCodeAt(static_cast<uint32_t>(idx))).rawBits();
    }
    const uint64_t argv[1] = {posBits};
    return bronze_string_char_code_at(0, thisBits, 1, argv);
}

extern "C" uint64_t bronze_fast_string_char_at(uint64_t thisBits, uint64_t posBits) {
    const Value self(thisBits);
    double idx = 0.0;
    if (self.isString() && plainPosition(Value(posBits), idx)) {
        const uint32_t len = self.asString<StringHeader>()->getLength();
        if (idx < 0 || idx >= static_cast<double>(len)) return rtMakeString("").rawBits();
        Rooted<Value> str{self};
        const auto i = static_cast<uint32_t>(idx);
        return directSlice(str, i, i + 1).rawBits();
    }
    const uint64_t argv[1] = {posBits};
    return bronze_string_char_at(0, thisBits, 1, argv);
}

extern "C" uint64_t bronze_fast_string_index_of(uint64_t thisBits, uint64_t searchBits, uint64_t posBits) {
    const Value self(thisBits);
    const Value search(searchBits);
    double pos = 0.0;
    if (self.isString() && search.isString() && plainPosition(Value(posBits), pos)) {
        const StringHeader* hay = self.asString<StringHeader>();
        const uint32_t from = clampIndex(pos, hay->getLength());
        const int64_t found = indexOfHeaders(hay, search.asString<StringHeader>(), from);
        return Value::fromDouble(static_cast<double>(found)).rawBits();
    }
    const uint64_t argv[2] = {searchBits, posBits};
    return bronze_string_index_of(0, thisBits, 2, argv);
}

extern "C" uint64_t bronze_fast_string_starts_with(uint64_t thisBits, uint64_t searchBits, uint64_t posBits) {
    const Value self(thisBits);
    const Value search(searchBits);
    double pos = 0.0;
    if (self.isString() && search.isString() && plainPosition(Value(posBits), pos)) {
        const StringHeader* hay = self.asString<StringHeader>();
        const uint32_t from = clampIndex(pos, hay->getLength());
        return Value::fromBool(matchAtHeaders(hay, search.asString<StringHeader>(), from)).rawBits();
    }
    const uint64_t argv[2] = {searchBits, posBits};
    return bronze_string_starts_with(0, thisBits, 2, argv);
}

extern "C" uint64_t bronze_fast_string_slice(uint64_t thisBits, uint64_t startBits, uint64_t endBits) {
    const Value self(thisBits);
    const Value endArg(endBits);
    double startPos = 0.0;
    if (self.isString() && plainPosition(Value(startBits), startPos) &&
        (endArg.isNumber() || endArg.isUndefined())) {
        const uint32_t len = self.asString<StringHeader>()->getLength();
        const uint32_t start = relativeIndex(startPos, len);
        uint32_t end = endArg.isUndefined() ? len : relativeIndex(toInteger(endArg.asNumber()), len);
        if (end < start) end = start;
        Rooted<Value> str{self};
        return directSlice(str, start, end).rawBits();
    }
    const uint64_t argv[2] = {startBits, endBits};
    return bronze_string_slice(0, thisBits, 2, argv);
}

extern "C" uint64_t bronze_fast_string_substring(uint64_t thisBits, uint64_t startBits, uint64_t endBits) {
    const Value self(thisBits);
    const Value endArg(endBits);
    double startPos = 0.0;
    if (self.isString() && plainPosition(Value(startBits), startPos) &&
        (endArg.isNumber() || endArg.isUndefined())) {
        const uint32_t len = self.asString<StringHeader>()->getLength();
        uint32_t a = clampIndex(startPos, len);
        uint32_t b = endArg.isUndefined() ? len : clampIndex(toInteger(endArg.asNumber()), len);
        if (a > b) std::swap(a, b);
        Rooted<Value> str{self};
        return directSlice(str, a, b).rawBits();
    }
    const uint64_t argv[2] = {startBits, endBits};
    return bronze_string_substring(0, thisBits, 2, argv);
}

}  // namespace bronze::runtime::string_detail
