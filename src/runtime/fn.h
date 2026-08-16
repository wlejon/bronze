#pragma once

#include <cstddef>
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
    // Set by bronze_construct's ordinary path and nothing else: this function
    // object has been constructed once as a PLAIN constructor — not bound, not
    // a primitive-wrapper intrinsic — and its prototype/instance_shape pair
    // exists. The inline `new` fast path in generated code trusts the byte to
    // skip the helper's probes; it is sound because those probes answer by the
    // CODE pointer, which is fixed at creation, so no vetted function can
    // later start needing a slow path. bronze_abi.h pins the offset.
    bool construct_vetted{false};
    // Which NATIVE EXOTIC OBJECT a `new` of this function must allocate, or
    // `NativeBase::None` for the overwhelming majority that allocate the
    // ordinary plain instance (runtime/native_base.h names the codes).
    //
    // It is what makes `class MyMap extends Map {}` produce a Map: ECMA-262
    // has the BASE constructor allocate, through OrdinaryCreateFromConstructor
    // over NewTarget (10.1.13/10.1.14), and bronze allocates once up front in
    // `bronze_construct` — where the function being constructed IS NewTarget.
    // So the base's allocation decision has to be visible from the derived
    // constructor, and `bronze_class_extends` copies it down the chain when the
    // link is made rather than walking the chain on every construction.
    //
    // A non-None value also keeps `construct_vetted` false forever, which is
    // what stops the inline `new` fast path in generated code — it bump
    // allocates a PLAIN object and can express nothing else — from ever firing
    // for one of these.
    uint8_t native_base{0};
    // The rest of this word, spelled out because the GC payload scan reads
    // the whole payload as Values and this word — two bools, one code byte
    // and padding — is one of them. A heap block is recycled semispace memory, so padding
    // left unwritten holds OLD VALUE RESIDUE, and residue whose top two
    // bytes spell a heap tag sends the scan chasing a garbage payload —
    // whether it corrupts then depends on which bytes an unrelated change
    // shifted under it (this chunk's one-byte vet write was enough to turn
    // a green pixi GC-stress run into an environment-chain corruption).
    // create() zeroes these, so the word is a small integer, never a
    // plausible pointer.
    uint8_t padding_to_value_scan[5]{};

    static FunctionHeader* create(Heap& heap, NativeFunctionCode code,
                                  Value env_record = Value::fromUndefined(), uint32_t arity = 0);

    Value call(Value thisArg, uint32_t argc, Value* argv) const;
};

// The Math direct-dispatch guard in generated code loads the code pointer to
// compare it against the intrinsic's exported symbol, so this one field's
// position — and the Function kind's number — are ABI facts.
static_assert(offsetof(FunctionHeader, code) == BRONZE_ABI_FN_CODE_OFFSET);
static_assert(HeapKind::Function == BRONZE_ABI_OBJ_FLAGS_FUNCTION);

// The fields the inline `new` fast path reads (llvm_construct.cpp): the
// closure environment and arity for the direct code call, the instance shape
// for the allocation, and the vet byte that gates the whole path.
static_assert(offsetof(FunctionHeader, env_record) == BRONZE_ABI_FN_ENV_OFFSET);
static_assert(offsetof(FunctionHeader, prototype) == BRONZE_ABI_FN_PROTOTYPE_OFFSET);
static_assert(offsetof(FunctionHeader, properties) == BRONZE_ABI_FN_PROPERTIES_OFFSET);
static_assert(offsetof(FunctionHeader, instance_shape) == BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET);
static_assert(offsetof(FunctionHeader, arity) == BRONZE_ABI_FN_ARITY_OFFSET);
static_assert(offsetof(FunctionHeader, construct_vetted) == BRONZE_ABI_FN_CTOR_VETTED_OFFSET);

}  // namespace bronze
