// The ASYNC DRIVER: the half of an async function that is not compiled.
//
// `async function f() { ... }` compiles to a coroutine body and a stub that
// starts it (src/lower/lower_generator.cpp). What resumes the body is this
// file, over a MACHINE: an internal object holding the body's frame, the
// promise the call returned, and where the body is.
//
//   start   (rtStartAsyncFunction)   the body, synchronously, up to its first
//                                    await or to completion (27.7.5.1 step 9);
//                                    answers the promise
//   resume  (rtAsyncFunctionResume)  one settled await: run the body on to its
//                                    next await, or to its completion
//
// The machine is not a JS value. Nothing hands one to a program and nothing
// can forge one: it is 27.7.5.2's async function object reduced to the three
// things a suspended async body needs.
//
// ---- THE ROOT PATH ---------------------------------------------------------
//
// A suspended async body is held by NOTHING ON ANY STACK: the caller has only
// the promise. So the path is written down:
//
//   machine  -> the FRAME, every value the body will read when it resumes
//   machine  -> the Promise the async call returned
//
// and the machine itself is reachable, while suspended, through the
// environment of the two closures its await subscribed (runtime/coro.cpp),
// which live in the awaited promise's reaction list or in a queued job — both
// traced.

#include "abi/bronze_abi.h"
#include "runtime/coro.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

#include <brass/runtime/coroutine.hpp>

namespace bronze::runtime {

namespace {

namespace MachineSlot {
enum : uint32_t { Frame, Promise, State, kCount };
}

// 27.7.5.2's states, minus the two a generator has that an async function
// cannot reach. `Executing` earns its place for the reason 27.5.1.1's does —
// it is the one state that is only ever true while the body is on the stack.
namespace MachineState {
enum : uint32_t { Executing, Suspended, Completed };
}

Value readSlot(Rooted<Value>& machine, uint32_t slot) {
    return machine.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& machine, uint32_t slot, Value value) {
    machine.get().asObject<ObjectHeader>()->setInternalSlot(slot, value);
}

void setState(Rooted<Value>& machine, uint32_t state) {
    writeSlot(machine, MachineSlot::State, Value::fromDouble(static_cast<double>(state)));
}

uint32_t stateOf(Rooted<Value>& machine) {
    return static_cast<uint32_t>(readSlot(machine, MachineSlot::State).asNumber());
}

// The body finished: the promise settles through its own latch, so
// `return p` inside an async function ADOPTS p (27.2.1.3.2), and the machine
// lets go of the frame.
void complete(Rooted<Value>& machine, Rooted<Value>& value, bool rejected) {
    setState(machine, MachineState::Completed);
    writeSlot(machine, MachineSlot::Frame, Value::fromUndefined());
    Rooted<Value> promise{readSlot(machine, MachineSlot::Promise)};
    promise.get().asObject<ObjectHeader>()->setInternalSlot(PromiseSlot::AsyncOwner, Value::fromUndefined());
    if (rejected) {
        rtRejectPromise(promise, value);
    } else {
        rtResolvePromise(promise, value);
    }
}

// The body in `frame` awaits `awaited`: when that is the promise of another
// async call still running, this frame is the one waiting on that call's
// frame, and brass's async stack (current_async_stack, its crash report and
// symbolizer, and Error.stack's "at async" frames) walks from it to here.
// Allocates nothing.
void linkAwaiter(Rooted<Value>& awaited, Rooted<Value>& frame) {
    if (!rtIsPromiseObject(awaited.get()) || !frame.get().isObject()) return;
    const Value owner = awaited.get().asObject<ObjectHeader>()->internalSlot(PromiseSlot::AsyncOwner);
    if (!owner.isObject()) return;
    const Value callee = owner.asObject<ObjectHeader>()->internalSlot(MachineSlot::Frame);
    if (!callee.isObject() || callee.rawBits() == frame.get().rawBits()) return;
    brass_coro_set_awaiter(reinterpret_cast<uintptr_t>(callee.asObject<void>()),
                           reinterpret_cast<uintptr_t>(frame.get().asObject<void>()));
}

}  // namespace

// One resumption: enter the body, then act on how it came back. Three
// outcomes, and they are the whole of 27.7.5.2:
//   - the body threw       -> the promise REJECTS with the thrown value
//   - it returned          -> the promise RESOLVES with the value
//   - it stopped at await  -> subscribe the resumption to the awaited value
void rtAsyncFunctionResume(Rooted<Value>& machine, uint32_t mode, Rooted<Value>& sent) {
    Rooted<Value> value{sent.get()};
    for (;;) {
        const uint32_t state = stateOf(machine);
        if (state == MachineState::Completed) {
            // An await subscribes ONE fulfill/reject pair, a promise runs one
            // of the two, and a settled promise never runs either again.
            fatal("internal: an async function resumed after it completed");
        }
        setState(machine, MachineState::Executing);
        Rooted<Value> frame{readSlot(machine, MachineSlot::Frame)};
        Rooted<Value> out{Value::fromUndefined()};
        CoroStep step;
        Value caught;
        if (rtTryCatch([&] { step = rtCoroResume(frame, mode, value, out); }, caught)) {
            // 27.7.5.2 step 3.f: an abrupt completion of the body rejects the
            // promise — also at the start, where `async function f() { throw
            // x }` returns a rejected promise rather than throwing at the call.
            Rooted<Value> thrown{caught};
            complete(machine, thrown, /*rejected=*/true);
            return;
        }
        if (step.done) {
            complete(machine, out, /*rejected=*/false);
            return;
        }
        if (step.kind != BRONZE_ABI_SUSPEND_AWAIT) {
            fatal("internal: an async function body suspended other than at an await");
        }
        setState(machine, MachineState::Suspended);
        linkAwaiter(out, frame);
        Rooted<Value> thrown{Value::fromUndefined()};
        if (rtCoroAwait(machine, out, thrown)) return;
        // PromiseResolve threw: the await throws it, at the await.
        mode = BRONZE_ABI_RESUME_THROW;
        value.set(thrown.get());
    }
}

// The body, synchronously, up to its first suspension (27.7.5.1 step 9):
// code before the first `await` observes the caller's world unchanged, which
// is what makes `f(); console.log("after")` print in the order it is written.
Value rtStartAsyncFunction(Rooted<Value>& frame) {
    Rooted<Value> promise{rtNewPromise()};
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(
        rtHeap(), rtArena(), rtPlainObjectShape(), MachineSlot::kCount);
    obj->header.flags = HeapKind::Plain;
    Rooted<Value> machine{Value::fromObject(obj)};
    writeSlot(machine, MachineSlot::Frame, frame.get());
    writeSlot(machine, MachineSlot::Promise, promise.get());
    promise.get().asObject<ObjectHeader>()->setInternalSlot(PromiseSlot::AsyncOwner, machine.get());
    setState(machine, MachineState::Suspended);
    rtCoroAdopt(frame);
    Rooted<Value> undef{Value::fromUndefined()};
    rtAsyncFunctionResume(machine, BRONZE_ABI_RESUME_NEXT, undef);
    return readSlot(machine, MachineSlot::Promise);
}

}  // namespace bronze::runtime
