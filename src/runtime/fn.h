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
    // 10.2.9 SetFunctionName's answer: this function's own `name` property,
    // ARENA-interned so it is immortal and non-moving — the same bargain a
    // shape key makes, and the reason a raw pointer here is invisible to the
    // collector's payload scan (which forwards only Values that point INTO the
    // semispace) exactly as `instance_shape` above already is.
    //
    // NULL means the name was never recorded, which is not the same as the
    // empty string: an anonymous function expression really has `name === ""`
    // (10.2.9 via NamedEvaluation), while a native builtin has no key index to
    // name it with and so answers neither. `length` below shares that one flag,
    // because the two properties are created together by OrdinaryFunctionCreate
    // and a function that has one always has the other.
    struct StringHeader* name{nullptr};
    // The arity a short call is PADDED to, which is a fact about the calling
    // convention and NOT the `length` property: it counts every formal
    // parameter but the rest one, and is zero for a function that owns an
    // `arguments` object.
    uint32_t arity{0};
    // 10.2.10 SetFunctionLength's answer: the number of formal parameters
    // BEFORE the first one with a default or the rest one, which is a different
    // count from `arity` above and is why it is a second field rather than a
    // reuse of it. `function f(a, b = 1, ...c)` pads to 2 and has length 1.
    uint32_t length{0};
    bool is_generator{false};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code,
                                  Value env_record = Value::fromUndefined(), uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

}  // namespace bronze
