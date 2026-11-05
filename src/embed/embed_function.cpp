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

#include <cstdio>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "embed/embed_fast_function.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/native_handle.h"
#include "runtime/object.h"
#include "runtime/root_slots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
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
    // The builtin prologue (rt_roots.h): copy the receiver and the arguments
    // into rooted slots before anything can allocate, and never read `argv`
    // again. A frame of our own, because the nearest generated frame roots
    // only its own slots — and the copy is rooted as the frame's one span,
    // not slot by slot: this runs on every WebGL call bro makes, and the
    // per-slot push and pop were the larger half of it.
    // The block first, so it outlives the frame whose span names it.
    RootValueBlock block(argc + 1);
    ShadowStackFrame frame;
    Value* slots = block.data();
    slots[0] = Value(this_bits);
    for (uint32_t i = 0; i < argc; ++i) slots[i + 1] = Value(argv[i]);
    frame.rootSpan(slots, argc + 1);

    // The std::function lives on the C++ heap, so the pointer is stable —
    // read once, before the callback can move anything. The environment is
    // the runtime's own cell, never a program's argument, so it is read
    // whichever realm made it: a host function is called from every realm.
    auto* fn = static_cast<NativeFn*>(handleDataAnyRealm(Value(env_bits)));
    if (!fn) {
        // Diagnostic dump before dying: what the environment slot actually
        // holds decides which corruption class this is.
        Value env(env_bits);
        char buf[256];
        const char* shape = "non-object";
        uint64_t kindBits = 0, slot2 = 0;
        uint32_t islots = 0;
        if (env.isObject()) {
            auto* hdr = env.asObject<HeapObjectHeader>();
            kindBits = static_cast<uint64_t>(hdr->flags);
            shape = (hdr->flags == HeapKind::Plain) ? "plain-object" : "other-heap-kind";
            if (hdr->flags == HeapKind::Plain) {
                auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
                islots = obj->internalSlotCount();
                if (islots >= 3) slot2 = obj->internalSlot(2).rawBits();
            }
        }
        std::snprintf(buf, sizeof buf,
                      "embed: host fn env not a native handle: env=%016llx (%s kind=%llu "
                      "islots=%u slot2=%016llx) this=%016llx argc=%u",
                      (unsigned long long)env_bits, shape, (unsigned long long)kindBits,
                      islots, (unsigned long long)slot2, (unsigned long long)this_bits, argc);
        fatal(buf);
    }

    // The span points at the rooted block itself: the collector updates it in
    // place, so the arguments the host reads stay current across anything it
    // does. The receiver is a copy — embed.h tells the host to re-root it
    // before allocating.
    //
    // A C++ exception out of the host (a library's std::invalid_argument, a
    // bad_alloc for a size a program asked for) becomes the JS error the call
    // throws, so a native refusing its input is never the process's end. A
    // JS throw is a BrassException, not a std::exception, and passes through.
    // The JS error is raised outside the handler, once the C++ exception is
    // gone.
    std::string failure;
    bool outOfMemory = false;
    try {
        Value result = (*fn)(slots[0], std::span<const Value>(slots + 1, argc));
        return result.rawBits();
    } catch (const std::bad_alloc&) {
        outOfMemory = true;
    } catch (const std::exception& e) {
        failure = e.what();
        if (failure.empty()) failure = "a native call failed";
    }
    if (outOfMemory) runtime::rtThrowOutOfMemory();
    runtime::rtThrowError(runtime::ErrorKind::Error, failure);
}


// A C++ exception out of a fast host function becomes the call's JS error,
// raised once the C++ exception is gone (as hostTrampoline does). The message
// is carried in a heap string the handler allocates, so the happy path of a
// fast call holds nothing but a pointer — no std::string, no TLS access.
[[noreturn]] void throwFastHostFailure(std::string* failure) {
    if (!failure) runtime::rtThrowOutOfMemory();
    std::string message = std::move(*failure);
    delete failure;
    runtime::rtThrowError(runtime::ErrorKind::Error, message);
}

// ---- the fast host function (embed_fast_function.h) ------------------------

// The env cell's payload: what makeFastFunction was handed.
struct FastHost {
    FastNativeFn fn;
    void* data;
    HandleDestructor dataDtor;
};

void destroyFastHost(void* p) {
    auto* host = static_cast<FastHost*>(p);
    if (host->dataDtor) host->dataDtor(host->data);
    delete host;
}

// The env cell is the one makeFastFunction made, and nothing reaches a
// function's env slot but the factory: no brand or realm check, only the data
// slot read (internalSlot keeps its bounds check, which fatals on a cell of
// the wrong shape rather than reading past it).
inline FastHost* fastHostOf(uint64_t env_bits) {
    auto* cell = Value(env_bits).asObject<ObjectHeader>();
    return reinterpret_cast<FastHost*>(
        static_cast<uintptr_t>(cell->internalSlot(runtime::kHandleDataSlot).rawBits()));
}

Value callFastHost(FastHost* host, Value self, const Value* args, uint32_t argc) {
    std::string* failure = nullptr;  // null after a bad_alloc
    try {
        return host->fn(host->data, self, std::span<const Value>(args, argc));
    } catch (const std::bad_alloc&) {
    } catch (const std::exception& e) {
        const char* what = e.what();
        failure = new std::string(what && *what ? what : "a native call failed");
    }
    throwFastHostFailure(failure);
}

// More arguments than the stack copy holds: the rooted path, as hostTrampoline.
uint64_t fastHostTrampolineWide(FastHost* host, uint64_t this_bits, uint32_t argc,
                                const uint64_t* argv) {
    RootValueBlock block(argc);
    ShadowStackFrame frame;
    Value* slots = block.data();
    for (uint32_t i = 0; i < argc; ++i) slots[i] = Value(argv[i]);
    frame.rootSpan(slots, argc);
    return callFastHost(host, Value(this_bits), slots, argc).rawBits();
}

// The fast trampoline. The arguments are copied into an array in THIS frame
// before anything can allocate, and `argv` is never read again: `argv` may be
// memory no stack names (a builtin's heap-allocated block, rooted only by its
// own frame and updated there), while every word of this frame is read by the
// collector's conservative stack scan, which pins (young) or marks (old) each
// object a word names — so the copy needs no root registration to stay
// current. The array outlives the callback because its address escapes into
// the call (no sibling call can reuse the frame).
uint64_t fastHostTrampoline(uint64_t env_bits, uint64_t this_bits, uint32_t argc,
                            const uint64_t* argv) {
    FastHost* host = fastHostOf(env_bits);
    if (argc > kFastInlineArgs) [[unlikely]] {
        return fastHostTrampolineWide(host, this_bits, argc, argv);
    }
    // Uninitialized storage: only the first argc words are ever read, and a
    // stale word in the rest is at worst one more conservative root.
    union Slots {
        Slots() {}
        Value v[kFastInlineArgs];
    } slots;
    // A word-per-argument copy through a jump table. Spelled as a loop, the
    // compiler turns it into memcpy and expands that, for under 64 bytes,
    // into a BYTE loop: eight iterations an argument on every host call.
    static_assert(kFastInlineArgs == 16, "the copy below is unrolled for 16");
    switch (argc) {
        case 16: slots.v[15] = Value(argv[15]); [[fallthrough]];
        case 15: slots.v[14] = Value(argv[14]); [[fallthrough]];
        case 14: slots.v[13] = Value(argv[13]); [[fallthrough]];
        case 13: slots.v[12] = Value(argv[12]); [[fallthrough]];
        case 12: slots.v[11] = Value(argv[11]); [[fallthrough]];
        case 11: slots.v[10] = Value(argv[10]); [[fallthrough]];
        case 10: slots.v[9] = Value(argv[9]); [[fallthrough]];
        case 9: slots.v[8] = Value(argv[8]); [[fallthrough]];
        case 8: slots.v[7] = Value(argv[7]); [[fallthrough]];
        case 7: slots.v[6] = Value(argv[6]); [[fallthrough]];
        case 6: slots.v[5] = Value(argv[5]); [[fallthrough]];
        case 5: slots.v[4] = Value(argv[4]); [[fallthrough]];
        case 4: slots.v[3] = Value(argv[3]); [[fallthrough]];
        case 3: slots.v[2] = Value(argv[2]); [[fallthrough]];
        case 2: slots.v[1] = Value(argv[1]); [[fallthrough]];
        case 1: slots.v[0] = Value(argv[0]); [[fallthrough]];
        default: break;
    }
    return callFastHost(host, Value(this_bits), slots.v, argc).rawBits();
}

}  // namespace

Value makeFunction(NativeFn fn, uint32_t arity) {
    return makeFunction(std::move(fn), arity, std::string_view());
}

namespace {

// The function object both factories build around `code`: `env` is the
// native handle cell owning the host's state. The caller has opened a root
// frame and rooted `env`.
Value makeHostFunctionObject(NativeFunctionCode code, Rooted<Value>& env, uint32_t arity,
                             std::string_view name) {
    // Same shape as bronze_create_function (rt_object.cpp): allocate first,
    // then read the environment through the root — the allocation can collect,
    // and a copy taken before it would point into dead from-space.
    // NEEDS_ENV like the bound-function and promise-pair factories: the
    // trampoline reads the env cell on every call, so the method-call IC must
    // never latch this function onto its env-less direct dispatch.
    FunctionHeader* fnObj =
        FunctionHeader::create(runtime::rtHeap(), code, Value::fromUndefined(), 0,
                               BRONZE_ABI_FN_FLAGS_ORDINARY | BRONZE_ABI_FN_FLAG_NATIVE |
                                   BRONZE_ABI_FN_FLAG_NEEDS_ENV);
    fnObj->env_record = env.get();
    fnObj->header.flags = HeapKind::Function;
    fnObj->length = arity;
    // Unnamed by default, like every native builtin: `f.name` then stays the
    // named refusal rt_state.cpp's rtSetFunctionNameAndLength documents,
    // rather than two wrong facts.
    //
    // A host that HAS a name says so, and it is a real fact rather than a
    // courtesy: a host standing in for a language feature answers for that
    // feature's naming too. 27.3.1.1 names every function it builds
    // `anonymous`, and library code reads `.name` — so a host answering
    // CreateDynamicFunction must be able to spell it, and before this it could
    // not: setProperty refuses `name` by name (10.2.9 makes it slot-backed),
    // which is correct and left no way in at all.
    // Rooted before the name is minted: that mint is a heap allocation, so it
    // can collect, and the function it names would otherwise be written to —
    // and returned from — its dead from-space address. (Found by the native
    // suite's GC-stress run: every named host function came back as garbage.)
    Rooted<Value> fnRoot{Value::fromObject(fnObj)};
    if (!name.empty()) {
        StringHeader* interned = StringHeader::internToArena(
            runtime::rtArena(), StringHeader::createFromUTF8(runtime::rtHeap(), name));
        fnRoot.get().asObject<FunctionHeader>()->name = interned;
    }
    return fnRoot.get();
}

}  // namespace

Value makeFunction(NativeFn fn, uint32_t arity, std::string_view name) {
    ShadowStackFrame frame;
    // The handle owns the callable; its finalizer is the destructor of a host
    // function that has become garbage. A stateless lambda, so it converts to
    // the plain function pointer the registry stores.
    auto* boxed = new NativeFn(std::move(fn));
    Rooted<Value> env{makeHandle(boxed, [](void* p) { delete static_cast<NativeFn*>(p); })};
    return makeHostFunctionObject(hostTrampoline, env, arity, name);
}

Value makeFastFunction(FastNativeFn fn, void* data, HandleDestructor dataDtor, uint32_t arity,
                       std::string_view name) {
    ShadowStackFrame frame;
    // The same handle-cell env as makeFunction's, owning a FastHost instead
    // of a std::function: the collector forwards it with the function, and
    // its finalizer releases the host's data once the function is garbage.
    auto* host = new FastHost{fn, data, dataDtor};
    Rooted<Value> env{makeHandle(host, destroyFastHost)};
    return makeHostFunctionObject(fastHostTrampoline, env, arity, name);
}

}  // namespace bronze::embed
