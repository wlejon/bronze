// Host globals, calling into compiled code, and the value conversions — the
// pieces of the embed API that are thin over one runtime entry point each.
//
// ROOTING PATTERN, shared by every embed translation unit: each entry point
// that can allocate opens a ShadowStackFrame of its own and takes what it
// holds through Rooted<> handles. The API cannot assume the host set a frame
// up — a game engine's frame loop knows nothing about bronze's shadow stack —
// and a Rooted with no current frame is silently unrooted, which is exactly
// the class of bug BRONZE_GC_STRESS exists to catch. Per-call frames make the
// contract local: inside an embed function everything is rooted; a Value the
// HOST holds across a call is the host's to keep alive (embed.h, Persistent).

#include "embed/embed.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/microtask.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/string.h"

namespace bronze::embed {

// ---- host globals ----------------------------------------------------------

void registerGlobal(std::string_view name, Value value) {
    runtime::rtRegisterHostGlobal(std::string(name), value);
}

bool hasHostGlobal(std::string_view name) {
    Value ignored = Value::fromUndefined();
    return runtime::rtHostGlobalLookup(std::string(name), ignored);
}

GlobalValue globalValue(std::string_view name) {
    // A frame: the builtin ladder constructs its namespaces lazily, so the
    // first ask for "Math" in a process allocates it.
    ShadowStackFrame frame;
    const std::string key(name);
    Value out = Value::fromUndefined();
    // The same order bronze_global_get resolves in, so this probe and a
    // compiled read cannot answer differently for a name both can see.
    if (runtime::rtResolveBuiltinGlobal(key, out)) return {out, true};
    if (runtime::rtHostGlobalLookup(key, out)) return {out, true};
    if (runtime::rtGlobalThisOwnLookup(key, out)) return {out, true};
    return {Value::fromUndefined(), false};
}

// ---- calling into compiled code --------------------------------------------

CallResult call(Value fn, Value thisValue, std::span<const Value> args) {
    ShadowStackFrame frame;
    Rooted<Value> fnRoot{fn};
    Rooted<Value> thisRoot{thisValue};
    // RootedBlock rather than a plain stack array for the same reason
    // bronze_construct uses one (rt_roots.h): the collector must be able to
    // update the argument slots if anything between here and the callee's
    // prologue collects — and a host calls from outside any generated frame,
    // so nothing else roots these.
    runtime::RootedBlock block(static_cast<uint32_t>(args.size()));
    for (uint32_t i = 0; i < args.size(); ++i) block.set(i, args[i]);

    Rooted<Value> result{Value(bronze_dynamic_call(fnRoot.get().rawBits(),
                                                   thisRoot.get().rawBits(), block.count(),
                                                   block.data()))};

    // The cell, not the return value, says whether the call threw — a helper
    // that raises returns undefined by the runtime's own convention
    // (exception.h). Cleared here because the host boundary is where
    // propagation ends: there is no enclosing JS frame left to unwind to, and
    // a pending cell left set would make the NEXT call into compiled code
    // appear to throw its predecessor's exception.
    if (bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS) {
        CallResult out{Value(bronze_tls_block_addr()->exception_cell), /*thrown=*/true};
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
        return out;
    }
    return CallResult{result.get(), /*thrown=*/false};
}

CallResult construct(Value fn, std::span<const Value> args) {
    ShadowStackFrame frame;
    Rooted<Value> fnRoot{fn};
    // The same RootedBlock discipline as call(), for the same reason: nothing
    // outside this frame roots the arguments, and everything between here and
    // the instance allocation may collect.
    runtime::RootedBlock block(static_cast<uint32_t>(args.size()));
    for (uint32_t i = 0; i < args.size(); ++i) block.set(i, args[i]);

    Rooted<Value> result{
        Value(bronze_construct(fnRoot.get().rawBits(), block.count(), block.data()))};

    if (bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS) {
        CallResult out{Value(bronze_tls_block_addr()->exception_cell), /*thrown=*/true};
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
        return out;
    }
    return CallResult{result.get(), /*thrown=*/false};
}

// ---- property reads --------------------------------------------------------

namespace {

// The shared tail of both readers: the generic element-get (the path a
// computed `obj[key]` in compiled code takes), with the pending cell handled
// exactly as `call` handles it — the host boundary is where propagation ends,
// so a throwing getter answers undefined here rather than poisoning the next
// entry into compiled code.
Value elemGetAtHostBoundary(uint64_t objBits, uint64_t keyBits) {
    Rooted<Value> result{Value(bronze_elem_get(objBits, keyBits))};
    if (bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS) {
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
        return Value::fromUndefined();
    }
    return result.get();
}

}  // namespace

Value getProperty(Value obj, std::string_view key) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    // Receiver rooted before the key string is allocated — setProperty's
    // discipline, mirrored.
    Rooted<Value> keyRoot{runtime::rtMakeString(key)};
    return elemGetAtHostBoundary(self.get().rawBits(), keyRoot.get().rawBits());
}

Value getElement(Value obj, uint32_t index) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    return elemGetAtHostBoundary(self.get().rawBits(),
                                 Value::fromDouble(index).rawBits());
}

// ---- throw helpers ---------------------------------------------------------

Value throwValue(Value thrown) { return runtime::rtThrow(thrown); }

Value throwError(const std::string& message) {
    ShadowStackFrame frame;
    return runtime::rtThrowError(runtime::ErrorKind::Error, message);
}

Value throwTypeError(const std::string& message) {
    ShadowStackFrame frame;
    return runtime::rtThrowTypeError(message);
}

Value throwRangeError(const std::string& message) {
    ShadowStackFrame frame;
    return runtime::rtThrowRangeError(message);
}

// ---- value conversions -----------------------------------------------------

uint64_t toBits(Value v) { return v.rawBits(); }
Value fromBits(uint64_t bits) { return Value(bits); }

Value undefined() { return Value::fromUndefined(); }
Value null() { return Value::fromNull(); }
Value fromDouble(double d) { return Value::fromDouble(d); }
Value fromBool(bool b) { return Value::fromBool(b); }

Value fromUtf8(std::string_view utf8) {
    ShadowStackFrame frame;
    return runtime::rtMakeString(utf8);
}

// A frame, because ToNumber of an OBJECT is 7.1.4 step 1: ToPrimitive, which
// calls the host's own JS and allocates. A primitive still costs nothing.
double toDouble(Value v) {
    ShadowStackFrame frame;
    return runtime::rtToNumber(v);
}

bool toBool(Value v) { return bronze_truthy(v.rawBits()); }

std::string toUtf8(Value v) {
    // A string answers from its own bytes with no allocation; everything else
    // goes through the primitive ToString, which allocates a heap string this
    // reads and immediately abandons — fine, because nothing here survives
    // the call.
    if (v.isString()) {
        return runtime::rtUtf8Chars(v.asString<StringHeader>());
    }
    ShadowStackFrame frame;
    Rooted<Value> str{runtime::rtValueToString(v)};
    return runtime::rtUtf8Chars(str.get().asString<StringHeader>());
}

bool isUndefined(Value v) { return v.isUndefined(); }
bool isNull(Value v) { return v.isNull(); }

bool isFunction(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

bool isObject(Value v) { return v.isObject(); }

bool isSymbol(Value v) { return v.isSymbol(); }

// ---- json parsing ----------------------------------------------------------

CallResult parseJson(std::string_view jsonUtf8) {
    ShadowStackFrame frame;
    Rooted<Value> result{runtime::rtJsonParse(jsonUtf8)};
    if (bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS) {
        CallResult out{Value(bronze_tls_block_addr()->exception_cell), /*thrown=*/true};
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
        return out;
    }
    return CallResult{result.get(), /*thrown=*/false};
}

// ---- the microtask checkpoint ----------------------------------------------

// The frame loop's half of the checkpoint. A host that keeps calling into
// compiled code — a game loop invoking `update()` once a frame — has to pump
// the queue between calls, or every promise the program made would settle only
// at shutdown. It is the HOST's call rather than something `call()` does for
// it, because where the checkpoint falls is the host's event-loop design and
// not the runtime's: ECMA-262 9.5 leaves it to the host, and a drain hidden
// inside every call would run jobs in the middle of a frame the host meant to
// be atomic.
//
// Here rather than in embed_run.cpp, which is quarantined around its one
// reference to `bronze_main` — a test binary pumping the queue must not have
// to link the compiled program. Its own root frame, per this file's rooting
// pattern, so a host may pump from a stack with no bronze frame on it.
void drainMicrotasks() {
    {
        ShadowStackFrame frame;
        runtime::rtDrainMicrotasks();
    }
    // Deferred handle destructors ride the same checkpoint (embed.h says why
    // they exist), AFTER the job queue: the frame's teardown notifications
    // come after the frame's work. Outside the frame above — the destructors
    // are plain host code and reach the heap only through embed entry points,
    // each of which opens its own frame.
    drainFinalizers();
}

bool microtasksPending() { return runtime::rtMicrotasksPending(); }

}  // namespace bronze::embed
