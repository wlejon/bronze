#pragma once

// The tags that tell apart the abstract objects one instruction can make
// (heap.h, AObj::tag). Private to the solver's translation units.

#include <cstdint>

namespace bronze::wpo {

inline constexpr uint32_t kTagMain = 0;      // the object the instruction itself makes
inline constexpr uint32_t kTagProto = 1;     // a constructible function's `prototype`
inline constexpr uint32_t kTagIter = 2;      // an iteration record
inline constexpr uint32_t kTagResult = 3;    // the object a builtin call returns
inline constexpr uint32_t kTagPair = 4;      // the [key, value] arrays an entries() iterator yields
inline constexpr uint32_t kTagBound = 5;     // a bound function
inline constexpr uint32_t kTagCoro = 6;      // a generator object / an async call's promise
// `new F` at one site makes one object per constructor F: the tag is this plus
// F's function index.
inline constexpr uint32_t kTagConstruct = 0x100000;
// The rest array of a function, one per function: its site is the function
// index and its tag this.
inline constexpr uint32_t kTagRest = 0x7FFFFFFF;

}  // namespace bronze::wpo
