// The Promise core: states, reactions, the resolving functions, thenable
// adoption, and the two job bodies. Everything program-visible about the
// intrinsic OBJECTS — constructor, prototype methods, statics — is next door
// in builtin_promise.cpp; everything here is 27.2.1's abstract operations.

#include "runtime/promise.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

Value readSlot(Value promise, uint32_t slot) {
    return promise.asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Value promise, uint32_t slot, Value val) {
    promise.asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// FulfillPromise / RejectPromise (27.2.1.4, 27.2.1.7), fused because they are
// one algorithm with the list and the state flipped. This is the promise-level
// entry the LATCHED paths call after consuming their latch; it re-checks
// `pending` anyway, because a defensive no-op here is cheaper than proving
// every caller sequence can never double-settle.
void settleInternal(Rooted<Value>& promise, Rooted<Value>& value, bool reject) {
    if (rtPromiseStateOf(promise.get()) != PromiseState::Pending) return;
    writeSlot(promise.get(), PromiseSlot::Result, value.get());
    writeSlot(promise.get(), PromiseSlot::State,
              Value::fromDouble(reject ? PromiseState::Rejected : PromiseState::Fulfilled));

    // 27.2.1.7 step 7 is HostPromiseRejectionTracker(promise, "reject"):
    // a rejection nobody has subscribed to yet is PARKED, and stays parked
    // until a `then` arrives or the drain ends with it still alone.
    if (reject && !bronze_truthy(readSlot(promise.get(), PromiseSlot::IsHandled).rawBits())) {
        rtParkRejection(promise.get());
    }

    // TriggerPromiseReactions (27.2.1.8): one job per reaction, in the order
    // they were subscribed. The list is (handler, capability) pairs
    // flattened; enqueueing allocates only C++ memory, so the raw walk over
    // the elements cannot be moved out from under itself.
    Value list = readSlot(promise.get(),
                          reject ? PromiseSlot::RejectReactions : PromiseSlot::FulfillReactions);
    if (list.isObject() && list.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        ArrayHeader* arr = list.asObject<ArrayHeader>();
        const Value* elems = arr->elementsData();
        for (uint32_t i = 0; i + 1 < arr->length; i += 2) {
            rtEnqueueReactionJob(elems[i], elems[i + 1], value.get(), reject);
        }
    }
    // BOTH lists are dropped — 27.2.1.4/27.2.1.7 set them to undefined — so a
    // settled promise holds no reaction alive and a late `then` reads the
    // state instead.
    writeSlot(promise.get(), PromiseSlot::FulfillReactions, Value::fromUndefined());
    writeSlot(promise.get(), PromiseSlot::RejectReactions, Value::fromUndefined());
}

// The body shared by every resolving-function pair once its latch has been
// consumed: 27.2.1.3.2 steps 7-16. Deciding WHOSE latch guards entry is the
// caller's business — the promise's own for rtResolvePromise, a pair's for a
// thenable job's functions.
void runResolutionSteps(Rooted<Value>& promise, Rooted<Value>& value) {
    // Step 7: resolving a promise with itself is the one cycle detectable
    // without running anything, and it is a rejection, not an error.
    if (value.get().rawBits() == promise.get().rawBits()) {
        Rooted<Value> err{
            rtNewErrorValue(ErrorKind::TypeError, "Chaining cycle detected for promise")};
        settleInternal(promise, err, /*reject=*/true);
        return;
    }
    // Step 8: a non-object fulfills directly — nothing to adopt.
    if (!value.get().isObject()) {
        settleInternal(promise, value, /*reject=*/false);
        return;
    }
    // Steps 9-10: Get(value, "then") runs user code (a getter), so its throw
    // becomes the rejection. Through the generic path, because the value may
    // be any receiver kind — an array's `then` is a real question.
    Rooted<Value> thenKey{rtMakeString("then")};
    Rooted<Value> thenFn{Value(bronze_elem_get(value.get().rawBits(), thenKey.get().rawBits()))};
    if (rtExceptionPending()) {
        Rooted<Value> thrown{Value(bronze_exception_cell)};
        rtClearException();
        settleInternal(promise, thrown, /*reject=*/true);
        return;
    }
    // Steps 11-12: an object whose `then` is not callable is a plain value.
    if (!isCallable(thenFn.get())) {
        settleInternal(promise, value, /*reject=*/false);
        return;
    }
    // Steps 13-15: the adoption is DEFERRED to a job, so no user `then`
    // runs inside whatever called resolve.
    rtEnqueueThenableJob(promise.get(), value.get(), thenFn.get());
}

// Consume the promise-level [[AlreadyResolved]] latch. True exactly once.
bool takePromiseLatch(Value promise) {
    if (bronze_truthy(readSlot(promise, PromiseSlot::AlreadyResolved).rawBits())) return false;
    writeSlot(promise, PromiseSlot::AlreadyResolved, Value::fromBool(true));
    return true;
}

// ---- the executor's resolving functions as objects --------------------------
// Their env record IS the promise: the pair's latch is the promise-level one,
// so no separate env object exists to hold it.

uint64_t resolvingResolve(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> promise{Value(env)};
    Rooted<Value> value{args[0]};
    rtResolvePromise(promise, value);
    return Value::fromUndefined().rawBits();
}

uint64_t resolvingReject(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> promise{Value(env)};
    Rooted<Value> reason{args[0]};
    rtRejectPromise(promise, reason);
    return Value::fromUndefined().rawBits();
}

// ---- a thenable job's resolving pair ----------------------------------------
// 27.2.1.3 gives every pair a FRESH [[AlreadyResolved]]. This pair's lives in
// an env object of its own — the promise's latch was consumed by the resolve
// that adopted the thenable, and blocking on it would block the adoption's
// completion (the bug the design caught on paper: latch-at-adoption-time).

namespace PairSlot {
enum : uint32_t { Promise, Latch, kCount };
}

bool takePairLatch(Value pairEnv) {
    ObjectHeader* env = pairEnv.asObject<ObjectHeader>();
    if (bronze_truthy(env->internalSlot(PairSlot::Latch).rawBits())) return false;
    env->setInternalSlot(PairSlot::Latch, Value::fromBool(true));
    return true;
}

uint64_t pairResolve(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> pair{Value(env)};
    Rooted<Value> value{args[0]};
    if (!takePairLatch(pair.get())) return Value::fromUndefined().rawBits();
    Rooted<Value> promise{pair.get().asObject<ObjectHeader>()->internalSlot(PairSlot::Promise)};
    runResolutionSteps(promise, value);
    return Value::fromUndefined().rawBits();
}

uint64_t pairReject(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> pair{Value(env)};
    Rooted<Value> reason{args[0]};
    if (!takePairLatch(pair.get())) return Value::fromUndefined().rawBits();
    Rooted<Value> promise{pair.get().asObject<ObjectHeader>()->internalSlot(PairSlot::Promise)};
    settleInternal(promise, reason, /*reject=*/true);
    return Value::fromUndefined().rawBits();
}

// The reaction list named by `slot`, created on first subscription.
void appendReaction(Rooted<Value>& promise, uint32_t slot, Rooted<Value>& handler,
                    Rooted<Value>& capability) {
    Rooted<Value> list{readSlot(promise.get(), slot)};
    if (!list.get().isObject()) {
        ArrayHeader* fresh = ArrayHeader::create(rtHeap(), 4);
        list.set(Value::fromObject(fresh));
        writeSlot(promise.get(), slot, list.get());
    }
    bronze_array_append(list.get().rawBits(), handler.get().rawBits());
    bronze_array_append(list.get().rawBits(), capability.get().rawBits());
}

}  // namespace

bool rtIsPromise(Value v) {
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return false;
    // The prototype accessor can BUILD the intrinsics (an allocation), so it
    // is taken before any raw pointer into `v` — reached only for a plain
    // object with exactly a promise's slot count, so the build triggers at
    // most once, and for something that is almost certainly a promise.
    if (v.asObject<ObjectHeader>()->internalSlotCount() != PromiseSlot::kCount) return false;
    Rooted<Value> self{v};
    const uint64_t protoBits = rtPromisePrototype().rawBits();
    ObjectHeader* proto = self.get().asObject<ObjectHeader>()->protoAncestor(1);
    return proto != nullptr && Value::fromObject(proto).rawBits() == protoBits;
}

Value rtNewPromise() {
    // The shape accessor first: it may build the intrinsics, which allocates.
    Shape* shape = rtPromiseInstanceShape();
    ObjectHeader* p =
        ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(), shape, PromiseSlot::kCount);
    p->header.flags = HeapKind::Plain;
    p->setInternalSlot(PromiseSlot::State, Value::fromDouble(PromiseState::Pending));
    p->setInternalSlot(PromiseSlot::IsHandled, Value::fromBool(false));
    p->setInternalSlot(PromiseSlot::AlreadyResolved, Value::fromBool(false));
    return Value::fromObject(p);
}

void rtResolvePromise(Rooted<Value>& promise, Rooted<Value>& value) {
    if (!takePromiseLatch(promise.get())) return;
    runResolutionSteps(promise, value);
}

void rtRejectPromise(Rooted<Value>& promise, Rooted<Value>& reason) {
    if (!takePromiseLatch(promise.get())) return;
    settleInternal(promise, reason, /*reject=*/true);
}

Value rtPromiseResolveValue(Rooted<Value>& v) {
    // 27.2.4.7 step 2, and the whole of the ES2019 single-tick rule: an
    // intrinsic promise passes through UNTOUCHED, so an await of one costs
    // exactly one reaction job. (With subclassing refused, "its constructor
    // is %Promise%" is the brand check.)
    if (rtIsPromise(v.get())) return v.get();
    Rooted<Value> p{rtNewPromise()};
    rtResolvePromise(p, v);
    return p.get();
}

void rtPerformPromiseThen(Rooted<Value>& promise, Rooted<Value>& onFulfilled,
                          Rooted<Value>& onRejected, Rooted<Value>& capability) {
    // 27.2.5.4.1 steps 3-4: a non-callable handler IS the absent handler.
    Rooted<Value> onF{isCallable(onFulfilled.get()) ? onFulfilled.get()
                                                    : Value::fromUndefined()};
    Rooted<Value> onR{isCallable(onRejected.get()) ? onRejected.get() : Value::fromUndefined()};

    const uint32_t state = rtPromiseStateOf(promise.get());
    switch (state) {
        case PromiseState::Pending:
            appendReaction(promise, PromiseSlot::FulfillReactions, onF, capability);
            appendReaction(promise, PromiseSlot::RejectReactions, onR, capability);
            break;
        case PromiseState::Fulfilled: {
            // Already settled still goes THROUGH THE QUEUE (27.2.5.4.1 step
            // 8): `then` on a resolved promise never runs its handler
            // synchronously.
            Value result = readSlot(promise.get(), PromiseSlot::Result);
            rtEnqueueReactionJob(onF.get(), capability.get(), result, /*rejected=*/false);
            break;
        }
        default: {
            Value result = readSlot(promise.get(), PromiseSlot::Result);
            rtEnqueueReactionJob(onR.get(), capability.get(), result, /*rejected=*/true);
            break;
        }
    }
    // 27.2.5.4.1 step 10: subscribing HANDLES the promise — before the
    // reaction runs, which is what lets `p.catch(...)` added in the same
    // tick as the rejection cancel the report.
    writeSlot(promise.get(), PromiseSlot::IsHandled, Value::fromBool(true));
    rtUnparkRejection(promise.get());
}

Value rtMakeNativeClosure(NativeFunctionCode code, Rooted<Value>& env, uint32_t arity) {
    FunctionHeader* fn =
        FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity);
    fn->env_record = env.get();
    fn->header.flags = HeapKind::Function;
    return Value::fromObject(fn);
}

Value rtMakeResolvingFunction(Rooted<Value>& promise, bool isReject) {
    return rtMakeNativeClosure(isReject ? resolvingReject : resolvingResolve, promise,
                               /*arity=*/1);
}

void rtRunReactionJob(Rooted<Value>& handler, Rooted<Value>& capability,
                      Rooted<Value>& argument, bool rejected) {
    // 27.2.2.1's job body. `result`/`resultRejected` are the completion the
    // capability is settled with.
    Rooted<Value> result{argument.get()};
    bool resultRejected = rejected;
    if (isCallable(handler.get())) {
        uint64_t argBits[1] = {argument.get().rawBits()};
        result.set(Value(bronze_dynamic_call(handler.get().rawBits(),
                                             Value::fromUndefined().rawBits(), 1, argBits)));
        if (rtExceptionPending()) {
            // The handler's throw is the capability's rejection (step 1.e-g),
            // never an escape from the drain.
            result.set(Value(bronze_exception_cell));
            rtClearException();
            resultRejected = true;
        } else {
            resultRejected = false;
        }
    }
    // No capability means the handler was the whole reaction — the async
    // driver's subscriptions, which settle the machine's own promise from
    // inside the handler. (An absent handler with no capability would be a
    // reaction that does nothing; no caller builds one.)
    if (!capability.get().isObject()) {
        // A reject reaction with NO handler and no capability lost its
        // reason silently once already on paper; the invariant is stated
        // here so a future caller trips it loudly.
        return;
    }
    if (resultRejected) {
        rtRejectPromise(capability, result);
    } else {
        rtResolvePromise(capability, result);
    }
}

void rtRunThenableJob(Rooted<Value>& promise, Rooted<Value>& thenable, Rooted<Value>& thenFn) {
    // 27.2.2.2: a FRESH pair with a fresh latch, then
    // Call(then, thenable, resolve, reject); its throw goes through the
    // pair's reject so a `then` that both called resolve and threw keeps the
    // first answer.
    Rooted<Value> pair{Value::fromUndefined()};
    {
        ObjectHeader* env = ObjectHeader::createWithInternalSlots(
            rtHeap(), rtArena(), rtPlainObjectShape(), PairSlot::kCount);
        env->header.flags = HeapKind::Plain;
        env->setInternalSlot(PairSlot::Latch, Value::fromBool(false));
        pair.set(Value::fromObject(env));
    }
    // Written through the root, after the allocation above.
    pair.get().asObject<ObjectHeader>()->setInternalSlot(PairSlot::Promise, promise.get());

    Rooted<Value> resolveFn{rtMakeNativeClosure(pairResolve, pair, 1)};
    Rooted<Value> rejectFn{rtMakeNativeClosure(pairReject, pair, 1)};

    uint64_t argBits[2] = {resolveFn.get().rawBits(), rejectFn.get().rawBits()};
    bronze_dynamic_call(thenFn.get().rawBits(), thenable.get().rawBits(), 2, argBits);
    if (rtExceptionPending()) {
        Rooted<Value> thrown{Value(bronze_exception_cell)};
        rtClearException();
        if (takePairLatch(pair.get())) {
            settleInternal(promise, thrown, /*reject=*/true);
        }
    }
}

std::string rtPromiseRejectionText(Value promise) {
    const Value reason = readSlot(promise, PromiseSlot::Result);
    if (std::string text; rtIsErrorInstance(reason) && rtErrorText(reason, text)) {
        return text;
    }
    // A non-Error reason is reported the way console.log would show it, for
    // the reason rtUncaughtText gives: `reject(7)` and `reject('7')` are
    // different programs.
    return rtInspect(reason);
}

uint32_t rtPromiseStateOf(Value promise) {
    return static_cast<uint32_t>(readSlot(promise, PromiseSlot::State).asNumber());
}

Value rtPromiseResultOf(Value promise) { return readSlot(promise, PromiseSlot::Result); }

}  // namespace bronze::runtime
