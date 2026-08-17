// Building objects from the host: thin wrappers over what the runtime already
// does for its own namespace objects (builtin_math.cpp, rtDefineMethods) —
// the same shape, the same setProp/defineAccessor calls, the same rooting
// discipline — so a host-built object is indistinguishable from a
// runtime-built one on every property path.

#include <string>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"

namespace bronze::embed {

namespace {

// The receiver setElement's define-half and defineAccessor demand. A host
// handing an array or a function here would have its payload read as a
// Shape*, so the refusal is loud and up front — hard errors over silent
// fallbacks. (setProperty widened past this to function receivers; the other
// two stay narrow until a host needs them wider.)
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

    // A function receiver: the definition lands on the function's own-property
    // object, exactly where bronze_method_def puts a class `static` — which is
    // what `URL.createObjectURL` from the host is. The three keys that are NOT
    // in that table are refused by name rather than silently shadowed there:
    // `name` and `length` are non-writable own properties the read path
    // answers first (rt_prop_write.cpp says why a table entry would lie), and
    // `prototype` lives in a FunctionHeader slot no table write can reach.
    if (self.get().isObject() &&
        self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        if (key == "name" || key == "length" || key == "prototype") {
            fatal((std::string("embed: setProperty cannot define `") + std::string(key) +
                   "` on a function (slot-backed or non-writable per 10.2)")
                      .c_str());
        }
        runtime::rtEnsureFunctionProperties(self);
        Rooted<Value> propsRoot{self.get().asObject<FunctionHeader>()->properties};
        propsRoot.get().asObject<ObjectHeader>()->setProp(
            runtime::rtHeap(), runtime::rtArena(), keyRoot, val, nullptr,
            /*enumerable=*/true, /*defineOwn=*/true);
        return self.get();
    }

    requirePlainObject(self.get(), "setProperty");
    // defineOwn: a definition, not an assignment — a host building an object
    // must never run an inherited setter, exactly as rtDefineMethods must not.
    ObjectHeader* live = self.get().asObject<ObjectHeader>()->setProp(
        runtime::rtHeap(), runtime::rtArena(), keyRoot, val, nullptr,
        /*enumerable=*/true, /*defineOwn=*/true);
    return Value::fromObject(live);
}

Value setElement(Value obj, uint32_t index, Value v) {
    // A plain object keeps the definition semantics setProperty has, for the
    // reason setProperty has them: a host building an object must not run an
    // inherited setter. On a plain object an integer key IS the canonical
    // numeric string — enumeration order and the element paths both key off
    // the spelling, and std::to_string of a uint32 is exactly canonical.
    if (obj.isObject() && obj.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        return setProperty(obj, std::to_string(index), v);
    }

    // Everything else — an Array, a typed array, a wrapper, a proxy — goes
    // through the generic element-set the compiled `arr[i] = v` takes. The
    // alternative was requirePlainObject's fatal, which is what this call used
    // to reach through setProperty: a host could READ an array element
    // (getElement has always been generic) and could not write one, so filling
    // an array meant handing the program a function to do it. Length
    // bookkeeping, hole semantics and a typed array's narrowing conversion all
    // live on this path; a second implementation here would be a second answer
    // to a question with one right one.
    ShadowStackFrame frame;
    Rooted<Value> self{obj};
    Rooted<Value> val{v};
    bronze_elem_set(self.get().rawBits(), Value::fromDouble(index).rawBits(),
                    val.get().rawBits(), /*strict=*/false);
    // The host boundary is where propagation ends — getProperty's rule, and
    // for the same reason: there is no enclosing JS frame to unwind into, and
    // a cell left set would make the next entry into compiled code appear to
    // throw this write's exception.
    if (bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS) {
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    }
    return self.get();
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
