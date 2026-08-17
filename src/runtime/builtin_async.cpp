// The ASYNC DRIVER: the half of an async function that is not compiled.
//
// `async function f() { ... }` compiles to exactly what `function* f() { ... }`
// compiles to — one frame record, one resume closure taking `(__mode, __sent)`,
// one state index — and lower_async.cpp's header says so. What differs is who
// resumes it. A generator object hands that job to a program calling `next`;
// an async function hands it to the promise machinery, and this file IS that
// caller:
//
//   create  (bronze_async_machine)  a machine over the resume closure, plus the
//                                   promise the function will return
//   start   (bronze_async_start)    run the body to its first suspension (or to
//                                   completion, 27.7.5.1 step 9) and hand back
//                                   that promise
//   await   (bronze_async_await)    subscribe a resumption to the awaited value
//
// The machine is not a JS value. Nothing hands one to a program, nothing can
// forge one, and it carries no prototype worth naming: it is a generator
// object's [[GeneratorContext]] with the object taken away, which is what
// 27.7.5.2's "async function object" reduces to once the execution context is
// a closure over a frame.
//
// ---- THE ROOT PATH, which is the whole GC story ----------------------------
//
// A suspended async frame is the one live thing in bronze that NOTHING ON ANY
// STACK holds. The body has returned; the caller has only the promise. So the
// path has to be written down:
//
//   machine  -> Resume closure -> its env_record -> the FRAME RECORD, which is
//               every binding, parameter and lifted intermediate the body will
//               read when it resumes (ast::liftYields put them all there)
//   machine  -> Promise, the one the async call returned
//
// and the machine itself is reachable, while suspended, through exactly one
// edge: the `env_record` of the two resumption closures the await subscribed.
// Those live in either
//
//   - the awaited promise's reaction list, when it is still pending — and the
//     awaited promise is held by whatever will settle it; or
//   - a queued Job's `v0`, when it was already settled — and microtask.cpp
//     registers the queue as a GC ROOT SOURCE for precisely this.
//
// The drain's pop is the gap that has to be closed on the other side, and it
// is: rtDrainMicrotasks copies a job's three captures into frame roots BEFORE
// popping it, so a job that allocates (every resumption does) is never holding
// values the collector cannot see. Nothing here may hold a raw header across
// an allocation, and every slot read below is re-derived through a root.

#include "abi/bronze_abi.h"
#include "runtime/async_generator.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The machine's internal slots. Three, and none of them is program-visible:
// the closure that IS the body, the promise the call returned, and where the
// body currently is.
namespace MachineSlot {
enum : uint32_t { Resume, Promise, State, kCount };
}

// 27.7.5.2's states, minus the two a generator has that an async function
// cannot reach: nothing outside can resume an async body, so there is no
// `suspendedStart` a program could observe and no `return` resumption.
// `Executing` earns its place for the reason 27.5.1.1's does — it is the one
// state that is only ever true while the body is on the stack below us.
namespace MachineState {
enum : uint32_t { NotStarted, Executing, Suspended, Completed };
}

Value readSlot(Rooted<Value>& machine, uint32_t slot) {
    return machine.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void setState(Rooted<Value>& machine, uint32_t state) {
    machine.get().asObject<ObjectHeader>()->setInternalSlot(
        MachineSlot::State, Value::fromDouble(static_cast<double>(state)));
}

uint32_t stateOf(Rooted<Value>& machine) {
    return static_cast<uint32_t>(readSlot(machine, MachineSlot::State).asNumber());
}

// One field of the IteratorResult the resume closure returned. It is an
// ordinary object this compilation's own code built two instructions ago — the
// same read builtin_generator.cpp makes of the same object — so it is a plain
// property get with no protocol around it.
Value resultField(Rooted<Value>& result, const char* name) {
    if (!result.get().isObject()) return Value::fromUndefined();
    Rooted<Value> key{rtMakeString(name)};
    return result.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

void resumeMachine(Rooted<Value>& machine, uint32_t mode, Rooted<Value>& sent);

// The two resumption closures an await subscribes. Their env record IS the
// machine, which is the edge the root path above turns on.
//
// The mode is the generator protocol's, unchanged and deliberately so: a
// fulfilled await continues at its suspension point with `__sent` as the
// await's value (mode `next`), and a rejected one RAISES `__sent` there (mode
// `throw`). lower_async.cpp emits that test inside whatever protected region
// the `await` was written in, so `try`/`catch`/`finally` across an await is
// not implemented anywhere — it is lower_try.cpp's handler stamping and
// cleanup routing, reached because the throw happens at the right place.

uint64_t resumeNext(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> machine{Value(env)};
    Rooted<Value> sent{args[0]};
    resumeMachine(machine, GeneratorResumeMode::Next, sent);
    return Value::fromUndefined().rawBits();
}

uint64_t resumeThrow(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> machine{Value(env)};
    Rooted<Value> reason{args[0]};
    resumeMachine(machine, GeneratorResumeMode::Throw, reason);
    return Value::fromUndefined().rawBits();
}

// Settle the machine's promise from a body that finished. Both halves go
// through the promise's own latch (rtResolvePromise / rtRejectPromise), so the
// "first settle wins" rule that covers the executor and the combinators covers
// the driver too — and `return p` inside an async function ADOPTS p, because
// resolving with a thenable is what 27.2.1.3.2 already does.
void settleFromCompletion(Rooted<Value>& machine, Rooted<Value>& value, bool rejected) {
    Rooted<Value> promise{readSlot(machine, MachineSlot::Promise)};
    if (rejected) {
        rtRejectPromise(promise, value);
    } else {
        rtResolvePromise(promise, value);
    }
}

// One resumption: enter the body, then read what came back out of it.
//
// Three outcomes, and they are the whole of 27.7.5.2:
//   - the body threw            -> the promise REJECTS with the thrown value
//   - it returned `done: true`  -> the promise RESOLVES with `value`
//   - it returned `done: false` -> it suspended at an await, which has ALREADY
//                                  subscribed (lower_async.cpp emits the
//                                  subscription before the result), so there is
//                                  nothing to do but record where we are
void resumeMachine(Rooted<Value>& machine, uint32_t mode, Rooted<Value>& sent) {
    if (rtIsIteratorObject(machine.get(), IteratorProto::AsyncGenerator)) {
        rtAsyncGeneratorResumeFromAwait(machine, mode, sent);
        return;
    }
    const uint32_t state = stateOf(machine);
    if (state == MachineState::Completed) {
        // Unreachable: an await subscribes ONE fulfill/reject pair, a promise
        // runs one of the two, and a settled promise never runs either again.
        // Named rather than assumed — a second entry would re-run the body
        // from a state index the first entry left behind.
        fatal("internal: an async machine resumed after it completed");
    }
    if (state == MachineState::Executing) {
        fatal("internal: an async machine resumed while its body is running");
    }
    setState(machine, MachineState::Executing);

    Rooted<Value> body{readSlot(machine, MachineSlot::Resume)};
    uint64_t argBits[2] = {Value::fromDouble(static_cast<double>(mode)).rawBits(),
                           sent.get().rawBits()};
    Rooted<Value> result{Value(bronze_dynamic_call(
        body.get().rawBits(), Value::fromUndefined().rawBits(), 2, argBits))};

    if (rtExceptionPending()) {
        // 27.7.5.2 step 3.f: an abrupt completion of the body rejects the
        // promise. Taken out of the cell and CLEARED here, because the throw
        // has reached its destination — there is no JS frame above an async
        // body's resumption to propagate into. (At `start` that frame exists,
        // and the answer is the same: `async function f() { throw x }` returns
        // a rejected promise rather than throwing at the call site.)
        Rooted<Value> thrown{Value(bronze_tls_block_addr()->exception_cell)};
        rtClearException();
        setState(machine, MachineState::Completed);
        settleFromCompletion(machine, thrown, /*rejected=*/true);
        return;
    }

    Rooted<Value> done{resultField(result, "done")};
    if (!bronze_truthy(done.get().rawBits())) {
        setState(machine, MachineState::Suspended);
        return;
    }
    Rooted<Value> value{resultField(result, "value")};
    setState(machine, MachineState::Completed);
    settleFromCompletion(machine, value, /*rejected=*/false);
}

}  // namespace

extern "C" {

// The three helpers `src/abi/bronze_abi.h` declares for the async ops, in the
// registry's order and with the registry's arities — u64 in, u64 out, and
// `void` for the one that answers nothing. Nothing here returns a C++ class:
// that is the sret rule the ABI header exists to enforce, and an async helper
// returning a `Value` would shift every argument register under MSVC. They sit
// inside the namespace with C language linkage, as exception.cpp's do: the
// linkage decides the symbol name, so the enclosing namespace costs nothing
// and buys the file's internal helpers by their own names.

// `create.async_machine`: the machine, and with it the promise the async call
// will return. The promise is minted HERE and not at `start` because the body
// can finish synchronously — 27.7.5.1 step 9 runs it up to the first await —
// and something has to be there to resolve when it does.
uint64_t bronze_async_machine(uint64_t resumeBits) {
    Rooted<Value> body{Value(resumeBits)};
    Rooted<Value> promise{rtNewPromise()};
    // Allocated first, filled through the roots afterwards: both allocations
    // above and this one can collect, and a by-value copy taken before any of
    // them would name dead from-space.
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), rtPlainObjectShape(), MachineSlot::kCount);
    obj->header.flags = HeapKind::Plain;
    Rooted<Value> machine{Value::fromObject(obj)};
    ObjectHeader* fields = machine.get().asObject<ObjectHeader>();
    fields->setInternalSlot(MachineSlot::Resume, body.get());
    fields->setInternalSlot(MachineSlot::Promise, promise.get());
    fields->setInternalSlot(MachineSlot::State, Value::fromDouble(MachineState::NotStarted));
    return machine.get().rawBits();
}

// `async.start`: the body, synchronously, up to its first suspension. That
// synchrony is 27.7.5.1 step 9 and it is the one thing the tail of an async
// function commits to — code before the first `await` observes the caller's
// world unchanged, which is what makes `f(); console.log("after")` print in
// the order it is written.
uint64_t bronze_async_start(uint64_t machineBits) {
    Rooted<Value> machine{Value(machineBits)};
    if (stateOf(machine) != MachineState::NotStarted) {
        fatal("internal: an async machine started twice");
    }
    Rooted<Value> undef{Value::fromUndefined()};
    resumeMachine(machine, GeneratorResumeMode::Next, undef);
    // Read through the root AFTER the body ran: it allocated, it may have
    // collected, and the promise's address is not the one this frame saw.
    return readSlot(machine, MachineSlot::Promise).rawBits();
}

// `async.await`: 27.7.5.3 Await, which is PromiseResolve followed by
// PerformPromiseThen over two closures that resume this machine.
//
// THE SINGLE-TICK RULE lives in the first of those two calls. 27.2.4.7 step 2
// returns an argument that already IS an intrinsic promise UNCHANGED, so
// `await Promise.resolve(1)` subscribes the fulfilled promise directly: ONE
// reaction job, the same one `.then(f)` on it would cost. Without step 2 the
// await would wrap it in a fresh promise, resolving THAT with a promise would
// queue a thenable-adoption job, and the resumption would land a tick late —
// the extra ticks ES2019 removed, and the reason `async_await_single_tick`
// pins an await's interleaving against a `.then` chain rather than trusting
// this comment.
//
// No capability is passed: the reaction settles nothing of its own. What it
// does instead is re-enter the body, whose completion settles the machine's
// promise — which is why rtRunReactionJob's "no capability" arm is a plain
// return rather than an error.
void bronze_async_await(uint64_t machineBits, uint64_t awaitedBits) {
    Rooted<Value> machine{Value(machineBits)};
    Rooted<Value> awaited{Value(awaitedBits)};
    Rooted<Value> promise{rtPromiseResolveValue(awaited)};
    Rooted<Value> onFulfilled{rtMakeNativeClosure(resumeNext, machine, 1)};
    Rooted<Value> onRejected{rtMakeNativeClosure(resumeThrow, machine, 1)};
    Rooted<Value> noCapability{Value::fromUndefined()};
    rtPerformPromiseThen(promise, onFulfilled, onRejected, noCapability);
}

}  // extern "C"

}  // namespace bronze::runtime
