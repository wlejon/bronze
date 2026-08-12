#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

struct StringHeader {
    HeapObjectHeader header;
    uint32_t length;
    mutable uint32_t flags;

    static constexpr uint32_t kUTF16Flag = 1U << 0;
    static constexpr uint32_t kHasHashFlag = 1U << 1;

    static StringHeader* createLatin1(Heap& heap, const char* str, uint32_t len);
    static StringHeader* createUTF16(Heap& heap, const uint16_t* str, uint32_t len);
    static StringHeader* createFromUTF8(Heap& heap, const char* str, uint32_t utf8_len);
    static StringHeader* createFromUTF8(Heap& heap, std::string_view sv);
    // Immortal, non-moving copy for consumers that must never point into
    // the movable heap (shape keys, the compiled key-constant table).
    static StringHeader* internToArena(NonMovingArena& arena, const StringHeader* src);

    bool isLatin1() const noexcept { return (flags & kUTF16Flag) == 0; }
    bool isUTF16() const noexcept { return (flags & kUTF16Flag) != 0; }

    const char* latin1Data() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }
    char* latin1Data() noexcept {
        return reinterpret_cast<char*>(this + 1);
    }

    const uint16_t* utf16Data() const noexcept {
        return reinterpret_cast<const uint16_t*>(this + 1);
    }
    uint16_t* utf16Data() noexcept {
        return reinterpret_cast<uint16_t*>(this + 1);
    }

    uint32_t getLength() const noexcept { return length; }

    uint16_t charAt(uint32_t index) const noexcept { return charCodeAt(index); }
    uint16_t charCodeAt(uint32_t index) const noexcept;

    bool equals(const StringHeader& other) const noexcept;
    // ECMA-262 7.2.13 IsStringLessThan: the two strings are compared UTF-16
    // CODE UNIT by code unit, and a prefix is less than what extends it. Not a
    // collation and never to become one — `"Z" < "a"` is true because 0x5A
    // precedes 0x61, and a locale would answer otherwise; deterministic output
    // is a house rule, and `localeCompare` stays unimplemented because of it.
    bool lessThan(const StringHeader& other) const noexcept;
    uint32_t hash() const noexcept;

    static Value concat(Heap& heap, Rooted<Value>& a, Rooted<Value>& b);
};

}  // namespace bronze
