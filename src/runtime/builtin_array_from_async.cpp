// 23.1.2.1 `Array.fromAsync` (ES2024): `Array.from` over an ASYNC iterable,
// answering a promise for the array. The spec writes it as an async abstract
// closure with an `await` at each of three points — the result of `next()`, the
// value a sync source yields, and the mapper's return — and a native builtin
// has no compiled state machine to suspend at them. So the closure is written
// the way the machine would have compiled it: one STATE record (a plain object
// with internal slots, invisible to the program) holding everything the loop
// carries across a tick, and one driver that runs the synchronous stretch
// between two awaits and subscribes a continuation to the promise it stops at.
// Each continuation is a native closure over the record, so no C++ frame lives
// across a job-queue turn and nothing the collector moves is held anywhere but
// a slot it walks.
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

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/builtin_constructors_internal.h"
#include "runtime/exception.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

using namespace ctor_internal;

namespace {

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

// IfAbruptCloseAsyncIterator / IfAbruptCloseIterator: the throw the caller
// holds is the completion the program sees, so the iterator's `return` runs
// with its own errors discarded. An async `return` answers a promise the spec
// would await; nothing observable of this operation depends on when it
// settles, so it is not waited for.
void fromAsyncCloseSource(Rooted<Value>& state) {
    const uint32_t mode = fromAsyncMode(state);
    if (mode == 1) {
        Rooted<Value> rec{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return;
    }
    if (mode != 0) return;
    Value ignored;
    rtTryCatch(
        [&] {
            Rooted<Value> iter{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
            Rooted<Value> key{rtMakeString("return")};
            Rooted<Value> ret{Value(bronze_elem_get(iter.get().rawBits(), key.get().rawBits()))};
            if (isCallable(ret.get())) {
                ret.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr);
            }
        },
        ignored);
}

void fromAsyncReject(Rooted<Value>& state, Rooted<Value>& reason) {
    Rooted<Value> cap{fromAsyncSlot(state, FromAsyncSlot::Capability)};
    rtSettleCapability(cap, reason, /*reject=*/true);
}

// Runs one synchronous stretch of the operation. A throw out of it rejects the
// operation's promise — after closing the source, where `close` says the
// spec's IfAbruptClose applies — and answers false; a normal completion
// answers true.
template <typename Body>
bool fromAsyncStep(Rooted<Value>& state, bool close, Body&& body) {
    Value thrown;
    if (!rtTryCatch(std::forward<Body>(body), thrown)) return true;
    Rooted<Value> reason{thrown};
    if (close) fromAsyncCloseSource(state);
    fromAsyncReject(state, reason);
    return false;
}

// `Await(v)`: PromiseResolve(%Promise%, v), then the two continuations. The
// rejection side closes the source only where the spec's IfAbruptClose does
// — around the mapper's await and the sync source's value — and not around
// `next()` itself, whose rejection is the iterator's own.
void fromAsyncAwait(Rooted<Value>& state, Rooted<Value>& v, NativeFunctionCode onFulfilled,
                    bool closeOnReject) {
    Rooted<Value> promise{Value::fromUndefined()};
    if (!fromAsyncStep(state, false, [&] { promise.set(rtPromiseResolveValue(v)); })) return;
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
    if (!fromAsyncStep(state, false, [&] { setResultLength(out, k, constructed); })) return;
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
        Rooted<Value> result{Value::fromUndefined()};
        if (!fromAsyncStep(state, false, [&] {
                result.set(next.get().asObject<FunctionHeader>()->call(iter.get(), 0, nullptr));
            })) {
            return;
        }
        fromAsyncAwait(state, result, fromAsyncOnNextResult, /*closeOnReject=*/false);
        return;
    }
    if (mode == 1) {
        Rooted<Value> rec{fromAsyncSlot(state, FromAsyncSlot::Iterator)};
        bool more = false;
        if (!fromAsyncStep(state, false, [&] { more = bronze_iter_step(rec.get().rawBits()); })) {
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
    Rooted<Value> value{Value::fromUndefined()};
    if (!fromAsyncStep(state, false, [&] {
            value.set(
                Value(bronze_elem_get(source.get().rawBits(), Value::fromDouble(k).rawBits())));
        })) {
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
    Rooted<Value> value{Value::fromUndefined()};
    bool done = false;
    if (!fromAsyncStep(state, false, [&] {
            if (!result.get().isObject()) {
                rtThrowTypeError(
                    "Array.fromAsync: the async iterator's next() result is not an object");
            }
            Rooted<Value> doneKey{rtMakeString("done")};
            done = bronze_truthy(bronze_elem_get(result.get().rawBits(), doneKey.get().rawBits()));
            if (done) return;
            Rooted<Value> valueKey{rtMakeString("value")};
            value.set(Value(bronze_elem_get(result.get().rawBits(), valueKey.get().rawBits())));
        })) {
        return Value::fromUndefined().rawBits();
    }
    if (done) {
        fromAsyncFinish(state);
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
    // 5.k.i.5.b.ii / 6.e.iii.b.ii: a mapper that throws closes the source
    // before the rejection.
    Rooted<Value> mapped{Value::fromUndefined()};
    if (!fromAsyncStep(state, true,
                       [&] { mapped.set(callMapper(mapFn, thisArg, value, k)); })) {
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
    if (!fromAsyncStep(state, true, [&] { emitAt(out, k, value, constructed); })) {
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
    fromAsyncCloseSource(state);
    fromAsyncReject(state, reason);
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

}  // namespace

uint64_t rtArrayFromAsyncBuiltin(uint64_t, uint64_t thisBits, uint32_t argc,
                                 const uint64_t* argv) {
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

    // Every throw from here to the first await is a rejection. The source is
    // not closed on any of them: none follows a successful open.
    const bool started = fromAsyncStep(state, false, [&] {
        // 3.a-b: a mapper that is present and not callable.
        if (!mapFn.get().isUndefined() && !isCallable(mapFn.get())) {
            rtThrowTypeError("Array.fromAsync: the second argument is not a function");
        }
        // 3.c-d: GetMethod(asyncItems, @@asyncIterator), then @@iterator.
        // GetMethod of null or undefined is the TypeError that makes
        // `fromAsync(null)` a rejection.
        if (src.get().isNull() || src.get().isUndefined()) {
            rtThrowTypeError("Array.fromAsync requires an array-like or iterable object, not " +
                             rtIterableKindName(src.get()));
        }
        Rooted<Value> asyncMethod{Value::fromUndefined()};
        if (src.get().isObject()) {
            Rooted<Value> key{rtAsyncIteratorKey()};
            asyncMethod.set(Value(bronze_elem_get(src.get().rawBits(), key.get().rawBits())));
        }
        const bool constructed = buildsThroughThis(ctor.get());
        setFromAsyncSlot(state, FromAsyncSlot::Constructed,
                         Value::fromDouble(constructed ? 1 : 0));

        if (isCallable(asyncMethod.get())) {
            // 3.e-h: the async iterator, and Construct(C) with no length.
            Rooted<Value> iter{
                asyncMethod.get().asObject<FunctionHeader>()->call(src.get(), 0, nullptr)};
            if (!iter.get().isObject()) {
                rtThrowTypeError("the result of Symbol.asyncIterator is not an object");
            }
            Rooted<Value> nextKey{rtMakeString("next")};
            Rooted<Value> next{
                Value(bronze_elem_get(iter.get().rawBits(), nextKey.get().rawBits()))};
            Rooted<Value> out{constructed ? constructThrough(ctor, nullptr) : newEmptyArray()};
            setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(0));
            setFromAsyncSlot(state, FromAsyncSlot::Iterator, iter.get());
            setFromAsyncSlot(state, FromAsyncSlot::NextFn, next.get());
            setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
            return;
        }
        if (rtHasIteratorMethod(src)) {
            Rooted<Value> rec{Value(bronze_iter_open(src.get().rawBits()))};
            Rooted<Value> out{constructed ? constructThrough(ctor, nullptr) : newEmptyArray()};
            setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(1));
            setFromAsyncSlot(state, FromAsyncSlot::Iterator, rec.get());
            setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
            return;
        }
        // 3.i-k: the array-like, and Construct(C, « len »).
        const uint32_t len = rtArrayLikeLength(src);
        Rooted<Value> out{constructed ? constructThrough(ctor, &len) : newEmptyArray()};
        setFromAsyncSlot(state, FromAsyncSlot::Mode, Value::fromDouble(2));
        setFromAsyncSlot(state, FromAsyncSlot::Source, src.get());
        setFromAsyncSlot(state, FromAsyncSlot::Length, Value::fromDouble(len));
        setFromAsyncSlot(state, FromAsyncSlot::Out, out.get());
    });
    if (started) fromAsyncResume(state);
    return promise.get().rawBits();
}

}  // namespace bronze::runtime
