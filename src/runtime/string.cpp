#include "runtime/string.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace bronze {

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

    bool is_latin1 = true;
    uint32_t code_units = 0;
    uint32_t i = 0;

    while (i < utf8_len) {
        uint8_t b0 = static_cast<uint8_t>(str[i]);
        if (b0 < 0x80) {
            code_units++;
            i++;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 < utf8_len) {
                uint8_t b1 = static_cast<uint8_t>(str[i + 1]);
                uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
                if (cp > 0xFF) {
                    is_latin1 = false;
                }
                code_units++;
                i += 2;
            } else {
                is_latin1 = false;
                code_units++;
                i++;
            }
        } else if ((b0 & 0xF0) == 0xE0) {
            is_latin1 = false;
            code_units++;
            if (i + 2 < utf8_len) {
                i += 3;
            } else {
                i++;
            }
        } else if ((b0 & 0xF8) == 0xF0) {
            is_latin1 = false;
            code_units += 2;
            if (i + 3 < utf8_len) {
                i += 4;
            } else {
                i++;
            }
        } else {
            is_latin1 = false;
            code_units++;
            i++;
        }
    }

    if (is_latin1) {
        StringHeader* s = createLatin1(heap, nullptr, code_units);
        char* dst = s->latin1Data();
        uint32_t out_idx = 0;
        i = 0;
        while (i < utf8_len) {
            uint8_t b0 = static_cast<uint8_t>(str[i]);
            if (b0 < 0x80) {
                dst[out_idx++] = static_cast<char>(b0);
                i++;
            } else if ((b0 & 0xE0) == 0xC0 && i + 1 < utf8_len) {
                uint8_t b1 = static_cast<uint8_t>(str[i + 1]);
                uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
                dst[out_idx++] = static_cast<char>(cp & 0xFF);
                i += 2;
            } else {
                dst[out_idx++] = str[i++];
            }
        }
        dst[code_units] = '\0';
        return s;
    } else {
        StringHeader* s = createUTF16(heap, nullptr, code_units);
        uint16_t* dst = s->utf16Data();
        uint32_t out_idx = 0;
        i = 0;
        while (i < utf8_len) {
            uint8_t b0 = static_cast<uint8_t>(str[i]);
            if (b0 < 0x80) {
                dst[out_idx++] = static_cast<uint16_t>(b0);
                i++;
            } else if ((b0 & 0xE0) == 0xC0) {
                if (i + 1 < utf8_len) {
                    uint8_t b1 = static_cast<uint8_t>(str[i + 1]);
                    uint32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
                    dst[out_idx++] = static_cast<uint16_t>(cp);
                    i += 2;
                } else {
                    dst[out_idx++] = 0xFFFD;
                    i++;
                }
            } else if ((b0 & 0xF0) == 0xE0) {
                if (i + 2 < utf8_len) {
                    uint8_t b1 = static_cast<uint8_t>(str[i + 1]);
                    uint8_t b2 = static_cast<uint8_t>(str[i + 2]);
                    uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    dst[out_idx++] = static_cast<uint16_t>(cp);
                    i += 3;
                } else {
                    dst[out_idx++] = 0xFFFD;
                    i++;
                }
            } else if ((b0 & 0xF8) == 0xF0) {
                if (i + 3 < utf8_len) {
                    uint8_t b1 = static_cast<uint8_t>(str[i + 1]);
                    uint8_t b2 = static_cast<uint8_t>(str[i + 2]);
                    uint8_t b3 = static_cast<uint8_t>(str[i + 3]);
                    uint32_t cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                    if (cp >= 0x10000) {
                        uint32_t v = cp - 0x10000;
                        dst[out_idx++] = static_cast<uint16_t>(0xD800 + (v >> 10));
                        dst[out_idx++] = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
                    } else {
                        dst[out_idx++] = static_cast<uint16_t>(cp);
                    }
                    i += 4;
                } else {
                    dst[out_idx++] = 0xFFFD;
                    i++;
                }
            } else {
                dst[out_idx++] = 0xFFFD;
                i++;
            }
        }
        dst[code_units] = 0;
        return s;
    }
}

StringHeader* StringHeader::createFromUTF8(Heap& heap, std::string_view sv) {
    return createFromUTF8(heap, sv.data(), static_cast<uint32_t>(sv.size()));
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

StringHeader* StringHeader::internToArena(NonMovingArena& arena, const StringHeader* src) {
    size_t data_bytes =
        src->isUTF16() ? static_cast<size_t>(src->length) * sizeof(uint16_t) : src->length;
    size_t total = sizeof(StringHeader) + data_bytes;
    void* mem = arena.allocate(total, alignof(StringHeader));
    std::memcpy(mem, src, total);
    return static_cast<StringHeader*>(mem);
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
    std::memcpy(hdr + 1, str, len);
    hdr->latin1Data()[len] = '\0';
    return hdr;
}

}  // namespace bronze
