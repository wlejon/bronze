// %AsyncGeneratorPrototype% (ECMA-262 27.6.1) and the async generator object's
// three methods (next, return, throw), each returning a Promise.
//
// Maintains the [[AsyncGeneratorQueue]] and resumes the generator's body
// (runtime/coro.h) when requests are processed.

#include "runtime/async_generator.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/coro.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/generator.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

Value readSlot(Rooted<Value>& obj, uint32_t slot) {
    return obj.get().asObject<ObjectHeader>()->internalSlot(slot);
}

void writeSlot(Rooted<Value>& obj, uint32_t slot, Value val) {
    obj.get().asObject<ObjectHeader>()->setInternalSlot(slot, val);
}

uint32_t stateOf(Rooted<Value>& gen) {
    return static_cast<uint32_t>(readSlot(gen, AsyncGeneratorSlot::State).asNumber());
}

void setState(Rooted<Value>& gen, uint32_t state) {
    writeSlot(gen, AsyncGeneratorSlot::State, Value::fromDouble(static_cast<double>(state)));
}

uint32_t queueLength(Rooted<Value>& queue) {
    if (!queue.get().isObject()) return 0;
    return queue.get().asObject<ArrayHeader>()->length;
}

Value queuePeek(Rooted<Value>& queue, uint32_t index) {
    return queue.get().asObject<ArrayHeader>()->getElem(index);
}

void queuePopFront(Rooted<Value>& queue) {
    auto* arr = queue.get().asObject<ArrayHeader>();
    if (arr->length == 0) return;
    for (uint32_t i = 1; i < arr->length; ++i) {
        Rooted<Value> el{arr->getElem(i)};
        arr->setElem(rtHeap(), i - 1, el);
    }
    Rooted<Value> undef{Value::fromUndefined()};
    arr->setElem(rtHeap(), arr->length - 1, undef);
    arr->length--;
}

Value iterResult(Rooted<Value>& value, bool done) { return rtCreateIterResult(value, done); }

void asyncGeneratorResumeNext(Rooted<Value>& gen);

void processResumeResult(Rooted<Value>& gen, bool done, Rooted<Value>& valueVal);

// A completed async generator never enters its body again, so it lets go of
// the frame and everything the frame held.
void complete(Rooted<Value>& gen) {
    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Completed));
    writeSlot(gen, AsyncGeneratorSlot::Frame, Value::fromUndefined());
}

// Runs the body one resumption and hands its completion on: to the request at
// the head of the queue for a `yield` or the body's end, to the awaited
// value's subscription for an `await`. A throw out of the body completes the
// generator and rejects every queued request with it; with no request queued
// there is nothing to reject, and it propagates.
void resumeBody(Rooted<Value>& gen, uint32_t mode, Rooted<Value>& sent) {
    Rooted<Value> value{sent.get()};
    for (;;) {
        Rooted<Value> frame{readSlot(gen, AsyncGeneratorSlot::Frame)};
        Rooted<Value> out{Value::fromUndefined()};
        CoroStep step;
        Value caught;
        const bool threw =
            rtTryCatch([&] { step = rtCoroResume(frame, mode, value, out); }, caught);
        if (!threw) {
            if (!step.done && step.kind == BRONZE_ABI_SUSPEND_AWAIT) {
                // Still executing as far as the queue is concerned: a request
                // made meanwhile waits its turn.
                Rooted<Value> thrown{Value::fromUndefined()};
                if (rtCoroAwait(gen, out, thrown)) return;
                mode = GeneratorResumeMode::Throw;
                value.set(thrown.get());
                continue;
            }
            processResumeResult(gen, step.done, out);
            return;
        }
        Rooted<Value> thrown{caught};
        Rooted<Value> queue{readSlot(gen, AsyncGeneratorSlot::Queue)};
        complete(gen);
        if (queueLength(queue) == 0) rtThrow(thrown.get());
        Rooted<Value> req{queuePeek(queue, 0)};
        Rooted<Value> promise{req.get().asObject<ArrayHeader>()->getElem(2)};
        queuePopFront(queue);
        rtRejectPromise(promise, thrown);
        while (queueLength(queue) > 0) {
            Rooted<Value> nextReq{queuePeek(queue, 0)};
            queuePopFront(queue);
            Rooted<Value> p{nextReq.get().asObject<ArrayHeader>()->getElem(2)};
            rtRejectPromise(p, thrown);
        }
        return;
    }
}

void processResumeResult(Rooted<Value>& gen, bool done, Rooted<Value>& valueVal) {
    Rooted<Value> queue{readSlot(gen, AsyncGeneratorSlot::Queue)};
    if (done) {
        complete(gen);
    } else {
        setState(gen, static_cast<uint32_t>(AsyncGeneratorState::SuspendedYield));
    }
    if (queueLength(queue) == 0) return;
    Rooted<Value> req{queuePeek(queue, 0)};
    Rooted<Value> promise{req.get().asObject<ArrayHeader>()->getElem(2)};
    queuePopFront(queue);

    if (done) {
        Rooted<Value> res{iterResult(valueVal, true)};
        rtResolvePromise(promise, res);
        while (queueLength(queue) > 0) {
            asyncGeneratorResumeNext(gen);
        }
    } else {
        Rooted<Value> res{iterResult(valueVal, false)};
        rtResolvePromise(promise, res);
        if (queueLength(queue) > 0) {
            asyncGeneratorResumeNext(gen);
        }
    }
}

void asyncGeneratorResumeNext(Rooted<Value>& gen) {
    Rooted<Value> queue{readSlot(gen, AsyncGeneratorSlot::Queue)};
    if (queueLength(queue) == 0) return;
    const uint32_t state = stateOf(gen);
    if (state == static_cast<uint32_t>(AsyncGeneratorState::Executing)) return;

    Rooted<Value> req{queuePeek(queue, 0)};
    Rooted<Value> modeVal{req.get().asObject<ArrayHeader>()->getElem(0)};
    Rooted<Value> sent{req.get().asObject<ArrayHeader>()->getElem(1)};
    Rooted<Value> promise{req.get().asObject<ArrayHeader>()->getElem(2)};

    const uint32_t mode = static_cast<uint32_t>(modeVal.get().asNumber());

    if (state == static_cast<uint32_t>(AsyncGeneratorState::Completed)) {
        queuePopFront(queue);
        if (mode == GeneratorResumeMode::Throw) {
            rtRejectPromise(promise, sent);
        } else {
            Rooted<Value> val{mode == GeneratorResumeMode::Return ? sent.get()
                                                                  : Value::fromUndefined()};
            Rooted<Value> res{iterResult(val, true)};
            rtResolvePromise(promise, res);
        }
        asyncGeneratorResumeNext(gen);
        return;
    }

    if (state == static_cast<uint32_t>(AsyncGeneratorState::SuspendedStart) &&
        mode != GeneratorResumeMode::Next) {
        queuePopFront(queue);
        complete(gen);
        if (mode == GeneratorResumeMode::Throw) {
            rtRejectPromise(promise, sent);
        } else {
            Rooted<Value> res{iterResult(sent, true)};
            rtResolvePromise(promise, res);
        }
        asyncGeneratorResumeNext(gen);
        return;
    }

    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Executing));
    writeSlot(gen, AsyncGeneratorSlot::CurrentPromise, promise.get());
    resumeBody(gen, mode, sent);
}

uint64_t enqueueRequest(uint64_t thisBits, uint32_t mode, Rooted<Value>& sent,
                        const char* method) {
    Rooted<Value> self{Value(thisBits)};
    if (!rtIsIteratorObject(self.get(), IteratorProto::AsyncGenerator)) {
        Rooted<Value> promise{rtNewPromise()};
        Rooted<Value> err{Value::fromString(StringHeader::createFromUTF8(
            rtHeap(), "AsyncGenerator.prototype." + std::string(method) +
                          " called on an incompatible receiver"))};
        rtRejectPromise(promise, err);
        return promise.get().rawBits();
    }

    Rooted<Value> promise{rtNewPromise()};
    const uint32_t state = stateOf(self);

    if (state == static_cast<uint32_t>(AsyncGeneratorState::Completed)) {
        if (mode == GeneratorResumeMode::Throw) {
            rtRejectPromise(promise, sent);
        } else {
            Rooted<Value> val{mode == GeneratorResumeMode::Return ? sent.get()
                                                                  : Value::fromUndefined()};
            Rooted<Value> res{iterResult(val, true)};
            rtResolvePromise(promise, res);
        }
        return promise.get().rawBits();
    }

    if (state == static_cast<uint32_t>(AsyncGeneratorState::SuspendedStart) &&
        mode != GeneratorResumeMode::Next) {
        complete(self);
        if (mode == GeneratorResumeMode::Throw) {
            rtRejectPromise(promise, sent);
        } else {
            Rooted<Value> res{iterResult(sent, true)};
            rtResolvePromise(promise, res);
        }
        return promise.get().rawBits();
    }

    Rooted<Value> queue{readSlot(self, AsyncGeneratorSlot::Queue)};
    if (!queue.get().isObject()) {
        queue = Value(bronze_create_array(0));
        writeSlot(self, AsyncGeneratorSlot::Queue, queue.get());
    }

    Rooted<Value> req{Value(bronze_create_array(3))};
    Rooted<Value> modeRoot{Value::fromDouble(static_cast<double>(mode))};
    req.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, modeRoot);
    req.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, sent);
    req.get().asObject<ArrayHeader>()->setElem(rtHeap(), 2, promise);

    const uint32_t at = queue.get().asObject<ArrayHeader>()->length;
    queue.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, req);

    if (state != static_cast<uint32_t>(AsyncGeneratorState::Executing)) {
        asyncGeneratorResumeNext(self);
    }

    return promise.get().rawBits();
}

uint64_t asyncGeneratorNext(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return enqueueRequest(thisBits, GeneratorResumeMode::Next, sent, "next");
}

uint64_t asyncGeneratorReturn(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return enqueueRequest(thisBits, GeneratorResumeMode::Return, sent, "return");
}

uint64_t asyncGeneratorThrow(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> sent{args[0]};
    return enqueueRequest(thisBits, GeneratorResumeMode::Throw, sent, "throw");
}

}  // namespace

void rtInstallAsyncGeneratorPrototype(Rooted<Value>& proto) {
    // 27.6.1.2 through 27.6.1.4, each of length 1.
    const NativeMethod methods[] = {
        {"next", asyncGeneratorNext, 1, 1},
        {"return", asyncGeneratorReturn, 1, 1},
        {"throw", asyncGeneratorThrow, 1, 1},
    };
    for (const auto& method : methods) {
        Rooted<Value> fn{rtNativeFunction(method.code, method.arity, method.name, method.length)};
        Rooted<Value> key{rtMakeString(method.name)};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn);
    }
}

void rtAsyncGeneratorResumeFromAwait(Rooted<Value>& gen, uint32_t mode, Rooted<Value>& sent) {
    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Executing));
    resumeBody(gen, mode, sent);
}

// The async generator object over `frame`. As for a generator (15.5.3), the
// arguments are bound at the call: the body runs here to its START suspension.
Value rtCreateAsyncGeneratorObject(Rooted<Value>& frame) {
    Rooted<Value> gen{rtNewIteratorObject(IteratorProto::AsyncGenerator)};
    Rooted<Value> queue{Value(bronze_create_array(0))};
    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Executing));
    writeSlot(gen, AsyncGeneratorSlot::Frame, frame.get());
    writeSlot(gen, AsyncGeneratorSlot::Queue, queue.get());
    writeSlot(gen, AsyncGeneratorSlot::CurrentPromise, Value::fromUndefined());
    rtCoroAdopt(frame);
    Rooted<Value> undef{Value::fromUndefined()};
    Rooted<Value> out{Value::fromUndefined()};
    const CoroStep step = rtCoroResume(frame, GeneratorResumeMode::Next, undef, out);
    if (step.done || step.kind != BRONZE_ABI_SUSPEND_START) {
        fatal("internal: an async generator body did not stop at its start");
    }
    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::SuspendedStart));
    return gen.get();
}

}  // namespace bronze::runtime
