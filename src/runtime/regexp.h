#pragma once

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// A RegExp object. Everything the collector must see is a Value in the payload,
// so the generic payload scan forwards it and this file owns no root source.
//
// The compiled pattern is deliberately NOT here. It is a C++ object with a
// tree of `unique_ptr`s in it, which the moving collector must never relocate
// and must never scan as Values; what the header carries instead is its INDEX
// in a runtime-owned table, as a plain number. That also makes two regular
// expressions with the same source and flags share one compilation, which is
// what keeps a literal inside a loop from compiling its pattern per iteration.
struct RegExpHeader {
    HeapObjectHeader header;
    Value source;     // string: the pattern text, exactly as written
    Value flagsText;  // string: the flags in 22.2.6.5's order
    // `lastIndex`, the one mutable property a RegExp has (22.2.6.9). A double,
    // because a program may assign any number to it and 22.2.7.2 reads it back
    // through ToLength.
    Value lastIndex;
    Value programIndex;  // double: index into the compiled-pattern table

    static constexpr uint16_t kFlags = 8;
};

}  // namespace bronze
