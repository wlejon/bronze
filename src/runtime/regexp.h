#pragma once

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace bronze {

// A RegExp object (ECMA-262 22.2): an ordinary object with internal slots.
//
// It opens with the ordinary-object prefix — the shape, the overflow word and
// the inline slots — for the reason a typed array's header does (typed_array.h):
// the shape carries the [[Prototype]], so `RegExp.prototype` is found by the
// walk every other object takes, an expando lands in a slot, and a subclass
// instance is this same header with NewTarget's shape. The four Values after
// the prefix are the internal slots 22.2 names, and every one of them is a
// Value so the generic payload scan forwards it; the layout is exactly what
// `ObjectHeader::createWithInternalSlots(…, 4)` allocates.
//
// The compiled pattern is deliberately NOT here. It is a C++ object with a
// tree of `unique_ptr`s in it, which the moving collector must never relocate
// and must never scan as Values; what the header carries instead is its INDEX
// in a runtime-owned table, as a plain number. That also makes two regular
// expressions with the same source and flags share one compilation, which is
// what keeps a literal inside a loop from compiling its pattern per iteration.
struct RegExpHeader {
    ObjectHeader object;
    Value inlineSlots[ObjectHeader::kInlineSlots];
    Value source;     // string: the pattern text, exactly as written
    Value flagsText;  // string: the flags in 22.2.6.4's order
    // `lastIndex` (22.2.4.1): the one own DATA property every RegExp is created
    // with, kept in the header rather than in a slot so the matcher reads and
    // writes it without a property lookup. The property paths synthesise it
    // as own, writable, non-enumerable and non-configurable. It holds whatever
    // the program last assigned — a string stays a string — and 22.2.7.2 reads
    // it back through ToLength when a match starts.
    Value lastIndex;
    Value programIndex;  // double: index into the compiled-pattern table

    static constexpr uint16_t kFlags = HeapKind::RegExp;
    static constexpr uint32_t kInternalSlots = 4;
};

static_assert(sizeof(RegExpHeader) ==
                  sizeof(ObjectHeader) +
                      (ObjectHeader::kInlineSlots + RegExpHeader::kInternalSlots) * sizeof(Value),
              "a RegExp is an ordinary object with exactly four internal slots");

}  // namespace bronze
