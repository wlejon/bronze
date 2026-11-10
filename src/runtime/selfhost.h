#pragma once

// The builtins written in JavaScript (src/selfhost/README.md).
//
// Their library is compiled ahead of time with bronze's own compiler, in the
// self-host dialect (src/lower/lower_selfhost.cpp), into one object every
// runtime image links (bronze_runtime by its link interface, the shared
// runtime as a source). Its entry, `bronze_selfhost_main`, runs once per
// thread, the first time the thread asks for one of these functions: its top
// level registers each builtin's function object here, and those objects are
// what the intrinsic tables (Array.prototype, Object) hold, exactly where a
// C++ builtin's object would be.
//
// A self-hosted builtin's function object is NATIVE (toString answers the
// NativeFunction form, it has no [[Construct]] and no `prototype`, it belongs
// to no realm), but its code is compiled code: it needs the pinned TLS
// register a C++ native ignores. So the runtime's shortcuts that call a
// native's code as a C function (bronze_dynamic_call, Function.prototype.call,
// the method cache's EXOTIC hit) ask rtIsSelfHostedCode first and take the
// generated-code entry for these.

#include <cstdint>

#include "runtime/value.h"

namespace bronze::runtime {

// One row per builtin the library defines. The registry key each row is
// registered under is in selfhost.cpp, beside the C++ table it replaces.
enum class SelfHosted : uint8_t {
    None = 0,
    ArrayForEach,
    ArrayMap,
    ArrayFilter,
    ArraySome,
    ArrayEvery,
    ArrayFind,
    ArrayFindIndex,
    ArrayReduce,
    ArrayFindLast,
    ArrayFindLastIndex,
    Count,
};

// The builtin's function object on this thread, running the library's entry
// first if this thread has not. One object per thread, as every intrinsic.
Value rtSelfHostedFunction(SelfHosted id);

// Whether `code` is the entry of a self-hosted builtin: compiled code that
// must be entered through the generated-code entry, never called as C.
bool rtIsSelfHostedCode(const void* code) noexcept;

}  // namespace bronze::runtime
