// The Promise INTRINSIC OBJECTS (ECMA-262 27.2.3-27.2.5): the constructor and
// its executor protocol, `Promise.prototype`'s three methods, and the six
// statics. The abstract machinery they all sit on — states, reactions,
// resolution, jobs — is promise.cpp, reached through promise.h.
//
// The arrangement is the Error family's: the prototype is a real object built
// with a root shape, its methods are non-enumerable own properties, the
// constructor carries `prototype` / `instance_shape` / a `constructor`
// back-pointer, and one initializer builds both because each holds the other.
//
// SUBCLASSING runs through two hooks that have to agree, and both are here.
// `Promise.resolve` and the six statics construct through `this` (27.2.4.7.1
// PromiseResolve(C, x)), so `MyPromise.resolve(1)` is a MyPromise; `then`,
// `catch` and `finally` build their result through SpeciesConstructor
// (7.3.20) and NewPromiseCapability (27.2.1.5), so a chain started on a
// subclass stays in it. Both funnel through promise.h's capability record, and
// both take a one-compare fast path — "this promise's prototype is
// %Promise.prototype%" — for every promise a program did not subclass.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

thread_local Value g_promiseCtor = Value::fromUndefined();
thread_local Value g_promiseProto = Value::fromUndefined();
thread_local Shape* g_instanceShape = nullptr;

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// First-use registration, for the initialization-order reason
// ensureExceptionRoots records.
void ensurePromiseRoots() {
    static thread_local const bool registered = [] {
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            visit(g_promiseCtor);
            visit(g_promiseProto);
        });
        return true;
    }();
    (void)registered;
}

// A plain-shaped object used only as a closure's ENVIRONMENT: internal slots
// carry the captures, so nothing a program can reach ever sees them.
Value makeEnvObject(uint32_t slotCount) {
    ObjectHeader* env = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(),
                                                              rtPlainObjectShape(), slotCount);
    env->header.flags = HeapKind::Plain;
    return Value::fromObject(env);
}

// ---- the constructor (27.2.3.1) ---------------------------------------------

uint64_t promiseConstructorBody(uint64_t, uint64_t thisBits, uint32_t argc,
                                const uint64_t* argv) {
    // Step 1: NewTarget undefined is a TypeError. The uniform convention
    // cannot see NewTarget, but it can see the RECEIVER the running
    // construction allocated — 27.2.3.1 step 3 is
    // OrdinaryCreateFromConstructor over NewTarget, and bronze performs it at
    // the one site every `new` goes through, so this body is handed the promise
    // it has to fill. `Promise(f)` without `new` has no such receiver.
    Rooted<Value> promise{Value(thisBits)};
    if (!rtIsNativeConstructReceiver(promise.get()) || !rtIsPromiseObject(promise.get())) {
        return rtThrowTypeError("Constructor Promise requires 'new'").rawBits();
    }
    RootedArgs args{argc, argv};
    if (!isCallable(args[0])) {
        return rtThrowTypeError("Promise resolver is not a function").rawBits();
    }
    Rooted<Value> executor{args[0]};
    Rooted<Value> resolveFn{rtMakeResolvingFunction(promise, /*isReject=*/false)};
    Rooted<Value> rejectFn{rtMakeResolvingFunction(promise, /*isReject=*/true)};

    uint64_t block[2] = {resolveFn.get().rawBits(), rejectFn.get().rawBits()};
    bronze_dynamic_call(executor.get().rawBits(), Value::fromUndefined().rawBits(), 2, block);
    if (rtExceptionPending()) {
        // Step 10: the executor's throw is the promise's rejection — through
        // the reject function's latch, so an executor that resolved and THEN
        // threw keeps its first answer.
        Rooted<Value> thrown{Value(bronze_tls_block_addr()->exception_cell)};
        rtClearException();
        rtRejectPromise(promise, thrown);
    }
    return promise.get().rawBits();
}

// ---- Promise.prototype.then / catch / finally -------------------------------

uint64_t promiseThen(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> self{Value(thisBits)};
    RootedArgs args{argc, argv};
    // 27.2.5.4 step 2 is the BRAND — an object allocated as a promise — and not
    // the identity of its prototype: a subclass instance is a promise and every
    // one of 27.2.5's methods works on it.
    if (!rtIsPromiseObject(self.get())) {
        return rtThrowTypeError(
                   "Promise.prototype.then called on a value that is not a promise")
            .rawBits();
    }
    // Step 3: SpeciesConstructor(promise, %Promise%), then NewPromiseCapability
    // over it — so `sub.then(...)` answers a `sub` and
    // `static get [Symbol.species]() { return Promise }` opts back out.
    Rooted<Value> species{rtPromiseSpeciesConstructor(self)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> cap{rtNewPromiseCapability(species)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> onF{args[0]};
    Rooted<Value> onR{args[1]};
    rtPerformPromiseThen(self, onF, onR, cap);
    return rtCapabilityPromise(cap.get()).rawBits();
}

// 27.2.5.1: `then(undefined, onRejected)`. The spec spells Invoke(this,
// "then") — an own `then` a program stuck on the instance would be honoured
// there. bronze calls its own `then` directly: an instance's shadowing `then`
// is a divergence this file accepts once for all three members rather than
// re-dispatching per call.
uint64_t promiseCatch(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    uint64_t forwarded[2] = {Value::fromUndefined().rawBits(), args[0].rawBits()};
    return promiseThen(0, thisBits, 2, forwarded);
}

// The two 27.2.5.3.1 thunks. Each returns a promise that waits for
// onFinally's result and then reinstates the ORIGINAL completion — value
// passed through, reason re-thrown — which is what makes `finally` observe
// without participating.

uint64_t valueThunk(uint64_t env, uint64_t, uint32_t, const uint64_t*) {
    return env;  // the captured value, unchanged
}

uint64_t reasonThrower(uint64_t env, uint64_t, uint32_t, const uint64_t*) {
    return rtThrow(Value(env)).rawBits();
}

uint64_t finallyStep(uint64_t env, uint32_t argc, const uint64_t* argv, bool rethrow) {
    RootedArgs args{argc, argv};
    Rooted<Value> onFinally{Value(env)};
    Rooted<Value> completion{args[0]};
    Rooted<Value> result{
        Value(bronze_dynamic_call(onFinally.get().rawBits(),
                                  Value::fromUndefined().rawBits(), 0, nullptr))};
    // onFinally's throw replaces the completion (27.2.5.3.1 has no step
    // catching it): left pending, the reaction job running this thunk rejects
    // its capability with it.
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> inner{rtPromiseResolveValue(result)};
    Rooted<Value> restore{
        rtMakeNativeClosure(rethrow ? reasonThrower : valueThunk, completion, 0)};
    Rooted<Value> noHandler{Value::fromUndefined()};
    Rooted<Value> cap{rtNewPromiseCapabilityForIntrinsic()};
    rtPerformPromiseThen(inner, restore, noHandler, cap);
    // Returning this promise makes the OUTER capability adopt it, which is
    // exactly the wait 27.2.5.3.1 step 6 spells as `then(thenFinally)`.
    return rtCapabilityPromise(cap.get()).rawBits();
}

uint64_t thenFinally(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    return finallyStep(env, argc, argv, /*rethrow=*/false);
}

uint64_t catchFinally(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    return finallyStep(env, argc, argv, /*rethrow=*/true);
}

uint64_t promiseFinally(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> self{Value(thisBits)};
    RootedArgs args{argc, argv};
    // The BRAND, like `then` above it: 27.2.5.3 step 2 requires an Object and
    // leaves the promise-ness to the `then` it forwards to, so a SUBCLASS
    // instance must pass here — `rtIsPromise`, which asks whether the prototype
    // is %Promise.prototype%, would refuse every one of them.
    if (!rtIsPromiseObject(self.get())) {
        return rtThrowTypeError(
                   "Promise.prototype.finally called on a value that is not a promise")
            .rawBits();
    }
    Rooted<Value> onF{Value::fromUndefined()};
    Rooted<Value> onR{Value::fromUndefined()};
    if (isCallable(args[0])) {
        // 27.2.5.3 step 5: both sides run the same callback, each through the
        // thunk that restores its own completion afterwards.
        Rooted<Value> onFinally{args[0]};
        onF.set(rtMakeNativeClosure(thenFinally, onFinally, 1));
        onR.set(rtMakeNativeClosure(catchFinally, onFinally, 1));
    } else {
        // Step 4: a non-callable onFinally is passed straight through to
        // `then`, where it counts as absent — `finally(42)` is `then()`.
        onF.set(args[0]);
        onR.set(args[0]);
    }
    Rooted<Value> species{rtPromiseSpeciesConstructor(self)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> cap{rtNewPromiseCapability(species)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    rtPerformPromiseThen(self, onF, onR, cap);
    return rtCapabilityPromise(cap.get()).rawBits();
}

// ---- the combinators (27.2.4.1-27.2.4.6) ------------------------------------
//
// One skeleton, three finishes. Each element subscribes NO-capability
// reactions closing over a SHARED record (the capability, the results array,
// the remaining count) plus its own index — because a pass-through reaction
// with the capability would settle it with each element's value, which is
// `race` and only `race`. `race` therefore IS the pass-through form, and the
// promise-level latch on the capability is what makes its first settle win.

namespace SharedSlot {
enum : uint32_t { Capability, Values, Remaining, Kind, kCount };
}
namespace ElementSlot {
enum : uint32_t { Shared, Index, kCount };
}

enum CombinatorKind : uint32_t { kAll, kAllSettled, kAny };

double sharedRemaining(Value shared) {
    return shared.asObject<ObjectHeader>()->internalSlot(SharedSlot::Remaining).asNumber();
}

// The last element (or the driver, for the final decrement) finishes the
// combinator: `all` and `allSettled` resolve with the results array, `any` —
// whose results are the REASONS — rejects with an AggregateError over them.
void combinatorFinish(Rooted<Value>& shared) {
    Rooted<Value> cap{shared.get().asObject<ObjectHeader>()->internalSlot(SharedSlot::Capability)};
    Rooted<Value> values{shared.get().asObject<ObjectHeader>()->internalSlot(SharedSlot::Values)};
    const auto kind = static_cast<uint32_t>(
        shared.get().asObject<ObjectHeader>()->internalSlot(SharedSlot::Kind).asNumber());
    if (kind == kAny) {
        // 27.2.4.3 step 4 via 27.2.4.3.1 step 8.c: the message is the spec's.
        Rooted<Value> err{rtNewErrorValue(ErrorKind::AggregateError,
                                          "All promises were rejected")};
        Rooted<Value> errorsKey{rtMakeString("errors")};
        err.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), errorsKey, values,
                                                    /*ic=*/nullptr, /*enumerable=*/false);
        rtSettleCapability(cap, err, /*reject=*/true);
        return;
    }
    rtSettleCapability(cap, values, /*reject=*/false);
}

void combinatorDecrement(Rooted<Value>& shared) {
    const double remaining = sharedRemaining(shared.get()) - 1;
    shared.get().asObject<ObjectHeader>()->setInternalSlot(SharedSlot::Remaining,
                                                           Value::fromDouble(remaining));
    if (remaining == 0) combinatorFinish(shared);
}

// Store `v` at this element's index. In range by construction: the driver
// appended an `undefined` placeholder per element (27.2.4.1.2 step 8.j), so
// settling out of order never writes past `length`.
void elementStore(Rooted<Value>& element, Rooted<Value>& v) {
    Rooted<Value> shared{element.get().asObject<ObjectHeader>()->internalSlot(ElementSlot::Shared)};
    Rooted<Value> values{shared.get().asObject<ObjectHeader>()->internalSlot(SharedSlot::Values)};
    Value index = element.get().asObject<ObjectHeader>()->internalSlot(ElementSlot::Index);
    bronze_elem_set(values.get().rawBits(), index.rawBits(), v.get().rawBits(),
                    /*strict=*/false);
}

Value elementShared(Value element) {
    return element.asObject<ObjectHeader>()->internalSlot(ElementSlot::Shared);
}

uint64_t allOnFulfilled(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> element{Value(env)};
    Rooted<Value> v{args[0]};
    elementStore(element, v);
    Rooted<Value> shared{elementShared(element.get())};
    combinatorDecrement(shared);
    return Value::fromUndefined().rawBits();
}

// `{ status, value }` / `{ status, reason }` in that key order — creation
// order is enumeration order, and allSettled's result objects are exactly
// what a program prints.
uint64_t settledElement(uint64_t env, uint32_t argc, const uint64_t* argv, bool rejected) {
    RootedArgs args{argc, argv};
    Rooted<Value> element{Value(env)};
    Rooted<Value> completion{args[0]};
    Rooted<Value> record{Value(bronze_create_object())};
    Rooted<Value> statusKey{rtMakeString("status")};
    Rooted<Value> statusVal{rtMakeString(rejected ? "rejected" : "fulfilled")};
    record.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), statusKey, statusVal);
    Rooted<Value> completionKey{rtMakeString(rejected ? "reason" : "value")};
    record.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), completionKey,
                                                   completion);
    elementStore(element, record);
    Rooted<Value> shared{elementShared(element.get())};
    combinatorDecrement(shared);
    return Value::fromUndefined().rawBits();
}

uint64_t settledOnFulfilled(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    return settledElement(env, argc, argv, /*rejected=*/false);
}

uint64_t settledOnRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    return settledElement(env, argc, argv, /*rejected=*/true);
}

uint64_t anyOnRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> element{Value(env)};
    Rooted<Value> reason{args[0]};
    elementStore(element, reason);
    Rooted<Value> shared{elementShared(element.get())};
    combinatorDecrement(shared);
    return Value::fromUndefined().rawBits();
}

// The pass-through halves whose env is the CAPABILITY itself: `all`'s
// rejection, `any`'s fulfillment. The capability's latch makes the first
// caller win, so many elements may share the one function value.
uint64_t capResolve(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> cap{Value(env)};
    Rooted<Value> v{args[0]};
    rtSettleCapability(cap, v, /*reject=*/false);
    return Value::fromUndefined().rawBits();
}

uint64_t capReject(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> cap{Value(env)};
    Rooted<Value> reason{args[0]};
    rtSettleCapability(cap, reason, /*reject=*/true);
    return Value::fromUndefined().rawBits();
}

// Take the pending exception and reject the capability with it — the
// IfAbruptRejectPromise every combinator wraps its iteration in.
void rejectWithPending(Rooted<Value>& cap) {
    Rooted<Value> thrown{Value(bronze_tls_block_addr()->exception_cell)};
    rtClearException();
    rtSettleCapability(cap, thrown, /*reject=*/true);
}

// `ctor` is the combinator's `this` (27.2.4.1 step 1: "Let C be the this
// value"), so `MyPromise.all([...])` answers a MyPromise and every element is
// adopted through `PromiseResolve(C, x)` rather than through the intrinsic.
uint64_t runCombinator(Rooted<Value>& ctor, uint32_t argc, const uint64_t* argv,
                       uint32_t kind) {
    RootedArgs args{argc, argv};
    Rooted<Value> source{args[0]};
    Rooted<Value> cap{rtNewPromiseCapability(ctor)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> values{Value(bronze_create_array(0))};
    Rooted<Value> shared{makeEnvObject(SharedSlot::kCount)};
    {
        ObjectHeader* env = shared.get().asObject<ObjectHeader>();
        env->setInternalSlot(SharedSlot::Capability, cap.get());
        env->setInternalSlot(SharedSlot::Values, values.get());
        // Starts at 1 and the driver decrements once after the loop
        // (27.2.4.1.2): the extra count is what keeps an already-settled
        // element from finishing the combinator while iteration is mid-walk.
        env->setInternalSlot(SharedSlot::Remaining, Value::fromDouble(1));
        env->setInternalSlot(SharedSlot::Kind, Value::fromDouble(kind));
    }

    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) {
        rejectWithPending(cap);
        return rtCapabilityPromise(cap.get()).rawBits();
    }
    double index = 0;
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        Rooted<Value> undef{Value::fromUndefined()};
        bronze_array_append(values.get().rawBits(), undef.get().rawBits());
        Rooted<Value> p{rtPromiseResolveWith(ctor, item)};
        if (rtExceptionPending()) break;
        Rooted<Value> element{makeEnvObject(ElementSlot::kCount)};
        element.get().asObject<ObjectHeader>()->setInternalSlot(ElementSlot::Shared,
                                                                shared.get());
        element.get().asObject<ObjectHeader>()->setInternalSlot(ElementSlot::Index,
                                                                Value::fromDouble(index));
        Rooted<Value> onF{Value::fromUndefined()};
        Rooted<Value> onR{Value::fromUndefined()};
        switch (kind) {
            case kAll:
                onF.set(rtMakeNativeClosure(allOnFulfilled, element, 1));
                onR.set(rtMakeNativeClosure(capReject, cap, 1));
                break;
            case kAllSettled:
                onF.set(rtMakeNativeClosure(settledOnFulfilled, element, 1));
                onR.set(rtMakeNativeClosure(settledOnRejected, element, 1));
                break;
            default:  // kAny
                onF.set(rtMakeNativeClosure(capResolve, cap, 1));
                onR.set(rtMakeNativeClosure(anyOnRejected, element, 1));
                break;
        }
        Rooted<Value> noCap{Value::fromUndefined()};
        rtPerformPromiseThen(p, onF, onR, noCap);
        shared.get().asObject<ObjectHeader>()->setInternalSlot(
            SharedSlot::Remaining, Value::fromDouble(sharedRemaining(shared.get()) + 1));
        index += 1;
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        rejectWithPending(cap);
        return rtCapabilityPromise(cap.get()).rawBits();
    }
    combinatorDecrement(shared);
    return rtCapabilityPromise(cap.get()).rawBits();
}

// ---- the statics ------------------------------------------------------------

// 27.2.4.7: `this` is the constructor the result is built through, which is
// the hook that makes `MyPromise.resolve(1)` a MyPromise.
uint64_t staticResolve(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> ctor{Value(thisBits)};
    Rooted<Value> v{args[0]};
    return rtPromiseResolveWith(ctor, v).rawBits();
}

uint64_t staticReject(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> ctor{Value(thisBits)};
    Rooted<Value> reason{args[0]};
    Rooted<Value> cap{rtNewPromiseCapability(ctor)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    // No pass-through here, unlike `resolve`: 27.2.4.6 always mints a new
    // rejected promise, even for a promise argument.
    rtSettleCapability(cap, reason, /*reject=*/true);
    return rtCapabilityPromise(cap.get()).rawBits();
}

uint64_t staticAll(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> ctor{Value(thisBits)};
    return runCombinator(ctor, argc, argv, kAll);
}

uint64_t staticAllSettled(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> ctor{Value(thisBits)};
    return runCombinator(ctor, argc, argv, kAllSettled);
}

uint64_t staticAny(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> ctor{Value(thisBits)};
    return runCombinator(ctor, argc, argv, kAny);
}

uint64_t staticRace(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> ctor{Value(thisBits)};
    Rooted<Value> source{args[0]};
    Rooted<Value> cap{rtNewPromiseCapability(ctor)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) {
        rejectWithPending(cap);
        return rtCapabilityPromise(cap.get()).rawBits();
    }
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        Rooted<Value> p{rtPromiseResolveWith(ctor, item)};
        if (rtExceptionPending()) break;
        // The pass-through form: absent handlers with the capability, so
        // every element's settle tries to settle the capability directly and
        // the latch keeps the first (27.2.4.5.1 hands each element the SAME
        // resolving pair).
        Rooted<Value> noHandler{Value::fromUndefined()};
        rtPerformPromiseThen(p, noHandler, noHandler, cap);
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        rejectWithPending(cap);
    }
    return rtCapabilityPromise(cap.get()).rawBits();
}

// 27.2.4.8 Promise.withResolvers(). One capability, handed out as an object
// instead of being hidden inside an executor — which is the whole point of the
// member: `const {promise, resolve} = Promise.withResolvers()` settles a
// promise from OUTSIDE the closure the constructor form forces you to write in.
//
// The two functions are built here rather than read off the capability record
// because for %Promise% the record's function slots are deliberately EMPTY
// (promise.h says why: `rtSettleCapability` goes through the promise's own
// latch, and per-`then` resolving functions would buy nothing observable).
// Nothing observed them before this member; `withResolvers` hands them to the
// program, so on the intrinsic path they have to exist as real function
// objects, and they are exactly the pair the executor form is given.
//
// The three properties are ORDINARY data properties — enumerable, writable,
// configurable — because 27.2.4.8 steps 4-6 are CreateDataPropertyOrThrow and
// not the non-enumerable definition every intrinsic's own members get. So
// `Object.keys(Promise.withResolvers())` really is ["promise","resolve","reject"].
uint64_t staticWithResolvers(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> ctor{Value(thisBits)};
    // Step 2's NewPromiseCapability(C) requires IsConstructor(C). A detached
    // `const f = Promise.withResolvers; f()` arrives with no receiver at all,
    // and `rtNewPromiseCapability` reads `undefined` as "the intrinsic" — which
    // would answer a promise for a call the language refuses.
    if (!ctor.get().isObject()) {
        return rtThrowTypeError("Promise.withResolvers called on a value that is not a "
                                "constructor")
            .rawBits();
    }
    Rooted<Value> cap{rtNewPromiseCapability(ctor)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> promise{rtCapabilityPromise(cap.get())};
    Rooted<Value> resolveFn{
        cap.get().asObject<ObjectHeader>()->internalSlot(CapabilitySlot::Resolve)};
    Rooted<Value> rejectFn{
        cap.get().asObject<ObjectHeader>()->internalSlot(CapabilitySlot::Reject)};
    if (!isCallable(resolveFn.get())) resolveFn.set(rtMakeResolvingFunction(promise, false));
    if (!isCallable(rejectFn.get())) rejectFn.set(rtMakeResolvingFunction(promise, true));

    Rooted<Value> out{Value(bronze_create_object())};
    struct Entry {
        const char* key;
        Rooted<Value>* value;
    };
    const Entry entries[] = {
        {"promise", &promise}, {"resolve", &resolveFn}, {"reject", &rejectFn}};
    for (const Entry& e : entries) {
        Rooted<Value> key{rtMakeString(e.key)};
        out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, *e.value);
    }
    return out.get().rawBits();
}

// ---- building the intrinsics ------------------------------------------------

void ensurePromiseIntrinsics() {
    if (g_promiseProto.isObject()) return;
    ensurePromiseRoots();

    // The prototype, with its [[Prototype]] named: 27.2.3.1 says
    // `Promise.prototype`'s is `Object.prototype`, and the chain-end fallback
    // does not stand in for it here. That fallback is a step the property path
    // takes for receivers whose members come from a TABLE beside the value; a
    // promise is an ordinary object, so its walk ended at null and
    // `Promise.resolve(1).hasOwnProperty` was `undefined`.
    Rooted<Value> parent{rtObjectPrototype()};
    ObjectHeader* protoObj =
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(parent.get()));
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};

    // 27.2.5.5: @@toStringTag is "Promise", non-enumerable.
    rtDefineToStringTag(proto, "Promise");

    // 27.2.5.4, 27.2.5.1, 27.2.5.3, with the spec's lengths.
    const NativeMethod methods[] = {
        {"then", promiseThen, 2},
        {"catch", promiseCatch, 1},
        {"finally", promiseFinally, 1},
    };
    rtDefineMethods(proto, methods, 3);

    Rooted<Value> ctor{rtNativeFunction(promiseConstructorBody, 1)};
    rtEnsureFunctionProperties(ctor);
    Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
    // 27.2.4's function properties, non-enumerable like every intrinsic's.
    const NativeMethod statics[] = {
        {"resolve", staticResolve, 1}, {"reject", staticReject, 1},
        {"all", staticAll, 1},         {"allSettled", staticAllSettled, 1},
        {"race", staticRace, 1},       {"any", staticAny, 1},
        {"withResolvers", staticWithResolvers, 0},
    };
    for (const NativeMethod& s : statics) {
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> val{rtNativeFunction(s.code, s.arity)};
        // DEFINED rather than assigned, and non-enumerable, for the reason
        // rtInstallNumberStatics gives about every intrinsic's statics:
        // `Object.keys(Promise)` is `[]`. The properties object is re-derived
        // through its root each turn, because both lines above allocate.
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false,
                                                      /*defineOwn=*/true);
    }

    // 27.2.5.2: the back-pointer, a DEFINITION so it does not write through.
    Rooted<Value> ctorKey{rtMakeString("constructor")};
    proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), ctorKey, ctor,
                                                  /*ic=*/nullptr, /*enumerable=*/false,
                                                  /*defineOwn=*/true);

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    fn->instance_shape = rtNewRootShape(proto.get());
    g_instanceShape = fn->instance_shape;
    g_promiseCtor = ctor.get();
    g_promiseProto = proto.get();
}

}  // namespace

Value rtPromisePrototype() {
    ensurePromiseIntrinsics();
    return g_promiseProto;
}

Shape* rtPromiseInstanceShape() {
    ensurePromiseIntrinsics();
    return g_instanceShape;
}

Value rtPromiseConstructor(const std::string& name) {
    if (name != "Promise") return Value::fromUndefined();
    ensurePromiseIntrinsics();
    return g_promiseCtor;
}

bool rtIsPromiseConstructor(Value fn) {
    return g_promiseCtor.isObject() && fn.rawBits() == g_promiseCtor.rawBits();
}

void rtCheckPromiseStaticMember(const std::string& key) {
    // The 27.2.4 members bronze has not built. `withResolvers` left this list
    // when it landed above; `try` is ES2025 and is real, so it is refused by
    // name rather than read as `undefined`.
    static const char* const kMissing[] = {"try"};
    rtCheckUnimplementedMember("Promise", kMissing, sizeof(kMissing) / sizeof(kMissing[0]),
                               key);
}

}  // namespace bronze::runtime
