// Building objects from the host: thin wrappers over what the runtime already
// does for its own namespace objects (builtin_math.cpp, rtDefineMethods) —
// the same shape, the same setProp/defineAccessor calls, the same rooting
// discipline — so a host-built object is indistinguishable from a
// runtime-built one on every property path.

#include <string>

#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"

namespace bronze::embed {

namespace {

// The receiver every function here demands. A host handing an array or a
// function to setProperty would have its payload read as a Shape*, so the
// refusal is loud and up front — hard errors over silent fallbacks.
ObjectHeader* requirePlainObject(Value v, const char* who) {
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        fatal((std::string("embed: ") + who + " needs a plain object receiver").c_str());
    }
    return v.asObject<ObjectHeader>();
}

}  // namespace

Value createObject() {
    ShadowStackFrame frame;
    // The shape every `{}` literal shares, so a host-built object sits on the
    // same inline-cache paths — and inherits from Object.prototype the same
    // way (rtPlainObjectShape names the intrinsic as its prototype).
    ObjectHeader* obj = ObjectHeader::create(runtime::rtHeap(), runtime::rtArena(),
                                             runtime::rtPlainObjectShape());
    return Value::fromObject(obj);
}

Value setProperty(Value obj, std::string_view key, Value v) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    Rooted<Value> val{v};
    // Receiver and value are rooted BEFORE the key string is allocated; from
    // here every raw pointer is re-derived through a root.
    Rooted<Value> keyRoot{runtime::rtMakeString(key)};
    requirePlainObject(self.get(), "setProperty");
    // defineOwn: a definition, not an assignment — a host building an object
    // must never run an inherited setter, exactly as rtDefineMethods must not.
    ObjectHeader* live = self.get().asObject<ObjectHeader>()->setProp(
        runtime::rtHeap(), runtime::rtArena(), keyRoot, val, nullptr,
        /*enumerable=*/true, /*defineOwn=*/true);
    return Value::fromObject(live);
}

Value setElement(Value obj, uint32_t index, Value v) {
    // On a plain object an integer key IS the canonical numeric string —
    // enumeration order and the element paths both key off the spelling, and
    // std::to_string of a uint32 is exactly canonical.
    return setProperty(obj, std::to_string(index), v);
}

Value defineAccessor(Value obj, std::string_view key, Value getter, Value setter,
                     bool enumerable) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    Rooted<Value> get{getter};
    Rooted<Value> set{setter};
    Rooted<Value> keyRoot{runtime::rtMakeString(key)};
    requirePlainObject(self.get(), "defineAccessor");
    ObjectHeader::defineAccessor(runtime::rtHeap(), runtime::rtArena(), self, keyRoot, get, set,
                                 enumerable);
    return self.get();
}

Value freeze(Value obj) {
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    // The runtime's own Object.freeze, argument block and all, so the host
    // path and the program path cannot disagree about what freezing means.
    uint64_t argBits = self.get().rawBits();
    return Value(runtime::rtObjectFreeze(Value::fromUndefined().rawBits(),
                                         Value::fromUndefined().rawBits(), 1, &argBits));
}

}  // namespace bronze::embed
