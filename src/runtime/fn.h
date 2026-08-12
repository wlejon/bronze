#pragma once

#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// The primitives-only shape generated code is entered through; declared
// in the pure-C ABI registry (src/abi/bronze_abi.h) so a C++ class type
// can never leak into it — see the header for the sret-shift rationale.
using NativeFunctionCode = bronze_fn_code;

struct FunctionHeader {
    HeapObjectHeader header;
    NativeFunctionCode code{nullptr};
    // The closure's captured environment, or undefined. A Value, not a raw
    // pointer, so the generic GC payload scan forwards it — as a `void*` it was
    // invisible to the collector.
    Value env_record;
    // This function's `.prototype` object, or undefined until something asks
    // for it — a function that is never a constructor and whose prototype is
    // never decorated should not pay for the object, and closures are created
    // in loops.
    Value prototype;
    // This function's OWN properties - what `C.staticMethod =...` and a class's
    // `static` members are stored in - or undefined until one is written. An
    // ordinary object, so it costs nothing until used and inherits through the
    // shape's prototype, which is what makes a static member of a base class
    // visible on a derived one.
    Value properties;
    // Root shape for objects `new`ed from this function; its prototype is
    // the object above. Non-moving, created with it, and reset if
    // `.prototype` is reassigned.
    class Shape* instance_shape{nullptr};
    uint32_t arity{0};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code,
                                  Value env_record = Value::fromUndefined(), uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

}  // namespace bronze
