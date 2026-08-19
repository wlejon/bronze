// A host C++ callable as a bronze function object: the trampoline that
// conforms a NativeFn to bronze_fn_code, and the factory that pairs it with
// the closure state.
//
// The state rides where a closure's does — in the function object's
// environment slot — as a native handle cell owning a heap-allocated
// std::function. That is the whole trick: the cell is an ordinary heap value
// the GC payload scan forwards with the function object, so `env_bits` at call
// time is always the cell's CURRENT address, and the finalizer registry
// deletes the std::function when the function object dies.

#include <span>
#include <utility>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::embed {

namespace {

// One trampoline for every host function — which functions apart is the env
// cell, never the code pointer. That is why the factory below builds through
// FunctionHeader::create and NOT bronze_function_singleton: the singleton
// table interns on the code pointer, and every host function sharing this
// address would collapse into one object, the way closures would if they
// interned.
uint64_t hostTrampoline(uint64_t env_bits, uint64_t this_bits, uint32_t argc,
                        const uint64_t* argv) {
    // The builtin prologue (rt_roots.h): copy the arguments into rooted
    // slots before anything can allocate, and never read `argv` again. A
    // frame of our own, because the nearest generated frame roots only its
    // own slots.
    ShadowStackFrame frame;
    runtime::RootedArgs args(argc, argv);
    Rooted<Value> thisRoot{Value(this_bits)};

    // The std::function lives on the C++ heap, so the pointer is stable —
    // read once, before the callback can move anything.
    auto* fn = static_cast<NativeFn*>(handleData(Value(env_bits)));
    if (!fn) {
        fatal("embed: a host function whose environment is not a native handle "
              "(the trampoline reached a function the factory did not build)");
    }

    // The span points at RootedArgs' own slots: the collector updates them in
    // place, so the arguments the host reads stay current across anything it
    // does. The receiver is a copy — embed.h tells the host to re-root it
    // before allocating.
    Value result = (*fn)(thisRoot.get(),
                         std::span<const Value>(args.data(), args.count()));
    return result.rawBits();
}

}  // namespace

Value makeFunction(NativeFn fn, uint32_t arity) {
    ShadowStackFrame frame;
    // The handle owns the callable; its finalizer is the destructor of a host
    // function that has become garbage. A stateless lambda, so it converts to
    // the plain function pointer the registry stores.
    auto* boxed = new NativeFn(std::move(fn));
    Rooted<Value> env{makeHandle(boxed, [](void* p) { delete static_cast<NativeFn*>(p); })};

    // Same shape as bronze_create_function (rt_object.cpp): allocate first,
    // then read the environment through the root — the allocation can collect,
    // and a copy taken before it would point into dead from-space.
    FunctionHeader* fnObj =
        FunctionHeader::create(runtime::rtHeap(), hostTrampoline, Value::fromUndefined(), arity,
                               BRONZE_ABI_FN_FLAGS_ORDINARY | BRONZE_ABI_FN_FLAG_NATIVE);
    fnObj->env_record = env.get();
    fnObj->header.flags = HeapKind::Function;
    // No name key and no length, like every native builtin: `f.name` on a
    // host function stays the named refusal rt_state.cpp's
    // rtSetFunctionNameAndLength documents, rather than two wrong facts.
    return Value::fromObject(fnObj);
}

}  // namespace bronze::embed
