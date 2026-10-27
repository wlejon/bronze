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
#include "runtime/tls_block.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/realm.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/weak_ref.h"

namespace bronze::embed {

// ---- host globals ----------------------------------------------------------

void registerGlobal(std::string_view name, Value value) {
    runtime::rtRegisterHostGlobal(std::string(name), value);
}

bool hasHostGlobal(std::string_view name) {
    return runtime::rtHostGlobalRegistered(std::string(name));
}

namespace {

// Enters the realm whose global object `receiver` is, for as long as it lives,
// when that is not the current realm: a lazy name belongs to the realm it was
// registered in (host_globals.h), so its accessor must force and register
// there whichever realm's code happened to read it.
class OwningRealmScope {
public:
    explicit OwningRealmScope(Value receiver) {
        Realm* owner = bronze::rtRealmOfGlobal(receiver);
        if (owner && owner != bronze::rtCurrentRealm()) {
            bronze::rtEnterRealm(owner);
            entered_ = true;
        }
    }
    ~OwningRealmScope() {
        if (entered_) bronze::rtExitRealm();
    }
    OwningRealmScope(const OwningRealmScope&) = delete;
    OwningRealmScope& operator=(const OwningRealmScope&) = delete;

private:
    bool entered_ = false;
};

// The accessor pair a pending lazy global shows on a realm's global object
// (runtime/host_globals.h): reading forces the name and answers what its
// init registered; assigning forces it and registers the assigned value.
Value defineLazyGlobalAccessor(Value globalObject, const std::string& name) {
    ShadowStackFrame frame;
    Rooted<Value> glob{globalObject};
    // Each callback copies the name first: forcing deletes the accessor, and
    // a collection can then free the function object, and with it this
    // closure, while the callback is still running.
    Rooted<Value> getter{makeFunction(
        [name](Value self, std::span<const Value>) -> Value {
            const std::string key = name;
            OwningRealmScope scope(self);
            Value out = Value::fromUndefined();
            runtime::rtHostGlobalLookup(key, out);
            return out;
        },
        0)};
    Rooted<Value> setter{makeFunction(
        [name](Value self, std::span<const Value> args) -> Value {
            const std::string key = name;
            const Rooted<Value> v{args.empty() ? Value::fromUndefined() : args[0]};
            OwningRealmScope scope(self);
            runtime::rtForceLazyHostGlobal(key);
            runtime::rtRegisterHostGlobal(key, v.get());
            return Value::fromUndefined();
        },
        1)};
    Rooted<Value> keyRoot{runtime::rtMakeString(name)};
    ObjectHeader::defineAccessor(runtime::rtHeap(), runtime::rtArena(), glob, keyRoot, getter, setter,
                                 /*enumerable=*/false);
    return glob.get();
}

}  // namespace

void registerLazyGlobal(std::string_view name, std::function<void()> init) {
    runtime::rtSetLazyGlobalAccessorHook(&defineLazyGlobalAccessor);
    runtime::rtRegisterLazyHostGlobal(std::string(name), std::move(init));
}

void forceLazyGlobal(std::string_view name) { runtime::rtForceLazyHostGlobal(std::string(name)); }

void runAsLazyInit(const std::function<void()>& init) { runtime::rtRunAsLazyInit(init); }

std::vector<std::string> hostGlobalNames() {
    const auto& entries = runtime::rtHostGlobalEntries();
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) names.push_back(entry.first);
    return names;
}

GlobalValue globalValue(std::string_view name) {
    // A frame: the builtin ladder constructs its namespaces lazily, so the
    // first ask for "Math" in a process allocates it.
    ShadowStackFrame frame;
    const std::string key(name);
    Value out = Value::fromUndefined();
    // The same order bronze_global_get resolves in, so this probe and a
    // compiled read cannot answer differently for a name both can see.
    if (runtime::rtHostShadowsBuiltin(key)) {
        if (runtime::rtHostGlobalLookup(key, out)) return {out, true};
    }
    if (runtime::rtResolveBuiltinGlobal(key, out)) return {out, true};
    if (runtime::rtHostGlobalLookup(key, out)) return {out, true};
    if (runtime::rtGlobalThisOwnLookup(key, out)) return {out, true};
    return {Value::fromUndefined(), false};
}

void setDynamicFunctionHook(DynamicFunctionHook hook) {
    runtime::rtSetDynamicFunctionHost(std::move(hook));
}

void setDynamicEvalHook(DynamicEvalHook hook) {
    runtime::rtSetDynamicEvalHost(std::move(hook));
}

void setDynamicImportHook(DynamicImportHook hook) {
    runtime::rtSetDynamicImportHost(std::move(hook));
}

// ---- calling into compiled code --------------------------------------------

namespace {

// A host entry into a function runs it in the function's realm
// (runtime/realm.h, CalleeRealmScope): a host keeping several realms on a
// thread (one per app it hosts) calls a callback from wherever it happens to
// be, and the callback must read its own globals and its own module
// instances, never the caller's.
uint32_t realmSlotOf(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return 0;
    return fn.asObject<FunctionHeader>()->realmSlot();
}

}  // namespace

CallResult call(Value fn, Value thisValue, std::span<const Value> args) {
    ShadowStackFrame frame;
    CalleeRealmScope realm(realmSlotOf(fn));
    if (realm.dead()) {
        Value err = Value::fromUndefined();
        runtime::rtTryCatch([&] { runtime::rtThrowTypeError(kDeadRealmCall); }, err);
        return CallResult{err, true};
    }
    Rooted<Value> fnRoot{fn};
    Rooted<Value> thisRoot{thisValue};
    // RootedBlock rather than a plain stack array for the same reason
    // bronze_construct uses one (rt_roots.h): the collector must be able to
    // update the argument slots if anything between here and the callee's
    // prologue collects — and a host calls from outside any generated frame,
    // so nothing else roots these.
    runtime::RootedBlock block(static_cast<uint32_t>(args.size()));
    for (uint32_t i = 0; i < args.size(); ++i) block.set(i, args[i]);

    // The host boundary is where propagation ends: the throw is caught here
    // and handed back as a value, so the host never sees a C++ exception.
    Value out = Value::fromUndefined();
    const bool threw = runtime::rtTryCatch(
        [&] {
            out = Value(bronze_dynamic_call(fnRoot.get().rawBits(), thisRoot.get().rawBits(),
                                            block.count(), block.data()));
        },
        out);
    return CallResult{out, threw};
}

CallResult construct(Value fn, std::span<const Value> args) {
    ShadowStackFrame frame;
    CalleeRealmScope realm(realmSlotOf(fn));
    if (realm.dead()) {
        Value err = Value::fromUndefined();
        runtime::rtTryCatch([&] { runtime::rtThrowTypeError(kDeadRealmCall); }, err);
        return CallResult{err, true};
    }
    Rooted<Value> fnRoot{fn};
    // The same RootedBlock discipline as call(), for the same reason: nothing
    // outside this frame roots the arguments, and everything between here and
    // the instance allocation may collect.
    runtime::RootedBlock block(static_cast<uint32_t>(args.size()));
    for (uint32_t i = 0; i < args.size(); ++i) block.set(i, args[i]);

    Value out = Value::fromUndefined();
    const bool threw = runtime::rtTryCatch(
        [&] {
            out = Value(bronze_construct(fnRoot.get().rawBits(), block.count(), block.data()));
        },
        out);
    return CallResult{out, threw};
}

// ---- property reads --------------------------------------------------------

namespace {

// The shared tail of both readers: the generic element-get (the path a
// computed `obj[key]` in compiled code takes), with a throw caught exactly as
// `call` catches it — the host boundary is where propagation ends, so a
// throwing getter answers undefined here.
Value elemGetAtHostBoundary(uint64_t objBits, uint64_t keyBits) {
    Value out = Value::fromUndefined();
    if (runtime::rtTryCatch([&] { out = Value(bronze_elem_get(objBits, keyBits)); }, out)) {
        return Value::fromUndefined();
    }
    return out;
}

}  // namespace

Value getProperty(Value obj, std::string_view key) {
    if (obj.isObject() && obj.asObject<HeapObjectHeader>()->flags == HeapKind::Array &&
        key == "length") {
        return Value::fromDouble(obj.asObject<ArrayHeader>()->length);
    }
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    // Receiver rooted before the key string is allocated — setProperty's
    // discipline, mirrored.
    Rooted<Value> keyRoot{runtime::rtMakeString(key)};
    return elemGetAtHostBoundary(self.get().rawBits(), keyRoot.get().rawBits());
}

Value getElement(Value obj, uint32_t index) {
    if (obj.isObject() && index <= 4294967294u &&
        obj.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        return obj.asObject<ArrayHeader>()->getElem(index);
    }
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    return elemGetAtHostBoundary(self.get().rawBits(),
                                 Value::fromDouble(index).rawBits());
}

// ---- throw helpers ---------------------------------------------------------

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4646)  // embed.h: noreturn with a non-void type
#endif
Value throwValue(Value thrown) { runtime::rtThrow(thrown); }

Value throwError(const std::string& message) {
    ShadowStackFrame frame;
    runtime::rtThrowError(runtime::ErrorKind::Error, message);
}

Value throwTypeError(const std::string& message) {
    ShadowStackFrame frame;
    runtime::rtThrowTypeError(message);
}

Value throwRangeError(const std::string& message) {
    ShadowStackFrame frame;
    runtime::rtThrowRangeError(message);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---- value conversions -----------------------------------------------------

uint64_t toBits(Value v) { return v.rawBits(); }
Value fromBits(uint64_t bits) { return Value(bits); }

Value undefined() { return Value::fromUndefined(); }
Value null() { return Value::fromNull(); }
Value fromDouble(double d) { return Value::fromDouble(d); }
Value fromBool(bool b) { return Value::fromBool(b); }

Value fromUtf8(std::string_view utf8) {
    ShadowStackFrame frame;
    // A host's bytes, as the WHATWG decoder reads them (U+FFFD for each
    // maximal ill-formed subpart, an encoded surrogate included).
    return Value::fromString(StringHeader::createFromHostUTF8(runtime::rtHeap(), utf8));
}

// A frame, because ToNumber of an OBJECT is 7.1.4 step 1: ToPrimitive, which
// A frame, because ToNumber of an OBJECT is 7.1.4 step 1: ToPrimitive, which
// calls the host's own JS and allocates. A primitive still costs nothing.
double toDouble(Value v) {
    if (v.isNumber()) {
        return v.asNumber();
    }
    if (v.isBigInt()) {
        return runtime::rtBigIntToNumber(v);
    }
    ShadowStackFrame frame;
    return runtime::rtToNumber(v);
}

uint64_t toUint64(Value v) {
    if (v.isBigInt()) {
        uint64_t bits = 0;
        if (runtime::rtBigIntToRawBits64(v, bits)) return bits;
        return static_cast<uint64_t>(runtime::rtBigIntToNumber(v));
    }
    if (v.isNumber()) {
        double d = v.asNumber();
        if (!std::isnan(d)) return static_cast<uint64_t>(d);
    }
    return 0;
}

int64_t toInt64(Value v) {
    if (v.isBigInt()) {
        uint64_t bits = 0;
        if (runtime::rtBigIntToRawBits64(v, bits)) return static_cast<int64_t>(bits);
        return static_cast<int64_t>(runtime::rtBigIntToNumber(v));
    }
    if (v.isNumber()) {
        double d = v.asNumber();
        if (!std::isnan(d)) return static_cast<int64_t>(d);
    }
    return 0;
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
bool isNumber(Value v) { return v.isNumber(); }
bool isBigInt(Value v) { return v.isBigInt(); }
bool isString(Value v) { return v.isString(); }
bool isBool(Value v) { return v.isBool(); }

// ---- json parsing ----------------------------------------------------------

CallResult parseJson(std::string_view jsonUtf8) {
    ShadowStackFrame frame;
    Value out = Value::fromUndefined();
    const bool threw = runtime::rtTryCatch([&] { out = runtime::rtJsonParse(jsonUtf8); }, out);
    return CallResult{out, threw};
}

CallResult catchThrow(const std::function<Value()>& body) {
    ShadowStackFrame frame;
    Value out = Value::fromUndefined();
    const bool threw = runtime::rtTryCatch([&] { out = body(); }, out);
    return CallResult{out, threw};
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

void clearKeptObjects() { runtime::rtClearKeptObjects(); }

}  // namespace bronze::embed
