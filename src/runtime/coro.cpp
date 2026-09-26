// The resumption of a coroutine body and the await subscription: the two
// operations every driver (builtin_generator.cpp, builtin_async.cpp,
// builtin_async_generator.cpp) is built from, and `bronze_coro_start`, the
// helper a generator or async function's stub calls with the fresh frame.

#include "runtime/coro.h"

#include <brass/interpreter/interpreter.hpp>
#include <brass/runtime/coroutine.hpp>
#include <brass/runtime/exception.hpp>

#include "abi/bronze_abi.h"
#include "runtime/async_generator.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/generator.h"
#include "runtime/iterator.h"
#include "runtime/promise.h"
#include "runtime/rt_roots.h"

namespace bronze::runtime {

namespace {

uintptr_t frameAddress(const Rooted<Value>& frame) {
    return reinterpret_cast<uintptr_t>(frame.get().asObject<void>());
}

[[noreturn]] void rethrowAsBrass(uint64_t bits) {
    throw brass::runtime::BrassException(brass::HostValue::from_raw(bits));
}

uint64_t resumeFrame(uintptr_t frame, uint64_t cell, uint32_t mode) {
    uint64_t thrown = 0;
    try {
        return brass_coro_resume_with(frame, cell, mode);
    } catch (const brass::InterpreterThrownException& e) {
        // A body brass runs in its interpreter tier raises its own kind; the
        // runtime knows one (exception.h).
        thrown = e.value().raw_bits();
    }
    rethrowAsBrass(thrown);
}

// The await's two resumptions. Their environment is the OWNER, which is how a
// suspended body stays reachable: the owner holds the frame, and the closures
// live in the awaited promise's reaction list or in a queued job (both traced).
void awaitSettled(Rooted<Value>& owner, uint32_t mode, Rooted<Value>& value) {
    if (rtIsIteratorObject(owner.get(), IteratorProto::AsyncGenerator)) {
        rtAsyncGeneratorResumeFromAwait(owner, mode, value);
    } else {
        rtAsyncFunctionResume(owner, mode, value);
    }
}

uint64_t awaitFulfilled(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> owner{Value(env)};
    Rooted<Value> value{args[0]};
    awaitSettled(owner, BRONZE_ABI_RESUME_NEXT, value);
    return Value::fromUndefined().rawBits();
}

uint64_t awaitRejected(uint64_t env, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args{argc, argv};
    Rooted<Value> owner{Value(env)};
    Rooted<Value> reason{args[0]};
    awaitSettled(owner, BRONZE_ABI_RESUME_THROW, reason);
    return Value::fromUndefined().rawBits();
}

}  // namespace

CoroStep rtCoroResume(Rooted<Value>& frame, uint32_t mode, Rooted<Value>& sent,
                      Rooted<Value>& out) {
    // The body reads the sent value through this cell's address, right after
    // its suspension: the cell is a root, so the value is current however
    // much the body allocates before it gets there.
    Rooted<Value> cell{sent.get()};
    const uint64_t bits = resumeFrame(frameAddress(frame), reinterpret_cast<uint64_t>(cell.slot_ptr()), mode);
    out.set(Value(bits));
    const auto* header = frame.get().asObject<brass::runtime::BrassCoroFrame>();
    if (header->is_done != 0) return CoroStep{.done = true};
    return CoroStep{.done = false, .kind = header->state_id & BRONZE_ABI_SUSPEND_KIND_MASK};
}

void rtCoroAdopt(Rooted<Value>& frame) { brass_coro_unroot(frameAddress(frame)); }

bool rtCoroAwait(Rooted<Value>& owner, Rooted<Value>& awaited, Rooted<Value>& thrown) {
    // THE SINGLE-TICK RULE lives in PromiseResolve: 27.2.4.7 step 2 returns an
    // argument that already IS an intrinsic promise unchanged, so `await p`
    // subscribes `p` itself — one reaction job, the one `.then(f)` would cost.
    Rooted<Value> promise{Value::fromUndefined()};
    Value caught;
    if (rtTryCatch([&] { promise.set(rtPromiseResolveValue(awaited)); }, caught)) {
        thrown.set(caught);
        return false;
    }
    Rooted<Value> onFulfilled{rtMakeNativeClosure(awaitFulfilled, owner, 1)};
    Rooted<Value> onRejected{rtMakeNativeClosure(awaitRejected, owner, 1)};
    // No capability: the reaction settles nothing of its own; the resumed
    // body's completion is what settles the owner's promise.
    Rooted<Value> noCapability{Value::fromUndefined()};
    rtPerformPromiseThen(promise, onFulfilled, onRejected, noCapability);
    return true;
}

}  // namespace bronze::runtime

extern "C" {

// `coro.start`: the stub of a generator or async function hands over the
// frame it just created. Until the owner below holds it, the frame is one of
// brass's roots; from then on it lives as long as its owner does.
uint64_t bronze_coro_start(uint32_t kind, uint64_t frameAddr) {
    using namespace bronze;
    using namespace bronze::runtime;
    Rooted<Value> frame{Value::fromObject(reinterpret_cast<const void*>(frameAddr))};
    switch (kind) {
        case BRONZE_ABI_CORO_GENERATOR: return rtCreateGeneratorObject(frame).rawBits();
        case BRONZE_ABI_CORO_ASYNC: return rtStartAsyncFunction(frame).rawBits();
        case BRONZE_ABI_CORO_ASYNC_GENERATOR: return rtCreateAsyncGeneratorObject(frame).rawBits();
        default: break;
    }
    bronze::fatal("internal: coro.start with an unknown coroutine kind");
}

}  // extern "C"
