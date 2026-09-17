#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

// What `Array.of`, `Array.from` (builtin_constructors.cpp) and `Array.fromAsync`
// (builtin_array_from_async.cpp) share: the "construct through `this`"
// protocol of 23.1.2.1-3. The three members build their result by CONSTRUCTING
// `this` when `this` is a constructor — which is the whole reason
// `MyArr.of(1, 2, 3)` is a MyArr and not an Array — and the fast path when it
// is not. Kept as one set of helpers so the two paths INTERLEAVE identically
// with the iteration around them: a subclass with an index setter that throws
// must see the same prefix written as a plain array would have had.

namespace bronze::runtime {
namespace ctor_internal {

inline bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

inline Value newEmptyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->length = 0;
    return Value::fromObject(arr);
}

// Append through the root: growth reallocates the element block and can move
// the array itself.
inline void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    const uint32_t at = arrRoot.get().asObject<ArrayHeader>()->length;
    arrRoot.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

// 23.1.2.1 step 4 and 23.1.2.2 step 4: `Array.of` and `Array.from` build their
// result by CONSTRUCTING `this` when `this` is a constructor.
//
// False takes the plain-array path, and covers the three cases where
// constructing would be observably the same as ArrayCreate: `this` is absent
// (a detached `const of = Array.of`), `this` is not a constructor at all, or
// `this` IS %Array% — whose 23.1.1.1 over a single length argument is exactly
// ArrayCreate(len). So the ordinary `Array.of(1, 2, 3)` never enters a
// construction, and the guard is one call and one identity compare.
inline bool buildsThroughThis(Value thisVal) {
    return isCallable(thisVal) && !rtIsArrayConstructor(thisVal);
}

// Construct(C, « len ») or Construct(C), depending on whether the caller knows
// the length yet — `Array.from` over an ITERATOR does not (step 5.b passes no
// argument), and every other site does.
inline Value constructThrough(Rooted<Value>& ctor, const uint32_t* len) {
    if (!len) return Value(bronze_construct(ctor.get().rawBits(), 0, nullptr));
    Rooted<Value> lenRoot{Value::fromDouble(*len)};
    return Value(bronze_construct(ctor.get().rawBits(), 1,
                                  reinterpret_cast<const uint64_t*>(lenRoot.slot_ptr())));
}

// CreateDataPropertyOrThrow(A, ToString(index), value), or the append that is
// the same thing on an array being filled front to back.
inline void emitAt(Rooted<Value>& out, uint32_t index, Rooted<Value>& value, bool constructed) {
    if (!constructed) {
        appendTo(out, value);
        return;
    }
    Rooted<Value> key{Value::fromDouble(index)};
    bronze_elem_set(out.get().rawBits(), key.get().rawBits(), value.get().rawBits(),
                    /*strict=*/true);
}

// The `Set(A, "length", n, true)` the members finish with. A no-op on the fast
// path, where the array's length IS the count appended.
inline void setResultLength(Rooted<Value>& out, uint32_t n, bool constructed) {
    if (!constructed) return;
    Rooted<Value> key{rtMakeString("length")};
    Rooted<Value> value{Value::fromDouble(n)};
    bronze_elem_set(out.get().rawBits(), key.get().rawBits(), value.get().rawBits(),
                    /*strict=*/true);
}

// Call(mapFn, thisArg, « item, index »).
inline Value callMapper(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& item,
                        uint32_t index) {
    Value block[2] = {item.get(), Value::fromDouble(static_cast<double>(index))};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 2,
                                     reinterpret_cast<const uint64_t*>(block)));
}

}  // namespace ctor_internal

// 23.1.2.1 `Array.fromAsync` (builtin_array_from_async.cpp), declared here so
// builtin_constructors.cpp's static table stays the ONE list of what `Array`
// implements.
uint64_t rtArrayFromAsyncBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                                 const uint64_t* argv);

}  // namespace bronze::runtime
