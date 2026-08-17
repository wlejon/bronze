// %AsyncGeneratorPrototype% (ECMA-262 27.6.1) and the async generator object's
// three methods (next, return, throw), each returning a Promise.
//
// Maintains the [[AsyncGeneratorQueue]] and executes the generator's resume
// function when requests are processed.

#include "runtime/async_generator.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
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

Value iterResult(Rooted<Value>& value, bool done) {
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> vk{rtMakeString("value")};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), vk, value);
    Rooted<Value> dk{rtMakeString("done")};
    Rooted<Value> dv{Value::fromBool(done)};
    out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), dk, dv);
    return out.get();
}

void asyncGeneratorResumeNext(Rooted<Value>& gen);

void processResumeResult(Rooted<Value>& gen, Rooted<Value>& result) {
    Rooted<Value> queue{readSlot(gen, AsyncGeneratorSlot::Queue)};
    if (queueLength(queue) == 0) return;
    Rooted<Value> req{queuePeek(queue, 0)};
    Rooted<Value> promise{req.get().asObject<ArrayHeader>()->getElem(2)};

    if (rtExceptionPending()) {
        Rooted<Value> thrown{Value(bronze_tls_block_addr()->exception_cell)};
        rtClearException();
        queuePopFront(queue);
        setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Completed));
        rtRejectPromise(promise, thrown);
        while (queueLength(queue) > 0) {
            Rooted<Value> nextReq{queuePeek(queue, 0)};
            queuePopFront(queue);
            Rooted<Value> p{nextReq.get().asObject<ArrayHeader>()->getElem(2)};
            rtRejectPromise(p, thrown);
        }
        return;
    }

    if (result.get().isObject() &&
        result.get().asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        Rooted<Value> isAwaitKey{rtMakeString("isAwait")};
        Rooted<Value> isAwaitVal{
            result.get().asObject<ObjectHeader>()->getProp(rtHeap(), isAwaitKey)};
        if (bronze_truthy(isAwaitVal.get().rawBits())) {
            return;
        }
    }

    queuePopFront(queue);

    Rooted<Value> doneKey{rtMakeString("done")};
    Rooted<Value> doneVal{
        result.get().isObject()
            ? result.get().asObject<ObjectHeader>()->getProp(rtHeap(), doneKey)
            : Value::fromBool(true)};
    const bool done = bronze_truthy(doneVal.get().rawBits());

    Rooted<Value> valKey{rtMakeString("value")};
    Rooted<Value> valueVal{
        result.get().isObject()
            ? result.get().asObject<ObjectHeader>()->getProp(rtHeap(), valKey)
            : Value::fromUndefined()};

    if (done) {
        setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Completed));
        Rooted<Value> res{iterResult(valueVal, true)};
        rtResolvePromise(promise, res);
        while (queueLength(queue) > 0) {
            asyncGeneratorResumeNext(gen);
        }
    } else {
        setState(gen, static_cast<uint32_t>(AsyncGeneratorState::SuspendedYield));
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
        setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Completed));
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

    Rooted<Value> body{readSlot(gen, AsyncGeneratorSlot::Resume)};
    Value args[2] = {Value::fromDouble(static_cast<double>(mode)), sent.get()};
    Rooted<Value> result{
        body.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, args)};

    processResumeResult(gen, result);
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
        setState(self, static_cast<uint32_t>(AsyncGeneratorState::Completed));
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
    struct Method {
        const char* key;
        NativeFunctionCode code;
        uint32_t arity;
    };
    const Method methods[] = {
        {"next", asyncGeneratorNext, 1},
        {"return", asyncGeneratorReturn, 1},
        {"throw", asyncGeneratorThrow, 1},
    };
    for (const auto& method : methods) {
        Rooted<Value> fn{rtNativeFunction(method.code, method.arity)};
        Rooted<Value> key{rtMakeString(method.key)};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn);
    }
}

void rtAsyncGeneratorResumeFromAwait(Rooted<Value>& gen, uint32_t mode, Rooted<Value>& sent) {
    setState(gen, static_cast<uint32_t>(AsyncGeneratorState::Executing));
    Rooted<Value> body{readSlot(gen, AsyncGeneratorSlot::Resume)};
    Value args[2] = {Value::fromDouble(static_cast<double>(mode)), sent.get()};
    Rooted<Value> result{
        body.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, args)};
    processResumeResult(gen, result);
}

}  // namespace bronze::runtime

extern "C" {

uint64_t bronze_create_async_generator_object(uint64_t resumeBits) {
    using namespace bronze;
    using namespace bronze::runtime;
    Rooted<Value> body{Value(resumeBits)};
    Rooted<Value> gen{rtNewIteratorObject(IteratorProto::AsyncGenerator)};
    Rooted<Value> queue{Value(bronze_create_array(0))};
    gen.get().asObject<ObjectHeader>()->setInternalSlot(
        AsyncGeneratorSlot::State,
        Value::fromDouble(static_cast<double>(AsyncGeneratorState::SuspendedStart)));
    gen.get().asObject<ObjectHeader>()->setInternalSlot(AsyncGeneratorSlot::Resume, body.get());
    gen.get().asObject<ObjectHeader>()->setInternalSlot(AsyncGeneratorSlot::Queue, queue.get());
    gen.get().asObject<ObjectHeader>()->setInternalSlot(AsyncGeneratorSlot::CurrentPromise,
                                                        Value::fromUndefined());
    return gen.get().rawBits();
}

}  // extern "C"
