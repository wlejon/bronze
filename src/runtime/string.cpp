#include "runtime/string.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_set>

namespace bronze {

namespace {

// A string's units, as bytes, for the arena's intern table. Two strings with
// the same text in different encodings hash apart and simply are not shared.
size_t unitBytes(const StringHeader* s) {
    return s->isUTF16() ? static_cast<size_t>(s->length) * sizeof(uint16_t) : s->length;
}

struct ArenaStringHash {
    size_t operator()(const StringHeader* s) const noexcept {
        const auto* p = reinterpret_cast<const unsigned char*>(s + 1);
        uint64_t h = 1469598103934665603ull ^ (s->isUTF16() ? 1u : 0u);
        for (size_t i = 0, n = unitBytes(s); i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

struct ArenaStringEq {
    bool operator()(const StringHeader* a, const StringHeader* b) const noexcept {
        return a->isUTF16() == b->isUTF16() && a->length == b->length &&
               std::memcmp(a + 1, b + 1, unitBytes(a)) == 0;
    }
};

struct ArenaStringTable {
    std::unordered_set<const StringHeader*, ArenaStringHash, ArenaStringEq> strings;
};

// One step of the WHATWG UTF-8 decoder (Encoding Standard 4.3, Unicode's
// maximal subparts): the code point at s[i] and the bytes it takes. A lead
// byte that cannot start a sequence (a continuation byte, C0/C1, F5..FF) is
// U+FFFD; so is a sequence cut short or broken by a byte out of range, for
// the lead and the continuation bytes valid so far, and the byte that broke
// it starts the next step (D7 34 is U+FFFD then '4'). The first
// continuation's range after E0, ED, F0 and F4 refuses overlongs, surrogates
// and anything past U+10FFFF. With `surrogates` (WTF-8, the runtime's own
// UTF-8 for a string, which a lone surrogate round-trips through), ED takes
// A0..BF as well: U+D800..U+DFFF decode.
uint32_t utf8Step(const unsigned char* s, uint32_t n, uint32_t i, uint32_t& cp, bool surrogates) {
    const uint32_t b0 = s[i];
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    }
    uint32_t need = 0, lower = 0x80, upper = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        cp = b0 & 0x0F;
        if (b0 == 0xE0) lower = 0xA0;
        else if (b0 == 0xED && !surrogates) upper = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        cp = b0 & 0x07;
        if (b0 == 0xF0) lower = 0x90;
        else if (b0 == 0xF4) upper = 0x8F;
    } else {
        cp = 0xFFFD;
        return 1;
    }
    for (uint32_t k = 1; k <= need; ++k) {
        if (i + k >= n || s[i + k] < lower || s[i + k] > upper) {
            cp = 0xFFFD;
            return k;
        }
        cp = (cp << 6) | (s[i + k] & 0x3F);
        lower = 0x80;
        upper = 0xBF;
    }
    return need + 1;
}

// UTF-8 into a string: Latin-1 when every code point fits, else UTF-16
// (surrogate pairs past U+FFFF). `latin1(n)` / `utf16(n)` allocate n units.
template <class MakeLatin1, class MakeUTF16>
StringHeader* decodeUTF8(const char* str, uint32_t n, bool surrogates, MakeLatin1 latin1, MakeUTF16 utf16) {
    const auto* s = reinterpret_cast<const unsigned char*>(str);
    bool isLatin1 = true;
    uint32_t units = 0;
    for (uint32_t i = 0, cp = 0; i < n;) {
        i += utf8Step(s, n, i, cp, surrogates);
        units += cp >= 0x10000 ? 2 : 1;
        if (cp > 0xFF) isLatin1 = false;
    }
    if (isLatin1) {
        StringHeader* h = latin1(units);
        char* dst = h->latin1Data();
        uint32_t o = 0;
        for (uint32_t i = 0, cp = 0; i < n;) {
            i += utf8Step(s, n, i, cp, surrogates);
            dst[o++] = static_cast<char>(cp);
        }
        dst[units] = '\0';
        return h;
    }
    StringHeader* h = utf16(units);
    uint16_t* dst = h->utf16Data();
    uint32_t o = 0;
    for (uint32_t i = 0, cp = 0; i < n;) {
        i += utf8Step(s, n, i, cp, surrogates);
        if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            dst[o++] = static_cast<uint16_t>(0xD800 + (v >> 10));
            dst[o++] = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
        } else {
            dst[o++] = static_cast<uint16_t>(cp);
        }
    }
    dst[units] = 0;
    return h;
}

}  // namespace

StringHeader* StringHeader::createLatin1(Heap& heap, const char* str, uint32_t len) {
    size_t payload_size = sizeof(uint32_t) + sizeof(uint32_t) + len + 1;
    HeapObjectHeader* h = heap.allocate(payload_size, Tag::String);
    auto* s = reinterpret_cast<StringHeader*>(h);
    s->length = len;
    s->flags = 0;
    if (str && len > 0) {
        std::memcpy(s->latin1Data(), str, len);
    }
    s->latin1Data()[len] = '\0';
    return s;
}

StringHeader* StringHeader::createUTF16(Heap& heap, const uint16_t* str, uint32_t len) {
    size_t payload_size = sizeof(uint32_t) + sizeof(uint32_t) + (static_cast<size_t>(len) * sizeof(uint16_t)) + sizeof(uint16_t);
    HeapObjectHeader* h = heap.allocate(payload_size, Tag::String);
    auto* s = reinterpret_cast<StringHeader*>(h);
    s->length = len;
    s->flags = kUTF16Flag;
    if (str && len > 0) {
        std::memcpy(s->utf16Data(), str, len * sizeof(uint16_t));
    }
    s->utf16Data()[len] = 0;
    return s;
}

StringHeader* StringHeader::createFromUTF8(Heap& heap, const char* str, uint32_t utf8_len) {
    if (!str || utf8_len == 0) {
        return createLatin1(heap, "", 0);
    }
    // ASCII is its own Latin-1: one scan and a copy, not two decoding
    // passes (an Error's stack text is a few hundred ASCII bytes).
    bool ascii = true;
    for (uint32_t i = 0; i < utf8_len; ++i) {
        if (static_cast<unsigned char>(str[i]) >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) return createLatin1(heap, str, utf8_len);
    return decodeUTF8(
        str, utf8_len, /*surrogates=*/true, [&](uint32_t n) { return createLatin1(heap, nullptr, n); },
        [&](uint32_t n) { return createUTF16(heap, nullptr, n); });
}

StringHeader* StringHeader::createFromUTF8(Heap& heap, std::string_view sv) {
    return createFromUTF8(heap, sv.data(), static_cast<uint32_t>(sv.size()));
}

StringHeader* StringHeader::createFromHostUTF8(Heap& heap, std::string_view sv) {
    if (sv.empty()) return createLatin1(heap, "", 0);
    return decodeUTF8(
        sv.data(), static_cast<uint32_t>(sv.size()), /*surrogates=*/false,
        [&](uint32_t n) { return createLatin1(heap, nullptr, n); },
        [&](uint32_t n) { return createUTF16(heap, nullptr, n); });
}

uint16_t StringHeader::charCodeAt(uint32_t index) const noexcept {
    if (index >= length) {
        return 0;
    }
    if (isLatin1()) {
        return static_cast<uint8_t>(latin1Data()[index]);
    } else {
        return utf16Data()[index];
    }
}

bool StringHeader::equals(const StringHeader& other) const noexcept {
    if (this == &other) {
        return true;
    }
    if (length != other.length) {
        return false;
    }
    if (isLatin1() && other.isLatin1()) {
        return std::memcmp(latin1Data(), other.latin1Data(), length) == 0;
    }
    if (isUTF16() && other.isUTF16()) {
        return std::memcmp(utf16Data(), other.utf16Data(), length * sizeof(uint16_t)) == 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (charCodeAt(i) != other.charCodeAt(i)) {
            return false;
        }
    }
    return true;
}

bool StringHeader::lessThan(const StringHeader& other) const noexcept {
    // charCodeAt on both sides rather than a memcmp on the latin1 pair: a
    // latin1 byte at or above 0x80 is a code unit above 0x7F, and `char` is
    // signed on this target, so memcmp would order "é" below "a".
    const uint32_t shared = length < other.length ? length : other.length;
    for (uint32_t i = 0; i < shared; ++i) {
        const uint16_t a = charCodeAt(i);
        const uint16_t b = other.charCodeAt(i);
        if (a != b) return a < b;
    }
    // 7.2.13 step 3: a prefix is less than what extends it, and equal strings
    // are not less than each other — which is what makes `<=` differ from `<`.
    return length < other.length;
}

uint32_t StringHeader::hash() const noexcept {
    if ((flags & kHasHashFlag) != 0) {
        return flags & 0xFFFFFFFCU;
    }

    uint32_t h = 0x811c9dc5U;
    for (uint32_t i = 0; i < length; ++i) {
        uint16_t u = charCodeAt(i);
        h ^= (u & 0xFF);
        h *= 0x01000193U;
        h ^= (u >> 8);
        h *= 0x01000193U;
    }

    uint32_t cached_hash = h & 0xFFFFFFFCU;
    flags = cached_hash | kHasHashFlag | (flags & kUTF16Flag);
    return cached_hash;
}

Value StringHeader::concat(Heap& heap, Rooted<Value>& a, Rooted<Value>& b) {
    if (!a.get().isString() || !b.get().isString()) {
        std::cerr << "Hard runtime error: Invalid string concat arguments" << std::endl;
        std::abort();
    }

    const StringHeader* sA = a.get().asString<StringHeader>();
    const StringHeader* sB = b.get().asString<StringHeader>();

    uint32_t lenA = sA->length;
    uint32_t lenB = sB->length;
    uint32_t totalLen = lenA + lenB;

    bool bothLatin1 = sA->isLatin1() && sB->isLatin1();

    if (bothLatin1) {
        StringHeader* res = createLatin1(heap, nullptr, totalLen);
        sA = a.get().asString<StringHeader>();
        sB = b.get().asString<StringHeader>();

        char* dst = res->latin1Data();
        if (lenA > 0) std::memcpy(dst, sA->latin1Data(), lenA);
        if (lenB > 0) std::memcpy(dst + lenA, sB->latin1Data(), lenB);
        dst[totalLen] = '\0';
        return Value::fromString(res);
    } else {
        StringHeader* res = createUTF16(heap, nullptr, totalLen);
        sA = a.get().asString<StringHeader>();
        sB = b.get().asString<StringHeader>();

        uint16_t* dst = res->utf16Data();
        for (uint32_t i = 0; i < lenA; ++i) {
            dst[i] = sA->charCodeAt(i);
        }
        for (uint32_t i = 0; i < lenB; ++i) {
            dst[lenA + i] = sB->charCodeAt(i);
        }
        dst[totalLen] = 0;
        return Value::fromString(res);
    }
}

uint32_t StringHeader::capacity() const noexcept {
    // The heap header's `size` is what the collector copies and what the heap
    // walk steps by, so it is the only honest answer to "where does this
    // allocation end" — and asking it here is what keeps the appender and the
    // collector from ever disagreeing about that boundary.
    const size_t avail = header.size - sizeof(HeapObjectHeader) - 2 * sizeof(uint32_t);
    const size_t units = isUTF16() ? avail / sizeof(uint16_t) : avail;
    // One unit is the terminator, which is not capacity.
    return static_cast<uint32_t>(units - 1);
}

StringHeader* StringHeader::createBuilder(Heap& heap, bool utf16, uint32_t len, uint32_t cap) {
    if (cap < len) cap = len;
    const size_t unit = utf16 ? sizeof(uint16_t) : 1;
    const size_t payload_size =
        sizeof(uint32_t) + sizeof(uint32_t) + (static_cast<size_t>(cap) + 1) * unit;
    HeapObjectHeader* h = heap.allocate(payload_size, Tag::String);
    auto* s = reinterpret_cast<StringHeader*>(h);
    s->header.flags = kBuilderFlag;
    s->length = len;
    s->flags = utf16 ? kUTF16Flag : 0;
    if (utf16) {
        s->utf16Data()[len] = 0;
    } else {
        s->latin1Data()[len] = '\0';
    }
    return s;
}

// Copy `src`'s units into `dst` starting at `at`, widening when the
// destination is UTF-16 and the source is not. Neither allocates, so neither
// operand can move underneath it.
static void copyUnitsInto(StringHeader* dst, uint32_t at, const StringHeader* src) {
    const uint32_t n = src->length;
    if (n == 0) return;
    if (!dst->isUTF16()) {
        std::memcpy(dst->latin1Data() + at, src->latin1Data(), n);
        return;
    }
    uint16_t* out = dst->utf16Data() + at;
    if (src->isUTF16()) {
        std::memcpy(out, src->utf16Data(), static_cast<size_t>(n) * sizeof(uint16_t));
        return;
    }
    const unsigned char* in = reinterpret_cast<const unsigned char*>(src->latin1Data());
    for (uint32_t i = 0; i < n; ++i) out[i] = in[i];
}

void StringHeader::startBuilder(Heap& heap, Rooted<Value>& s, uint32_t cap) {
    const StringHeader* src = s.get().asString<StringHeader>();
    const uint32_t len = src->length;
    const bool utf16 = src->isUTF16();
    if (cap < len) cap = len;
    StringHeader* b = createBuilder(heap, utf16, len, cap);
    // The mint allocated, so the source may have moved; the root is the only
    // pointer to it that survived.
    src = s.get().asString<StringHeader>();
    copyUnitsInto(b, 0, src);
    s.set(Value::fromString(b));
}

void StringHeader::appendToBuilder(Heap& heap, Rooted<Value>& dst, Rooted<Value>& src) {
    StringHeader* b = dst.get().asString<StringHeader>();
    const StringHeader* s = src.get().asString<StringHeader>();
    const uint32_t addLen = s->length;
    if (addLen == 0) return;

    const uint32_t oldLen = b->length;
    if (addLen > UINT32_MAX - oldLen) {
        std::cerr << "Hard runtime error: string concatenation length overflows 2^32"
                  << std::endl;
        std::abort();
    }
    const uint32_t newLen = oldLen + addLen;
    const bool needUTF16 = b->isUTF16() || s->isUTF16();

    if (needUTF16 == b->isUTF16() && b->capacity() >= newLen) {
        copyUnitsInto(b, oldLen, s);
        b->length = newLen;
        if (needUTF16) {
            b->utf16Data()[newLen] = 0;
        } else {
            b->latin1Data()[newLen] = '\0';
        }
        // The text changed, so any hash cached over the old text is a lie.
        // `flags` holds the hash from bit 2 up, so clearing it back to the
        // encoding bit is what un-caches it.
        b->flags &= kUTF16Flag;
        return;
    }

    // No room, or a Latin-1 builder that has just met a UTF-16 piece. Either
    // way the text moves, and the new allocation grows by half again on top of
    // what this append needs, so a chain that keeps outrunning its hint still
    // reallocates a logarithmic number of times rather than once per operand.
    uint32_t want = newLen;
    const uint32_t grown = b->capacity() + b->capacity() / 2;
    if (grown > want) want = grown;
    StringHeader* nb = createBuilder(heap, needUTF16, newLen, want);
    // `createBuilder` allocated, so both operands may have moved. Nothing
    // below allocates, so `nb` needs no root of its own.
    b = dst.get().asString<StringHeader>();
    s = src.get().asString<StringHeader>();
    copyUnitsInto(nb, 0, b);
    copyUnitsInto(nb, oldLen, s);
    if (needUTF16) {
        nb->utf16Data()[newLen] = 0;
    } else {
        nb->latin1Data()[newLen] = '\0';
    }
    dst.set(Value::fromString(nb));
}

// One copy per distinct content for the life of the arena. It used to be a
// copy per call, and its callers call it on hot paths — every shape transition
// taken (Shape::addProperty copies the key before it looks for the edge), every
// named host function made — so a program that kept building objects kept
// growing the immortal arena: a UI re-rendering its views gained tens of
// megabytes a minute of property names it already had. A shared copy is sound
// because an arena string is immutable (sealed, never a builder) and every
// consumer matches string keys by content.
StringHeader* StringHeader::internToArena(NonMovingArena& arena, const StringHeader* src) {
    auto* table = static_cast<ArenaStringTable*>(arena.string_table());
    if (table == nullptr) {
        table = arena.create<ArenaStringTable>();
        arena.set_string_table(table);
    }
    auto found = table->strings.find(src);
    if (found != table->strings.end()) return const_cast<StringHeader*>(*found);
    const size_t total = sizeof(StringHeader) + unitBytes(src);
    void* mem = arena.allocate(total, alignof(StringHeader));
    std::memcpy(mem, src, total);
    auto* out = static_cast<StringHeader*>(mem);
    // The copy is exact-fit and immortal, so whatever slack the source had is
    // gone and the builder mark must not survive into it: an arena string that
    // still claimed to be an appendable builder would offer room that this
    // allocation does not have.
    out->sealBuilder();
    table->strings.insert(out);
    return out;
}

StringHeader* StringHeader::createLatin1InArena(NonMovingArena& arena, const char* str, uint32_t len) {
    size_t total = sizeof(StringHeader) + len + 1;
    void* mem = arena.allocate(total, alignof(StringHeader));
    auto* hdr = static_cast<StringHeader*>(mem);
    hdr->header.tag = static_cast<uint16_t>(Tag::String);
    hdr->header.flags = 0;
    hdr->header.size = static_cast<uint32_t>(total);
    hdr->length = len;
    hdr->flags = 0;
    if (str && len > 0) {
        std::memcpy(hdr + 1, str, len);
    }
    hdr->latin1Data()[len] = '\0';
    return hdr;
}

StringHeader* StringHeader::createUTF16InArena(NonMovingArena& arena, const uint16_t* str, uint32_t len) {
    size_t payload_size = sizeof(uint32_t) + sizeof(uint32_t) + (static_cast<size_t>(len) * sizeof(uint16_t)) + sizeof(uint16_t);
    size_t total = sizeof(HeapObjectHeader) + payload_size;
    void* mem = arena.allocate(total, alignof(StringHeader));
    auto* hdr = static_cast<StringHeader*>(mem);
    hdr->header.tag = static_cast<uint16_t>(Tag::String);
    hdr->header.flags = 0;
    hdr->header.size = static_cast<uint32_t>(total);
    hdr->length = len;
    hdr->flags = kUTF16Flag;
    if (str && len > 0) {
        std::memcpy(hdr->utf16Data(), str, len * sizeof(uint16_t));
    }
    hdr->utf16Data()[len] = 0;
    return hdr;
}

StringHeader* StringHeader::createFromUTF8InArena(NonMovingArena& arena, const char* str, uint32_t utf8_len) {
    if (!str || utf8_len == 0) {
        return createLatin1InArena(arena, "", 0);
    }
    return decodeUTF8(
        str, utf8_len, /*surrogates=*/true, [&](uint32_t n) { return createLatin1InArena(arena, nullptr, n); },
        [&](uint32_t n) { return createUTF16InArena(arena, nullptr, n); });
}

StringHeader* StringHeader::createFromUTF8InArena(NonMovingArena& arena, std::string_view sv) {
    return createFromUTF8InArena(arena, sv.data(), static_cast<uint32_t>(sv.size()));
}

}  // namespace bronze
