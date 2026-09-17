// The global constructor OBJECTS: `Array`, `String`, `Boolean` and `Number`.
//
// This file is about the objects a bare name resolves to, which is a different
// thing from the prototype method tables in builtin_array.cpp and
// builtin_string.cpp — those answer "what can I call on a value?", this one
// answers "what does the identifier `Array` denote?". Keeping them apart is
// what stops one file from having to explain both.
//
// The interning mechanism is the typed arrays', unchanged and deliberately not
// re-invented: lowering's closed provided-globals list resolves the name to
// `global.get "<name>"`, `bronze_global_get` asks `rtGlobalConstructor` for it,
// and the answer is a `bronze_function_singleton` INTERNED ON THE CODE POINTER
// — so the bare name and `x.constructor` are the same object and `===` holds
// between them (`Array.isArray(x) ?...: x.constructor === Array` is ordinary
// JS, and three.js writes both spellings).
//
// One distinct C function per constructor is therefore load-bearing: two
// constructors sharing a body with a kind parameter would intern to ONE object
// and `Array === String` would be true.

#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/number_format.h"
#include "runtime/proxy.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

namespace bronze::runtime {

namespace {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

Value newEmptyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->length = 0;
    return Value::fromObject(arr);
}

// Append through the root: growth reallocates the element block and can move
// the array itself.
void appendTo(Rooted<Value>& arrRoot, Rooted<Value>& val) {
    const uint32_t at = arrRoot.get().asObject<ArrayHeader>()->length;
    arrRoot.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

// 23.1.2.1 step 4 and 23.1.2.2 step 4: `Array.of` and `Array.from` build their
// result by CONSTRUCTING `this` when `this` is a constructor, which is the
// whole reason `MyArr.of(1, 2, 3)` is a MyArr and not an Array.
//
// False takes the plain-array path, and covers the three cases where
// constructing would be observably the same as ArrayCreate: `this` is absent
// (a detached `const of = Array.of`), `this` is not a constructor at all, or
// `this` IS %Array% — whose 23.1.1.1 over a single length argument is exactly
// ArrayCreate(len). So the ordinary `Array.of(1, 2, 3)` never enters a
// construction, and the guard is one call and one identity compare.
bool buildsThroughThis(Value thisVal) {
    return isCallable(thisVal) && !rtIsArrayConstructor(thisVal);
}

// Construct(C, « len ») or Construct(C), depending on whether the caller knows
// the length yet — `Array.from` over an ITERATOR does not (step 5.b passes no
// argument), and every other site does.
Value constructThrough(Rooted<Value>& ctor, const uint32_t* len) {
    if (!len) return Value(bronze_construct(ctor.get().rawBits(), 0, nullptr));
    Rooted<Value> lenRoot{Value::fromDouble(*len)};
    return Value(bronze_construct(ctor.get().rawBits(), 1,
                                  reinterpret_cast<const uint64_t*>(lenRoot.slot_ptr())));
}

// CreateDataPropertyOrThrow(A, ToString(index), value), or the append that is
// the same thing on an array being filled front to back. Kept as one call so
// the two paths INTERLEAVE identically with the iteration around them: a
// subclass with an index setter that throws must see the same prefix written
// as a plain array would have had.
void emitAt(Rooted<Value>& out, uint32_t index, Rooted<Value>& value, bool constructed) {
    if (!constructed) {
        appendTo(out, value);
        return;
    }
    Rooted<Value> key{Value::fromDouble(index)};
    bronze_elem_set(out.get().rawBits(), key.get().rawBits(), value.get().rawBits(),
                    /*strict=*/true);
}

// The `Set(A, "length", n, true)` both members finish with. A no-op on the
// fast path, where the array's length IS the count appended.
void setResultLength(Rooted<Value>& out, uint32_t n, bool constructed) {
    if (!constructed) return;
    Rooted<Value> key{rtMakeString("length")};
    Rooted<Value> value{Value::fromDouble(n)};
    bronze_elem_set(out.get().rawBits(), key.get().rawBits(), value.get().rawBits(),
                    /*strict=*/true);
}

// ---- Array (23.1) -----------------------------------------------------------

// A dense array costs eight bytes per element, so a length the specification
// allows is not thereby a length this heap can hold. Refused BEFORE the
// allocation, so `std::bad_alloc` never unwinds out of a helper generated code
// called — the same rule a byte store follows. False means it threw.
bool arrayLengthFits(uint32_t n) {
    const size_t bytes = static_cast<size_t>(n) * sizeof(Value);
    if (bytes + 64 >= rtHeap().reserved_size() / 2) {
        rtThrowRangeError("Array allocation failed: " + std::to_string(n) +
                          " elements does not fit in the heap");
        return false;
    }
    return true;
}

// 23.1.1.1. The array this fills is the RECEIVER when there is one:
// ArrayCreate's proto argument comes from NewTarget, and bronze performs that
// allocation at the one site every `new` goes through, so `new Array(3)` and
// `new (class extends Array)(3)` both arrive here with the array already made
// and only its contents left to write. `Array(3)` without `new` has no
// receiver — the language allows the call form (step 1 defaults NewTarget to
// the active function) — and builds one.
uint64_t arrayConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> out{Value(thisBits)};
    if (!rtIsNativeConstructReceiver(out.get())) out.set(newEmptyArray());
    // 23.1.1.1 step 3: ONE argument that is a number is a LENGTH, and every
    // other shape is an element list. So `new Array(3)` has length 3 and
    // `new Array("3")` has length 1, and `new Array(3, 4)` has length 2.
    if (args.count() == 1 && args[0].isNumber()) {
        const double len = args[0].asNumber();
        // Step 3.b: the length has to survive a round trip through a uint32, or
        // it was never an array length. NaN, a negative, a fraction and 2^32
        // all fail this and all raise the same RangeError the specification
        // names.
        if (!(len >= 0.0) || len > 4294967295.0 || std::floor(len) != len) {
            return rtThrowRangeError("Invalid array length").rawBits();
        }
        // n HOLES, not n undefineds. The difference is observable: `new
        // Array(3).forEach(f)` calls `f` zero times, and console.log prints
        // `[ <3 empty items> ]`. Growing an empty array leaves exactly that
        // (array.h at setLength), which is why this is a length write and not
        // a fill.
        if (!arrayLengthFits(static_cast<uint32_t>(len))) {
            return Value::fromUndefined().rawBits();
        }
        ArrayHeader::setLength(rtHeap(), out, static_cast<uint32_t>(len));
        return out.get().rawBits();
    }
    for (uint32_t i = 0; i < args.count(); ++i) {
        Rooted<Value> elem{args[i]};
        appendTo(out, elem);
    }
    return out.get().rawBits();
}

// 23.1.2.2. The question is about the KIND of the object, not about anything a
// program can install, which is why it is a flags test and not a property read.
uint64_t arrayIsArray(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value v = args[0];
    // `Array.prototype` is an Array exotic object (23.1.3) that bronze keeps as
    // a plain one, for the reason builtin_array.cpp gives where its `length` is
    // installed. IsArray is the one question that difference is visible
    // through, so it is answered by identity here rather than left to report
    // the kind and be wrong.
    if (rtIsArrayPrototypeObject(v)) return Value::fromBool(true).rawBits();
    return Value::fromBool(v.isObject() &&
                           v.asObject<HeapObjectHeader>()->flags == HeapKind::Array)
        .rawBits();
}

uint64_t arrayOf(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> ctor{Value(thisBits)};
    const bool constructed = buildsThroughThis(ctor.get());
    const uint32_t len = args.count();
    Rooted<Value> out{constructed ? constructThrough(ctor, &len) : newEmptyArray()};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{args[i]};
        emitAt(out, i, elem, constructed);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    setResultLength(out, len, constructed);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return out.get().rawBits();
}

// Does 23.1.2.1 step 3's GetMethod(items, @@iterator) find something? The fast
// kinds answer yes without a property read at all — `rtOpenIterator` steps an
// array, a string, a typed array, a Map and a Set from a cursor — and anything
// else is asked for the well-known key, because the answer decides between the
// iterator path and the array-like one and getting it wrong turns
// `Array.from(userIterable)` into an empty array.

Value callMapper(Rooted<Value>& fn, Rooted<Value>& thisArg, Rooted<Value>& item, uint32_t index) {
    Value block[2] = {item.get(), Value::fromDouble(static_cast<double>(index))};
    return Value(bronze_dynamic_call(fn.get().rawBits(), thisArg.get().rawBits(), 2,
                                     reinterpret_cast<const uint64_t*>(block)));
}

uint64_t arrayFrom(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> src{args[0]};
    Rooted<Value> mapFn{args[1]};
    Rooted<Value> thisArg{args[2]};
    if (src.get().isNull() || src.get().isUndefined()) {
        return rtThrowTypeError("Array.from requires an array-like or iterable object, not " +
                                rtIterableKindName(src.get()))
            .rawBits();
    }
    // Step 2.a: checked BEFORE anything is iterated, so a bad mapper does not
    // half-consume the source first.
    if (!mapFn.get().isUndefined() && !isCallable(mapFn.get())) {
        return rtThrowTypeError("Array.from: the second argument is not a function").rawBits();
    }
    Rooted<Value> ctor{Value(thisBits)};
    const bool constructed = buildsThroughThis(ctor.get());

    if (rtHasIteratorMethod(src)) {
        // Step 5.b constructs with NO argument on this path, because the count
        // is not known until the iterator is exhausted — which is the one place
        // the two halves of this member differ in what they hand the base.
        Rooted<Value> out{constructed ? constructThrough(ctor, nullptr) : newEmptyArray()};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        Rooted<Value> rec{Value(bronze_iter_open(src.get().rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        uint32_t i = 0;
        while (bronze_iter_step(rec.get().rawBits())) {
            Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
            if (!mapFn.get().isUndefined()) {
                item.set(callMapper(mapFn, thisArg, item, i));
                // The mapper is user code. Step 6.e.viii closes the iterator
                // when it throws, and carrying on would be the runtime
                // continuing past an exception.
                if (rtExceptionPending()) {
                    bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
                    return out.get().rawBits();
                }
            }
            emitAt(out, i, item, constructed);
            if (rtExceptionPending()) {
                bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
                return out.get().rawBits();
            }
            ++i;
        }
        setResultLength(out, i, constructed);
        return out.get().rawBits();
    }

    const uint32_t len = rtArrayLikeLength(src);
    Rooted<Value> out{constructed ? constructThrough(ctor, &len) : newEmptyArray()};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> item{
            Value(bronze_elem_get(src.get().rawBits(), Value::fromDouble(i).rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        if (!mapFn.get().isUndefined()) {
            item.set(callMapper(mapFn, thisArg, item, i));
            if (rtExceptionPending()) return out.get().rawBits();
        }
        emitAt(out, i, item, constructed);
        if (rtExceptionPending()) return out.get().rawBits();
    }
    setResultLength(out, len, constructed);
    return out.get().rawBits();
}

// ---- Array.fromAsync (23.1.2.1, ES2024) --------------------------------------
//
// `Array.from` over an ASYNC iterable, answering a promise for the array. The
// spec writes it as an async abstract closure with an `await` at each of three
// points — the result of `next()`, the value a sync source yields, and the
// mapper's return — and a native builtin has no compiled state machine to
// suspend at them. So the closure is written the way the machine would have
// compiled it: one STATE record (a plain object with internal slots, invisible
// to the program) holding everything the loop carries across a tick, and one
// driver that runs the synchronous stretch between two awaits and subscribes
// a continuation to the promise it stops at. Each continuation is a native
// closure over the record, so no C++ frame lives across a job-queue turn and
// nothing the collector moves is held anywhere but a slot it walks.
//
// Three sources, one loop:
//   - an object with @@asyncIterator (mode 0): `next()` is called and its
//     RESULT awaited; the value is used as it comes (step 5.k.i.5 does not
//     await it again);
//   - one with only @@iterator (mode 1): the spec wraps it in
//     CreateAsyncFromSyncIterator, whose `next` awaits the sync result's
//     VALUE. The wrapper is not built; the sync iterator is stepped directly
//     and each value awaited, which is the same program with one fewer
//     object per element;
//   - anything else (mode 2): an array-like, each `Get(k)` awaited.
// A mapper's return is awaited on every mode. Errors before the first await
// reject the promise rather than throw (3's AsyncFunctionStart), so
// `Array.fromAsync(null)` is a rejection and `Array.fromAsync(7)` — ToObject
// of a number, length 0 — resolves with `[]`.

namespace FromAsyncSlot {
enum : uint32_t {
    Capability,   // the promise the whole operation settles
    Out,          // the array (or subclass instance) being filled
    Constructed,  // 1 when `Out` came from Construct(C), 0 for ArrayCreate
    Index,        // k
    MapFn,        // undefined when not mapping
    ThisArg,
    Mode,         // 0 async iterator, 1 sync iterator record, 2 array-like
    Iterator,     // mode 0: the iterator object; mode 1: the IterRecord
    NextFn,       // mode 0: its `next`
    Source,       // mode 2: the array-like
    Length,       // mode 2: its length
    kCount
};
}

Value fromAsyncSlot(Rooted<Value>& state, uint32_t slot) {
    return state.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void setFromAsyncSlot(Rooted<Value>& state, uint32_t slot, Value v) {
    state.get().asObject<ObjectHeader>()->setInternalSlot(slot, v);
}

uint32_t fromAsyncMode(Rooted<Value>& state) {
    return static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Mode).asNumber());
}

void fromAsyncResume(Rooted<Value>& state);
uint64_t fromAsyncOnNextResult(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t fromAsyncOnValue(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t fromAsyncOnMapped(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t fromAsyncOnRejectedClosing(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv);
uint64_t fromAsyncOnRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv);

// IfAbruptCloseAsyncIterator / IfAbruptCloseIterator: the pending exception is
// the completion the program sees, so the iterator's `return` runs with its own
// errors discarded. An async `return` answers a promise the spec would await;
// nothing observable of this operation depends on when it settles, so it is
// not waited for.
void fromAsyncCloseSource(Rooted<Value>& state) {
    const uint32_t mode = fromAsyncMode(state);
    if (mode == 1) {
        Rooted<Value> rec{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return;
    }
    if (mode != 0) return;
    Rooted<Value> thrown{Value(bronze_tls_block_addr()->exception_cell)};
    rtClearException();
    Rooted<Value> iter{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
    Rooted<Value> key{rtMakeString("return")};
    Rooted<Value> ret{Value(bronze_elem_get(iter.get().rawBits(), key.get().rawBits()))};
    if (!rtExceptionPending() && isCallable(ret.get())) {
        ret.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr);
    }
    rtClearException();
    rtThrow(thrown.get());
}

void fromAsyncReject(Rooted<Value>& state) {
    Rooted<Value> cap{fromAsyncSlot(state, FromAsyncSlot::Capability)};
    rtRejectCapabilityWithPending(cap);
}

// `Await(v)`: PromiseResolve(%Promise%, v), then the two continuations. The
// rejection side closes the source only where the spec's IfAbruptClose does
// — around the mapper's await and the sync source's value — and not around
// `next()` itself, whose rejection is the iterator's own.
void fromAsyncAwait(Rooted<Value>& state, Rooted<Value>& v, NativeFunctionCode onFulfilled,
                    bool closeOnReject) {
    Rooted<Value> promise{rtPromiseResolveValue(v)};
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return;
    }
    Rooted<Value> onF{rtMakeNativeClosure(onFulfilled, state, 1)};
    Rooted<Value> onR{rtMakeNativeClosure(
        closeOnReject ? fromAsyncOnRejectedClosing : fromAsyncOnRejected, state, 1)};
    Rooted<Value> noCap{Value::fromUndefined()};
    rtPerformPromiseThen(promise, onF, onR, noCap);
}

void fromAsyncFinish(Rooted<Value>& state) {
    Rooted<Value> out{fromAsyncSlot(state, FromAsyncSlot::Out)};
    const bool constructed = fromAsyncSlot(state, FromAsyncSlot::Constructed).asNumber() != 0;
    const auto k = static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Index).asNumber());
    setResultLength(out, k, constructed);
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return;
    }
    Rooted<Value> cap{fromAsyncSlot(state, FromAsyncSlot::Capability)};
    rtSettleCapability(cap, out, /*reject=*/false);
}

// The synchronous stretch that begins an element: ask the source for the
// next one and stop at the first await.
void fromAsyncResume(Rooted<Value>& state) {
    const uint32_t mode = fromAsyncMode(state);
    if (mode == 0) {
        Rooted<Value> iter{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
        Rooted<Value> next{fromAsyncSlot(state, FromAsyncSlot::NextFn)};
        Rooted<Value> result{next.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr)};
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return;
        }
        fromAsyncAwait(state, result, fromAsyncOnNextResult, /*closeOnReject=*/false);
        return;
    }
    if (mode == 1) {
        Rooted<Value> rec{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
        const bool more = bronze_iter_step(rec.get().rawBits());
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return;
        }
        if (!more) {
            fromAsyncFinish(state);
            return;
        }
        Rooted<Value> value{Value(bronze_iter_value(rec.get().rawBits()))};
        fromAsyncAwait(state, value, fromAsyncOnValue, /*closeOnReject=*/true);
        return;
    }
    const auto k = static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Index).asNumber());
    const auto len = static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Length).asNumber());
    if (k >= len) {
        fromAsyncFinish(state);
        return;
    }
    Rooted<Value> source{fromAsyncSlot(state, FromAsyncSlot::Source)};
    Rooted<Value> value{
        Value(bronze_elem_get(source.get().rawBits(), Value::fromDouble(k).rawBits()))};
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return;
    }
    fromAsyncAwait(state, value, fromAsyncOnValue, /*closeOnReject=*/false);
}

// Mode 0's continuation: the awaited `next()` result. 5.k.i.3: not an object
// is a TypeError, and no close — the iterator gave a bad answer.
uint64_t fromAsyncOnNextResult(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> state{Value(env)};
    Rooted<Value> result{args[0]};
    if (!result.get().isObject()) {
        rtThrowTypeError("Array.fromAsync: the async iterator's next() result is not an object");
        fromAsyncReject(state);
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> doneKey{rtMakeString("done")};
    Rooted<Value> done{Value(bronze_elem_get(result.get().rawBits(), doneKey.get().rawBits()))};
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return Value::fromUndefined().rawBits();
    }
    if (bronze_truthy(done.get().rawBits())) {
        fromAsyncFinish(state);
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> valueKey{rtMakeString("value")};
    Rooted<Value> value{Value(bronze_elem_get(result.get().rawBits(), valueKey.get().rawBits()))};
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return Value::fromUndefined().rawBits();
    }
    const uint64_t one[1] = {value.get().rawBits()};
    return fromAsyncOnValue(state.get().rawBits(), 0, 1, one);
}

// An element's value in hand: map it (and await the mapper) or store it.
uint64_t fromAsyncOnValue(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> state{Value(env)};
    Rooted<Value> value{args[0]};
    Rooted<Value> mapFn{fromAsyncSlot(state, FromAsyncSlot::MapFn)};
    if (mapFn.get().isUndefined()) {
        const uint64_t one[1] = {value.get().rawBits()};
        return fromAsyncOnMapped(state.get().rawBits(), 0, 1, one);
    }
    Rooted<Value> thisArg{fromAsyncSlot(state, FromAsyncSlot::ThisArg)};
    const auto k = static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Index).asNumber());
    Rooted<Value> mapped{callMapper(mapFn, thisArg, value, k)};
    if (rtExceptionPending()) {
        // 5.k.i.5.b.ii / 6.e.iii.b.ii: the mapper threw, so the source is
        // closed before the rejection.
        fromAsyncCloseSource(state);
        fromAsyncReject(state);
        return Value::fromUndefined().rawBits();
    }
    fromAsyncAwait(state, mapped, fromAsyncOnMapped, /*closeOnReject=*/true);
    return Value::fromUndefined().rawBits();
}

// The element is final: CreateDataPropertyOrThrow, advance, and resume.
uint64_t fromAsyncOnMapped(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> state{Value(env)};
    Rooted<Value> value{args[0]};
    Rooted<Value> out{fromAsyncSlot(state, FromAsyncSlot::Out)};
    const bool constructed = fromAsyncSlot(state, FromAsyncSlot::Constructed).asNumber() != 0;
    const auto k = static_cast<uint32_t>(fromAsyncSlot(state, FromAsyncSlot::Index).asNumber());
    emitAt(out, k, value, constructed);
    if (rtExceptionPending()) {
        fromAsyncCloseSource(state);
        fromAsyncReject(state);
        return Value::fromUndefined().rawBits();
    }
    setFromAsyncSlot(state, FromAsyncSlot::Index, Value::fromDouble(k + 1));
    fromAsyncResume(state);
    return Value::fromUndefined().rawBits();
}

uint64_t fromAsyncOnRejectedClosing(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> state{Value(env)};
    Rooted<Value> reason{args[0]};
    rtThrow(reason.get());
    fromAsyncCloseSource(state);
    fromAsyncReject(state);
    return Value::fromUndefined().rawBits();
}

uint64_t fromAsyncOnRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> state{Value(env)};
    Rooted<Value> reason{args[0]};
    Rooted<Value> cap{fromAsyncSlot(state, FromAsyncSlot::Capability)};
    rtSettleCapability(cap, reason, /*reject=*/true);
    return Value::fromUndefined().rawBits();
}

uint64_t arrayFromAsync(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> ctor{Value(thisBits)};
    Rooted<Value> src{args[0]};
    Rooted<Value> mapFn{args[1]};
    Rooted<Value> thisArg{args[2]};

    // Step 2: the capability comes first, because every failure from here on
    // is a rejection of it and not a throw.
    Rooted<Value> cap{rtNewPromiseCapabilityForIntrinsic()};
    Rooted<Value> promise{rtCapabilityPromise(cap.get())};

    Rooted<Value> state{Value::fromObject(ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), rtPlainObjectShape(), FromAsyncSlot::kCount))};
    state.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    setFromAsyncSlot(state, FromAsyncSlot::Capability, cap.get());
    setFromAsyncSlot(state, FromAsyncSlot::Index, Value::fromDouble(0));
    setFromAsyncSlot(state, FromAsyncSlot::MapFn, mapFn.get());
    setFromAsyncSlot(state, FromAsyncSlot::ThisArg, thisArg.get());

    // 3.a-b: a mapper that is present and not callable.
    if (!mapFn.get().isUndefined() && !isCallable(mapFn.get())) {
        rtThrowTypeError("Array.fromAsync: the second argument is not a function");
        fromAsyncReject(state);
        return promise.get().rawBits();
    }
    // 3.c-d: GetMethod(asyncItems, @@asyncIterator), then @@iterator. GetMethod
    // of null or undefined is the TypeError that makes `fromAsync(null)` a
    // rejection.
    if (src.get().isNull() || src.get().isUndefined()) {
        rtThrowTypeError("Array.fromAsync requires an array-like or iterable object, not " +
                         rtIterableKindName(src.get()));
        fromAsyncReject(state);
        return promise.get().rawBits();
    }
    Rooted<Value> asyncMethod{Value::fromUndefined()};
    if (src.get().isObject()) {
        Rooted<Value> key{rtAsyncIteratorKey()};
        asyncMethod.set(Value(bronze_elem_get(src.get().rawBits(), key.get().rawBits())));
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return promise.get().rawBits();
        }
    }
    const bool constructed = buildsThroughThis(ctor.get());
    setFromAsyncSlot(state, FromAsyncSlot::Constructed, Value::fromDouble(constructed ? 1 : 0));

    if (isCallable(asyncMethod.get())) {
        // 3.e-h: the async iterator, and Construct(C) with no length.
        Rooted<Value> iter{asyncMethod.get().asObject<FunctionHeader>()->call(src.get(), 0, nullptr)};
        if (!rtExceptionPending() && !iter.get().isObject()) {
            rtThrowTypeError("the result of Symbol.asyncIterator is not an object");
        }
        Rooted<Value> nextKey{rtMakeString("next")};
        Rooted<Value> next{rtExceptionPending()
                               ? Value::fromUndefined()
                               : Value(bronze_elem_get(iter.get().rawBits(), nextKey.get().rawBits()))};
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return promise.get().rawBits();
        }
        Rooted<Value> out{constructed ? constructThrough(ctor, nullptr) : newEmptyArray()};
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return promise.get().rawBits();
        }
        setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(0));
        setFromAsyncSlot(state, FromAsyncSlot::Iterator, iter.get());
        setFromAsyncSlot(state, FromAsyncSlot::NextFn, next.get());
        setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
        fromAsyncResume(state);
        return promise.get().rawBits();
    }
    if (rtHasIteratorMethod(src)) {
        Rooted<Value> rec{Value(bronze_iter_open(src.get().rawBits()))};
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return promise.get().rawBits();
        }
        Rooted<Value> out{constructed ? constructThrough(ctor, nullptr) : newEmptyArray()};
        if (rtExceptionPending()) {
            fromAsyncReject(state);
            return promise.get().rawBits();
        }
        setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(1));
        setFromAsyncSlot(state, FromAsyncSlot::Iterator, rec.get());
        setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
        fromAsyncResume(state);
        return promise.get().rawBits();
    }
    // 3.i-k: the array-like, and Construct(C, « len »).
    const uint32_t len = rtArrayLikeLength(src);
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return promise.get().rawBits();
    }
    Rooted<Value> out{constructed ? constructThrough(ctor, &len) : newEmptyArray()};
    if (rtExceptionPending()) {
        fromAsyncReject(state);
        return promise.get().rawBits();
    }
    setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(2));
    setFromAsyncSlot(state, FromAsyncSlot::Source, src.get());
    setFromAsyncSlot(state, FromAsyncSlot::Length, Value::fromDouble(len));
    setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
    fromAsyncResume(state);
    return promise.get().rawBits();
}

// ---- String (22.1) ----------------------------------------------------------

// 22.1.1.1 as a CONVERSION, which is the whole of this body. A native
// constructor cannot see NewTarget through the uniform calling convention, so
// the `new` form is not a branch here at all: `bronze_construct` recognises
// this function object and builds the String exotic object instead of ever
// entering it (builtin_wrappers.cpp).
uint64_t stringConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // Step 1: no argument at all is the empty string, which is NOT the same as
    // `String(undefined)` — that one is the text "undefined".
    if (args.count() == 0) return rtMakeString("").rawBits();
    // 22.1.1.1 step 2: called WITHOUT `new`, a symbol argument is the one value
    // `String` does not put through ToString — it answers
    // SymbolDescriptiveString instead. Reading it as a plain conversion would
    // make `String(sym)` throw, which is a wrong answer rather than a missing
    // feature: the specification says this call is how you spell it.
    if (args[0].isSymbol()) return rtMakeString(rtSymbolDescriptiveString(args[0])).rawBits();
    // 7.1.17 ToString, step 1 included: an object is ToPrimitive'd with hint
    // STRING, so `toString` is tried before `valueOf` — the opposite of what
    // `'' + o` does, and observably so for an object that defines both.
    Rooted<Value> value{args[0]};
    return rtToStringValue(value).rawBits();
}

// 22.1.2.1. Each argument is ToUint16, so `String.fromCharCode(65, 66)` is
// "AB" and a value outside the range wraps rather than being clamped or
// rejected — the truncation ECMA-262 7.1.7 specifies.
uint64_t stringFromCharCode(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::vector<uint16_t> units;
    units.reserve(args.count());
    for (uint32_t i = 0; i < args.count(); ++i) {
        const double n = rtToNumber(args[i]);
        // ToUint16 (7.1.7): NaN and the infinities are 0, everything else
        // truncates towards zero and takes the low sixteen bits.
        if (std::isnan(n) || std::isinf(n)) {
            units.push_back(0);
            continue;
        }
        const double truncated = std::trunc(n);
        const double wrapped = std::fmod(truncated, 65536.0);
        units.push_back(static_cast<uint16_t>(
            static_cast<int32_t>(wrapped < 0 ? wrapped + 65536.0 : wrapped)));
    }
    return rtStringFromUnits(units).rawBits();
}

// 22.1.2.2. CodePointsToString (11.1.6) over the arguments: this is the member
// that can spell an astral character, because 11.1.3 UTF16EncodeCodePoint emits
// a surrogate PAIR at or above 0x10000 where `fromCharCode` above truncates to
// one code unit and loses it.
//
// The three refusals are one step (2.b): a value that is not an integral
// Number, a negative one, or one above 0x10FFFF is a RangeError naming the
// value — catchable, unlike the wrapping `fromCharCode` performs, because a
// code point out of range names a bug in the caller where a code UNIT out of
// range is the defined truncation.
uint64_t stringFromCodePoint(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    std::vector<uint16_t> units;
    units.reserve(args.count());
    for (uint32_t i = 0; i < args.count(); ++i) {
        // ToNumber is user code (a `valueOf`), so nothing raw is held across
        // it; `units` is a C++ vector of plain integers and holds no heap.
        const double n = rtToNumber(args[i]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (!std::isfinite(n) || std::trunc(n) != n || n < 0.0 || n > 0x10FFFF) {
            char buf[32];
            const size_t len = formatJsNumber(n, buf);
            return rtThrowRangeError("Invalid code point " + std::string(buf, len)).rawBits();
        }
        const uint32_t cp = static_cast<uint32_t>(n);
        if (cp <= 0xFFFF) {
            units.push_back(static_cast<uint16_t>(cp));
            continue;
        }
        const uint32_t rest = cp - 0x10000;
        units.push_back(static_cast<uint16_t>(0xD800 + (rest >> 10)));
        units.push_back(static_cast<uint16_t>(0xDC00 + (rest & 0x3FF)));
    }
    return rtStringFromUnits(units).rawBits();
}

// 22.1.2.4 String.raw(template, ...substitutions). The one member of `String`
// that is not about characters at all: it reads the `raw` array a TAGGED
// TEMPLATE hands its tag and joins it with the substitutions, so
// `String.raw`\n`` is a two-character string rather than a newline.
//
// The template is an ORDINARY argument here, not a syntactic form: step 2 is
// ToObject and step 3 is Get(cooked, "raw"), so `String.raw({raw: ['a','b']},
// 1)` is "a1b" and is pinned as such. Both reads go through the general
// array-like protocol (7.3.18 LengthOfArrayLike, then Get per index), which is
// why a plain object with a `length` works exactly as an array does.
uint64_t stringRaw(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    (void)thisBits;
    RootedArgs args(argc, argv);
    Rooted<Value> cooked{args[0]};
    if (cooked.get().isUndefined() || cooked.get().isNull()) {
        return rtThrowTypeError("String.raw called on " + rtIterableKindName(cooked.get()))
            .rawBits();
    }
    // `raw` is read with the generic element path rather than an interned key,
    // because the receiver can be any object a program built — including one
    // whose `raw` is an accessor. The key is hoisted into its own statement:
    // making it a sibling argument of the receiver read would leave the
    // evaluation order to the compiler, and one of the two allocates.
    Rooted<Value> rawKey{rtMakeString("raw")};
    Rooted<Value> literals{
        Value(bronze_elem_get(cooked.get().rawBits(), rawKey.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    // Step 3 is ToObject(raw): only undefined/null throw. A primitive string
    // wraps to a String object whose `length` is its code-unit count and whose
    // indices are its characters — both of which the generic element path
    // already answers for a bare string, so no wrapper is materialized. Every
    // other primitive wraps to an object with no `length`, and
    // ToLength(undefined) is 0: the empty string, not an error.
    if (literals.get().isUndefined() || literals.get().isNull()) {
        return rtThrowTypeError("String.raw: the template's `raw` is " +
                                rtIterableKindName(literals.get()))
            .rawBits();
    }
    const uint32_t literalCount = literals.get().isString()
                                      ? literals.get().asString<StringHeader>()->length
                                      : rtArrayLikeLength(literals);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    // Step 5: an empty `raw` is the empty string, before any substitution is
    // even converted.
    Rooted<Value> out{rtMakeString("")};
    for (uint32_t i = 0; i < literalCount; ++i) {
        Rooted<Value> segment{rtArrayLikeElement(literals, i)};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        segment.set(rtValueToString(segment.get()));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        out.set(Value(bronze_string_concat(out.get().rawBits(), segment.get().rawBits())));
        // Step 8.d: the LAST literal ends the string — a substitution after it
        // is never read, and `String.raw({raw:['a']}, 1)` is "a".
        if (i + 1 == literalCount) break;
        if (i + 1 >= args.count()) continue;
        Rooted<Value> sub{rtValueToString(args[i + 1])};
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        out.set(Value(bronze_string_concat(out.get().rawBits(), sub.get().rawBits())));
    }
    return out.get().rawBits();
}

// ---- Boolean (20.3) ---------------------------------------------------------

// 20.3.1.1 as a conversion, for the same reason `String` is one: the `new` form
// is intercepted before this body runs. `Boolean(x)` is exactly ToBoolean, which
// is the only use a program that is not testing wrapper identity ever has for
// it.
uint64_t booleanConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(bronze_truthy(args[0].rawBits())).rawBits();
}

// ---- the registry -----------------------------------------------------------

struct StaticFn {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

// Arity 0 everywhere a builtin is variadic, for the reason a builtin is an
// ordinary function object: `FunctionHeader::arity` is the count a short call
// is PADDED to, so a variadic native declaring 2 would see `Array.of(1)` as
// `(1, undefined)` and produce a two-element array.
const StaticFn kArrayStatics[] = {
    {"from", arrayFrom, 0},
    {"fromAsync", arrayFromAsync, 0},
    {"isArray", arrayIsArray, 1},
    {"of", arrayOf, 0},
};

const StaticFn kStringStatics[] = {
    {"fromCharCode", stringFromCharCode, 0},
    {"fromCodePoint", stringFromCodePoint, 0},
    // Arity 0, not 1: `raw` is variadic and a short call padded to one argument
    // would hand it an `undefined` substitution that step 8.e would then
    // stringify into the result.
    {"raw", stringRaw, 0},
};

// Real static members of each constructor that bronze has not built.
// `Array.prototype` left this list when it became an object of its own
// (builtin_array.cpp): an array still answers its members BESIDE the value,
// but the VALUE the expression denotes is real now — the same method objects,
// off the same table, with a write to it refused by name so nothing can be
// installed there that no array would find. `String.prototype`,
// `Boolean.prototype` and `Number.prototype` are answered from the
// constructor's own prototype slot.
//
// Neither has an unimplemented static left: 23.1.2 names four members and
// 22.1.2 three, and bronze answers all of them. Spelled as empty lists rather
// than deleted from the table below, because the NEXT member ECMA-262 adds
// has to land here, and a null pointer that has to be re-derived is how it
// lands as `undefined` instead.
const char* const* const kArrayCtorUnimplemented = nullptr;
constexpr size_t kArrayCtorUnimplementedCount = 0;
const char* const* const kStringCtorUnimplemented = nullptr;
constexpr size_t kStringCtorUnimplementedCount = 0;

struct CtorEntry {
    const char* name;
    bronze_fn_code code;
    const StaticFn* statics;
    size_t staticCount;
    const char* const* unimplemented;
    size_t unimplementedCount;
    // The intrinsic prototype this constructor's instances have, or null when
    // its instances have no prototype OBJECT at all — which is what separates
    // the two kinds of constructor in this file. `Array` builds a value whose
    // members are answered beside it, so `new` can simply run the body: 23.1.1.1
    // reads NewTarget only to pick a prototype, which is why `Array(x)` and
    // `new Array(x)` agree. `String` and `Boolean` build a WRAPPER, which the
    // body cannot return at all (13.3.5.1 discards a primitive return in favour
    // of the plain instance), so `bronze_construct` builds it from this rather
    // than entering the body.
    Value (*prototype)();
    // Own properties this constructor carries that are not in the table above,
    // installed on the function object the first time anything asks for it.
    // `Number` is the one entry that has any: its fourteen statics are the
    // tables in builtin_number.cpp, and 21.1.2 makes them non-enumerable own
    // properties rather than the beside-the-value answers `Array.from` and
    // `String.fromCharCode` are — so a program can ask `'EPSILON' in Number`
    // and get the language's answer. Idempotent, because every route to a
    // constructor reaches the same interned function object.
    void (*decorate)(Rooted<Value>&);
};

// 28.2.1.2 Proxy.revocable. Revocation flips every internal method to a
// TypeError, which is a per-operation check the proxy kind does not carry —
// so the whole member is refused here, by name, rather than handed out as a
// pair whose `revoke` would quietly do nothing.
const StaticFn kProxyStatics[] = {
    {"revocable", rtProxyRevocable, 2},
};

const CtorEntry kCtors[] = {
    {"Array", arrayConstructor, kArrayStatics, std::size(kArrayStatics),
     kArrayCtorUnimplemented, kArrayCtorUnimplementedCount, nullptr, nullptr},
    {"String", stringConstructor, kStringStatics, std::size(kStringStatics),
     kStringCtorUnimplemented, kStringCtorUnimplementedCount, rtStringPrototype, nullptr},
    {"Boolean", booleanConstructor, nullptr, 0, nullptr, 0, rtBooleanPrototype, nullptr},
    // No unimplemented list: 21.1.2 names fifteen own properties and bronze
    // answers all fifteen (builtin_number.cpp says so at the table).
    {"Number", rtNumberConstructorBody, nullptr, 0, nullptr, 0, rtNumberPrototype,
     rtInstallNumberStatics},
    {"Proxy", rtProxyConstructor, kProxyStatics, std::size(kProxyStatics), nullptr, 0, nullptr,
     nullptr},
};

// Arity 0 for the constructors too, and here it decides an answer rather than
// an optimisation: `new Array(3)` padded to any fixed arity would arrive as
// `(3, undefined)`, take 23.1.1.1's element-list branch and produce `[3,
// undefined]` where the language says three holes.
//
// The `prototype` slot is filled in on first demand rather than left to
// `rtEnsureFunctionPrototype`, which mints a FRESH empty object for a function
// that has none — and that object is what `String.prototype` would read and
// what `instanceof` would compare against, so a wrapper would not be an
// `instanceof String`. Filling it here makes those one answer.
Value ctorObject(const CtorEntry& entry) {
    Rooted<Value> fn{rtNativeFunction(entry.code, 0)};
    if (entry.prototype && !fn.get().asObject<FunctionHeader>()->prototype.isObject()) {
        Rooted<Value> proto{entry.prototype()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        // Memoized on the prototype's identity, so repeating this read cannot
        // leak a shape per call. Nothing ever builds an object from it —
        // `bronze_construct` does not reach the ordinary instance path for
        // these two.
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }
    // The array constructor keeps its table slot NULL for the reason
    // `rtGlobalConstructorMember` names — an entry with one joins
    // `rtPrimitiveWrapperConstructorName`'s list — but its FunctionHeader slot
    // still has to hold %Array.prototype%, because generated code reads
    // `f.prototype` inline out of that slot (llvm_prop_get.cpp) and never
    // reaches the property path that knows better. Leaving it empty let the
    // first `new Array(...)` — or the species creation inside `[].map(...)` —
    // mint a FRESH object through `rtEnsureFunctionPrototype`, after which
    // `Array.prototype` was an empty object for the rest of the program and a
    // method installed on it was found by nothing.
    if (entry.code == arrayConstructor &&
        !fn.get().asObject<FunctionHeader>()->prototype.isObject()) {
        Rooted<Value> proto{rtArrayPrototypeObject()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }
    if (entry.decorate) entry.decorate(fn);
    return fn.get();
}

}  // namespace

// ---- the array-like protocol (7.3.18, 7.3.19, 23.1.2.1 step 3) --------------
//
// These three live here because `Array.from` is the member that has to tell the
// two protocols apart, and they are exported because it is not the only one:
// `Function.prototype.apply` and `Reflect.apply` build their argument list from
// CreateListFromArrayLike (7.3.19), and the nine typed-array constructors fall
// back to the array-like path when the source has no @@iterator (23.2.5.1 step
// 5.c). One answer to "is this iterable, and how long is it?" rather than four,
// because four is how they would come to disagree about `{length: 2}`.

bool rtHasIteratorMethod(Rooted<Value>& src) {
    if (src.get().isString()) return true;
    if (!src.get().isObject()) return false;
    const uint16_t flags = src.get().asObject<HeapObjectHeader>()->flags;
    if (flags == HeapKind::Array || flags == TypedArrayHeader::kFlags ||
        flags == MapHeader::kMapFlags ||
        flags == MapHeader::kSetFlags) {
        return true;
    }
    if (flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    Rooted<Value> key{rtIteratorKey()};
    const Value method = src.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return isCallable(method);
}

// 7.3.18 LengthOfArrayLike: ToLength of the `length` property. A missing
// `length` is ToLength(undefined), which is 0 — an empty result rather than an
// error, which is what the specification says and what
// `new Float64Array({valueOf(){return 4}})` therefore produces: a length-0
// view, because an object with a `valueOf` and no `length` is an array-like of
// no elements and not a number in disguise.
//
// Array and typed array read their own header because the answer is right
// there. EVERY OTHER object goes through the generic Get, which is what the
// step says and, unlike a direct `getProp`, is what reaches a `length` that
// lives behind a getter, up a prototype chain, or on a PROXY. The last of
// those is not hypothetical: a host object — an embedder's array-like, an
// interpreted array crossing a bridge — is not flagged plain, and the shape
// this replaces answered 0 for it silently. `f.apply(null, list)` then called
// with no arguments at all: not an error anywhere, just every parameter
// undefined, several frames from the `apply`.
//
// The read can run a getter, so the object arrives through a root.
uint32_t rtArrayLikeLength(Rooted<Value>& src) {
    if (!src.get().isObject()) return 0;
    const uint16_t flags = src.get().asObject<HeapObjectHeader>()->flags;
    if (flags == HeapKind::Array) return src.get().asObject<ArrayHeader>()->length;
    if (flags == TypedArrayHeader::kFlags) return src.get().asObject<TypedArrayHeader>()->length;
    Rooted<Value> key{rtMakeString("length")};
    const double len =
        rtToNumber(Value(bronze_elem_get(src.get().rawBits(), key.get().rawBits())));
    if (rtExceptionPending()) return 0;
    if (!(len >= 1.0)) return 0;
    return len > 4294967295.0 ? 4294967295u : static_cast<uint32_t>(len);
}

// 7.3.19 has no length cap and neither does the language; what has one is the
// machine. An argument list is a contiguous block of ROOTED slots, so
// `f.apply(null, {length: 4294967295})` would ask for 32 GB of them before a
// single one could be filled -- and `std::bad_alloc` unwinding out of a helper
// generated code called is the one failure mode this runtime must not have
// (see `arrayLengthFits` above, which refuses for the same reason).
//
// So the block is bounded, and the bound is named in the refusal rather than
// left as a crash. It is far above any real call: an argument list is written
// by a program, not generated, and 65535 is the count past which every engine
// in use also refuses.
bool rtCheckAppliedArgumentCount(uint32_t count, const char* member) {
    if (count <= kMaxAppliedArguments) return true;
    rtThrowRangeError(std::string(member) + ": an argument list of " + std::to_string(count) +
                      " does not fit in a call frame (the limit is " +
                      std::to_string(kMaxAppliedArguments) + ")");
    return false;
}

// One element of an array-like, by index. This is Get(obj, ToString(index)),
// which is what makes a HOLE and a missing index both read as `undefined` —
// 7.3.19 has no notion of a hole and neither does the argument list it builds.
Value rtArrayLikeElement(Rooted<Value>& src, uint32_t index) {
    return Value(bronze_elem_get(src.get().rawBits(), Value::fromDouble(index).rawBits()));
}

Value rtGlobalConstructor(const std::string& name) {
    for (const CtorEntry& entry : kCtors) {
        if (name == entry.name) return ctorObject(entry);
    }
    return Value::fromUndefined();
}

Value rtArrayConstructorObject() { return ctorObject(kCtors[0]); }

Value rtStringConstructorObject() { return ctorObject(kCtors[1]); }

Value rtBooleanConstructorObject() { return ctorObject(kCtors[2]); }

Value rtNumberConstructorObject() { return ctorObject(kCtors[3]); }

const char* rtPrimitiveWrapperConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code && entry.prototype) return entry.name;
    }
    return nullptr;
}

const char* rtIntrinsicConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code == code) return entry.name;
    }
    return nullptr;
}

// The intrinsic constructors bronze builds NO prototype object for. Their
// `prototype` is a named refusal on the property path (rt_prop.cpp), and the
// answer holds only while the FunctionHeader slot stays EMPTY: generated code
// reads that slot inline, so anything that fills it turns the refusal into a
// fresh empty object silently.
const char* rtNoPrototypeObjectIntrinsic(Value fn) {
    const char* name = rtMapConstructorName(fn);
    if (!name) name = rtWeakCollectionConstructorName(fn);
    if (!name) name = rtWeakRefConstructorName(fn);
    if (!name) name = rtTypedArrayConstructorName(fn);
    if (!name) name = rtSharedArrayBufferConstructorName(fn);
    if (!name) name = rtDataViewConstructorName(fn);
    return name;
}

bool rtIsArrayConstructor(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == kCtors[0].code;
}

bool rtIsArrayBufferConstructor(Value fn) {
    if (const char* name = rtTypedArrayConstructorName(fn)) {
        return std::strcmp(name, "ArrayBuffer") == 0;
    }
    return false;
}

bool rtIsTypedArrayConstructor(Value fn) {
    if (const char* name = rtTypedArrayConstructorName(fn)) {
        return std::strcmp(name, "ArrayBuffer") != 0;
    }
    return false;
}

// `rtIsRegExpConstructor` lives in builtin_regexp.cpp beside the body it
// compares against, for the reason `rtIsArrayConstructor` above compares a code
// pointer and not an object: identifying an intrinsic must not BUILD it. The
// version that lived here answered by materialising %RegExp% and comparing
// addresses, which made every caller an allocation site — and one of those
// callers is the function-object miss ladder in rt_prop.cpp, where an
// unexpected collection retires the property box mid-lookup.

bool rtGlobalConstructorMember(Value fn, const std::string& key, Value& out) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code) continue;
        // 22.1.2.3 / 20.3.2.1, ahead of the statics and of the function's own
        // prototype slot further down the property path. The slot holds the
        // same object, so this is about where the answer is WRITTEN rather than
        // what it is: one line here keeps `Array.prototype`'s named refusal
        // legible beside the two that now answer.
        if (entry.prototype && key == "prototype") {
            out = entry.prototype();
            return true;
        }
        // 23.1.3.4's holder, answered HERE and not through the entry's
        // prototype slot: filling that slot would put `Array` on
        // `rtPrimitiveWrapperConstructorName`'s list — it reports any entry
        // with a prototype — and `new Array` would build a wrapper instead of
        // running 23.1.1.1.
        if (key == "prototype" && entry.code == arrayConstructor) {
            out = rtArrayPrototypeObject();
            return true;
        }
        for (size_t i = 0; i < entry.staticCount; ++i) {
            if (key == entry.statics[i].name) {
                out = rtNativeFunction(entry.statics[i].code,
                                                       entry.statics[i].arity);
                return true;
            }
        }
        // Diagnoses and does not return for a name on the list, which is what
        // keeps `Array.prototype` from being answered by the FunctionHeader's
        // own prototype slot further down the property path. Anything else
        // falls through to the ordinary function-member treatment, so
        // `Array.call` stays the named error `Function.prototype` gives it.
        rtCheckUnimplementedMember(entry.name, entry.unimplemented, entry.unimplementedCount,
                                   key);
        return false;
    }
    return false;
}

bool rtGlobalConstructorStaticMatches(bronze_fn_code ctorCode, const std::string& key,
                                      bronze_fn_code calleeCode) noexcept {
    // The exotic method-IC latch's witness (rt_method_call.cpp): does
    // (ctorCode, key) name a static in the table above whose body IS
    // calleeCode? A pure walk over the same C arrays the read path answers
    // from — comparing code pointers rather than function objects, because the
    // latch runs where an allocation would invalidate its caller's raw
    // pointers and interning an object is exactly an allocation.
    if (!ctorCode || !calleeCode) return false;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != ctorCode) continue;
        for (size_t i = 0; i < entry.staticCount; ++i) {
            if (key == entry.statics[i].name) return entry.statics[i].code == calleeCode;
        }
        return false;
    }
    return false;
}

bool rtInstallGlobalConstructorStatics(Rooted<Value>& ctor) {
    if (!ctor.get().isObject() ||
        ctor.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = ctor.get().asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code || entry.staticCount == 0) continue;
        rtEnsureFunctionProperties(ctor);
        Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
        if (!props.get().isObject()) return false;
        for (size_t i = 0; i < entry.staticCount; ++i) {
            // The SAME interned function object the read path hands out, so
            // `Array.of === MyArr.of` however either was reached.
            Rooted<Value> key{rtMakeString(entry.statics[i].name)};
            Rooted<Value> fn{rtNativeFunction(entry.statics[i].code, entry.statics[i].arity)};
            props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn,
                                                         /*ic=*/nullptr, /*enumerable=*/false,
                                                         /*defineOwn=*/true);
        }
        // `prototype` is deliberately NOT among them: it is the base's own
        // object and a subclass has one of its own, so copying it here would
        // put %Array.prototype% on a chain `MyArr.prototype` already sits at
        // the bottom of.
        return true;
    }
    return false;
}

}  // namespace bronze::runtime
