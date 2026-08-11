#pragma once

#include <string>
#include <string_view>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The runtime's process-wide state — the heap, the non-moving arena, the
// root shapes and the collector's root sources — is owned by ONE translation
// unit (rt_helpers.cpp), so its construction order is that unit's business
// alone. Builtin families (builtin_math.cpp and the ones that follow it)
// reach it through the accessors below rather than declaring statics of
// their own, which would put the collector's roots at the mercy of cross-TU
// initialization order.
//
// Nothing here is part of the generated-code ABI: that is bronze_abi.h, and
// it stays pure C. This header is C++ and internal to src/runtime.

namespace bronze::runtime {

Heap& rtHeap();
NonMovingArena& rtArena();

// A root shape registered with the collector, for a builtin that needs its
// own hidden class rather than the one every `{}` literal shares.
Shape* rtNewRootShape(Value proto);

// A heap string from UTF-8 bytes, and JS ToString / ToNumber. ToString on
// an object and ToNumber on an object are hard errors: both need
// ToPrimitive, which bronze has not built.
Value rtMakeString(std::string_view utf8);
Value rtValueToString(Value v);
double rtToNumber(Value v);

// The characters of a string as bytes, with any code unit past U+007F
// replaced by 0xFF — enough for the numeric and structural parsing the
// builtins do, and never enough to be mistaken for a general conversion.
std::string rtAsciiChars(const StringHeader* s);

// Diagnose `key` if it is a real member of `receiver` that bronze has not
// implemented; return quietly otherwise, so the caller reads `undefined`,
// which is what the language says for a property that does not exist.
// The tables are the ECMA-262 question "does this member exist?", never
// "have we got round to it?" — see rt_helpers.cpp.
void rtCheckUnimplementedMember(const char* receiver, const char* const* names, size_t count,
                                const std::string& key);

// ---- builtin namespaces ---------------------------------------------------
// Each family owns its own translation unit and exposes exactly two things:
// the namespace object, and the miss check that keeps an unimplemented
// member loud instead of `undefined`.

Value rtMathObject();
void rtMathCheckMissingMember(Value obj, const std::string& key);

}  // namespace bronze::runtime
