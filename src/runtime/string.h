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

    // The `flags` word above has no spare bit: everything from bit 2 up is the
    // cached hash (`hash()` stores it there). The builder mark therefore lives
    // in the HEAP header's own flags word, which for a `Tag::String` is the one
    // field nothing else writes — `Heap::allocate` zeroes it and only a
    // `Tag::Object` ever spends it, on a HeapKind.
    static constexpr uint16_t kBuilderFlag = 1U << 0;

    static StringHeader* createLatin1(Heap& heap, const char* str, uint32_t len);
    static StringHeader* createUTF16(Heap& heap, const uint16_t* str, uint32_t len);
    static StringHeader* createFromUTF8(Heap& heap, const char* str, uint32_t utf8_len);
    static StringHeader* createFromUTF8(Heap& heap, std::string_view sv);
    // Immortal, non-moving copy for consumers that must never point into
    // the movable heap (shape keys, the compiled key-constant table).
    static StringHeader* internToArena(NonMovingArena& arena, const StringHeader* src);
    static StringHeader* createLatin1InArena(NonMovingArena& arena, const char* str, uint32_t len);

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

    // ---- the `+`-chain accumulator (bronze_concat_* in the ABI registry) ----
    //
    // A builder is an ordinary string that was allocated with room past its
    // `length` and carries `kBuilderFlag`. Nothing else in the runtime knows
    // that it exists: every reader goes through `length`, which is always the
    // text actually written, so a builder answers `.length`, compares, hashes
    // and prints exactly as the string it currently is. That is what makes it
    // safe for one to escape a chain that threw — it is a finished string with
    // slack, not a half-built object.
    bool isBuilder() const noexcept { return (header.flags & kBuilderFlag) != 0; }
    void sealBuilder() noexcept {
        header.flags = static_cast<uint16_t>(header.flags & ~kBuilderFlag);
    }

    // How many code units this allocation can hold, NOT counting the
    // terminator every reader of `latin1Data()` may rely on. Derived from the
    // heap header's size rather than stored, so a builder needs no field a
    // plain string does not have — and so the collector, which copies by that
    // same size, cannot disagree with the appender about where the room ends.
    uint32_t capacity() const noexcept;

    // A string of `len` units with room for `cap` (cap >= len), marked. The
    // units past `len` are uninitialized; the terminator at `len` is written.
    static StringHeader* createBuilder(Heap& heap, bool utf16, uint32_t len, uint32_t cap);

    // Replace the string `s` holds with a marked builder carrying the same
    // text and room for `cap` units. `s` is a root because minting collects.
    static void startBuilder(Heap& heap, Rooted<Value>& s, uint32_t cap);

    // Append `src` to the builder `dst` holds, in place when it fits and by
    // reallocating when it does not, and store the result back into `dst`.
    // Both are roots because either path can collect. `dst` must hold a marked
    // builder; `src` must hold a string.
    static void appendToBuilder(Heap& heap, Rooted<Value>& dst, Rooted<Value>& src);
};

}  // namespace bronze
