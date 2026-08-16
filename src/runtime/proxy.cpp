#include "runtime/proxy.h"

#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"

namespace bronze {

ProxyHeader* ProxyHeader::create(Heap& heap, Rooted<Value>& target, Rooted<Value>& handler) {
    HeapObjectHeader* raw =
        heap.allocate(sizeof(ProxyHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* proxy = reinterpret_cast<ProxyHeader*>(raw);
    proxy->header.flags = kFlags;
    proxy->target = target.get();
    proxy->handler = handler.get();
    return proxy;
}

namespace runtime {

namespace {

// The construction gate: every own key of the handler must be a trap bronze
// has built, because forwarding-by-default is only correct for a handler
// that provably cannot intercept the forwarded operation.
void requireOnlyBuiltTraps(Value handler) {
    Rooted<Value> keysArr{Value(bronze_object_keys(handler.rawBits()))};
    auto* arr = keysArr.get().asObject<ArrayHeader>();
    const uint32_t len = arr->length;
    for (uint32_t i = 0; i < len; ++i) {
        Value key = keysArr.get().asObject<ArrayHeader>()->getElem(i);
        const std::string name =
            key.isString() ? rtUtf8Chars(key.asString<StringHeader>()) : std::string("<symbol>");
        if (name != "get" && name != "set" && name != "has") {
            fatal((std::string("unsupported: Proxy trap '") + name +
                   "' (bronze has built get, set and has; every other trap is refused here, at "
                   "construction, so that the operations it would have intercepted can forward "
                   "to the target)")
                      .c_str());
        }
    }
}

// The named trap off the handler, or undefined. The handler is a plain
// object (the constructor refused anything else), so this is an ordinary
// shape read — but through getProp all the same, because a handler written
// as `Object.create(base)` keeps its traps on `base`.
Value trapOf(Rooted<Value>& handlerRoot, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return handlerRoot.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

bool isArrayValue(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array;
}

// The proxy this native was handed as `this`, brand-checked. These natives
// are handed out ONLY by rtProxyGet's forward path below, so a different
// receiver is a runtime bug and not something a program wrote.
ProxyHeader* proxyOfArray(Value self, const char* method) {
    if (!self.isObject() || self.asObject<HeapObjectHeader>()->flags != ProxyHeader::kFlags ||
        !isArrayValue(self.asObject<ProxyHeader>()->target)) {
        fatal((std::string("internal: proxy-aware Array.prototype.") + method +
               " invoked on a receiver that is not a proxy of an array")
                  .c_str());
    }
    return self.asObject<ProxyHeader>();
}

// 23.1.3.23 Array.prototype.push, receiver-generic the way the spec writes
// it: every element lands through [[Set]] ON THE RECEIVER, which is what
// fires the proxy's `set` trap — the whole reason a proxy wraps an array in
// the first place (pixi's parser registry invalidates a cache from exactly
// that trap).
uint64_t proxyArrayPush(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    proxyOfArray(Value(thisBits), "push");
    RootedArgs args(argc, argv);
    Rooted<Value> proxyRoot{Value(thisBits)};
    for (uint32_t i = 0; i < argc; ++i) {
        const uint32_t len = proxyRoot.get()
                                 .asObject<ProxyHeader>()
                                 ->target.asObject<ArrayHeader>()
                                 ->length;
        Rooted<Value> key{rtMakeString(std::to_string(len))};
        rtProxySet(proxyRoot.get(), key.get(), args[i], /*strict=*/true);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    const uint32_t newLen =
        proxyRoot.get().asObject<ProxyHeader>()->target.asObject<ArrayHeader>()->length;
    // Step 5's Set(O, "length"), through the receiver like the elements: a
    // trap that watches `length` sees the write the spec says it sees.
    Rooted<Value> lenKey{rtMakeString("length")};
    Rooted<Value> lenVal{Value::fromDouble(newLen)};
    rtProxySet(proxyRoot.get(), lenKey.get(), lenVal.get(), /*strict=*/true);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return Value::fromDouble(newLen).rawBits();
}

// 23.1.3.30 Array.prototype.sort on a proxy receiver: read out, sort a real
// scratch array with the one real sort (comparator calls and all), write
// back through [[Set]] so the trap sees every store. SortIndexedProperties
// followed by per-index Set is the spec's own shape for an exotic receiver.
uint64_t proxyArraySort(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    proxyOfArray(Value(thisBits), "sort");
    RootedArgs args(argc, argv);
    Rooted<Value> proxyRoot{Value(thisBits)};
    const uint32_t len =
        proxyRoot.get().asObject<ProxyHeader>()->target.asObject<ArrayHeader>()->length;
    Rooted<Value> scratch{Value(bronze_create_array(len))};
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> elem{proxyRoot.get()
                               .asObject<ProxyHeader>()
                               ->target.asObject<ArrayHeader>()
                               ->getElem(i)};
        scratch.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, elem);
    }
    rtArraySortBuiltin(0, scratch.get().rawBits(), argc, argv);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 0; i < len; ++i) {
        Rooted<Value> key{rtMakeString(std::to_string(i))};
        Rooted<Value> elem{scratch.get().asObject<ArrayHeader>()->getElem(i)};
        rtProxySet(proxyRoot.get(), key.get(), elem.get(), /*strict=*/true);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    return proxyRoot.get().rawBits();
}

// The mutators bronze has not made receiver-generic, refused by name when
// reached through a proxy — running them against the target would skip the
// `set` trap, which is the silent wrong answer the vetting exists to prevent.
#define BRONZE_PROXY_REFUSED_MUTATOR(fn, name)                                                \
    uint64_t fn(uint64_t, uint64_t, uint32_t, const uint64_t*) {                              \
        fatal("unsupported: Array.prototype." name                                            \
              " on a Proxy (its writes would bypass the handler's `set` trap; push and sort " \
              "are built)");                                                                  \
    }
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayPop, "pop")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayShift, "shift")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayUnshift, "unshift")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArraySplice, "splice")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayReverse, "reverse")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayFill, "fill")
BRONZE_PROXY_REFUSED_MUTATOR(proxyArrayCopyWithin, "copyWithin")
#undef BRONZE_PROXY_REFUSED_MUTATOR

// What a forwarded read of an array MEMBER hands back, for a proxy whose
// target is an array. A builtin found through the member table demands a real
// ArrayHeader as `this`, and the caller is about to invoke it with the PROXY
// — so a reader is bound to the target (identical by construction: with no
// `get` trap every read forwards there anyway), a built mutator is swapped
// for its receiver-generic form above, and the rest are refused by name.
Value maybeAdaptArrayMember(Rooted<Value>& targetRoot, Rooted<Value>& keyRoot,
                            Value forwarded) {
    if (!isArrayValue(targetRoot.get())) return forwarded;
    if (!forwarded.isObject() ||
        forwarded.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return forwarded;
    }
    Rooted<Value> forwardedRoot{forwarded};
    if (keyRoot.get().isSymbol()) {
        // 23.1.3.41: `[Symbol.iterator]` IS `Array.prototype.values`, and
        // both intern on one code pointer — identity is the membership test.
        Rooted<Value> values{rtNativeFunction(rtArrayValuesBuiltin, 0)};
        if (values.get().rawBits() != forwardedRoot.get().rawBits()) {
            return forwardedRoot.get();
        }
        uint64_t thisArg = targetRoot.get().rawBits();
        return Value(rtFunctionBindBuiltin(0, forwardedRoot.get().rawBits(), 1, &thisArg));
    }
    if (!keyRoot.get().isString()) return forwardedRoot.get();
    const std::string keyStr = rtUtf8Chars(keyRoot.get().asString<StringHeader>());
    Rooted<Value> builtin{rtArrayMethod(keyStr)};
    if (builtin.get().isUndefined() ||
        builtin.get().rawBits() != forwardedRoot.get().rawBits()) {
        // An own function property of the target, or something inherited —
        // not the member table's. Handed back as read, which is what a
        // no-get-trap [[Get]] is.
        return forwardedRoot.get();
    }
    if (keyStr == "push") return rtNativeFunction(proxyArrayPush, 1);
    if (keyStr == "sort") return rtNativeFunction(proxyArraySort, 1);
    if (keyStr == "pop") return rtNativeFunction(proxyArrayPop, 0);
    if (keyStr == "shift") return rtNativeFunction(proxyArrayShift, 0);
    if (keyStr == "unshift") return rtNativeFunction(proxyArrayUnshift, 1);
    if (keyStr == "splice") return rtNativeFunction(proxyArraySplice, 2);
    if (keyStr == "reverse") return rtNativeFunction(proxyArrayReverse, 0);
    if (keyStr == "fill") return rtNativeFunction(proxyArrayFill, 1);
    if (keyStr == "copyWithin") return rtNativeFunction(proxyArrayCopyWithin, 2);
    uint64_t thisArg = targetRoot.get().rawBits();
    return Value(rtFunctionBindBuiltin(0, forwardedRoot.get().rawBits(), 1, &thisArg));
}

}  // namespace

uint64_t rtProxyConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) {
        return rtThrowTypeError("Cannot create proxy with a non-object as target or handler")
            .rawBits();
    }
    Value target(argv[0]);
    Value handler(argv[1]);
    // 28.2.1.1 step 1: both operands must be objects, and that is the spec's
    // TypeError rather than a bronze refusal.
    if (!target.isObject() || !handler.isObject()) {
        return rtThrowTypeError("Cannot create proxy with a non-object as target or handler")
            .rawBits();
    }
    // Bronze's own gate, by name. A callable target makes the PROXY callable
    // (10.5.12), and the call path has no proxy arm; a handler that is not a
    // plain object cannot have its own keys walked by the check below.
    if (target.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        fatal("unsupported: Proxy over a callable target (calling through a Proxy is not "
              "built)");
    }
    if (handler.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        fatal("unsupported: a Proxy handler that is not a plain object");
    }
    // Rooted BEFORE the trap walk: enumerating the handler's keys allocates
    // the keys array, and both operands have to survive it.
    Rooted<Value> targetRoot{target};
    Rooted<Value> handlerRoot{handler};
    requireOnlyBuiltTraps(handlerRoot.get());

    ProxyHeader* proxy = ProxyHeader::create(rtHeap(), targetRoot, handlerRoot);
    return Value::fromObject(reinterpret_cast<HeapObjectHeader*>(proxy)).rawBits();
}

Value rtProxyGet(Value proxyVal, Value keyVal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> handlerRoot{proxyVal.asObject<ProxyHeader>()->handler};
    Rooted<Value> targetRoot{proxyVal.asObject<ProxyHeader>()->target};

    Value trap = trapOf(handlerRoot, "get");
    if (trap.isUndefined()) {
        // 10.5.8 step 6: no trap means the target's own [[Get]], receiver and
        // all. The receiver nuance is dropped deliberately: the target answers
        // as itself, which only a getter that inspects `this` can observe.
        Value forwarded =
            Value(bronze_elem_get(targetRoot.get().rawBits(), keyRoot.get().rawBits()));
        if (rtExceptionPending()) return forwarded;
        return maybeAdaptArrayMember(targetRoot, keyRoot, forwarded);
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[3] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                              proxyRoot.get().rawBits()};
    return Value(bronze_dynamic_call(trapRoot.get().rawBits(), handlerRoot.get().rawBits(), 3,
                                     args));
}

void rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> valRoot{val};
    Rooted<Value> handlerRoot{proxyVal.asObject<ProxyHeader>()->handler};
    Rooted<Value> targetRoot{proxyVal.asObject<ProxyHeader>()->target};

    Value trap = trapOf(handlerRoot, "set");
    if (trap.isUndefined()) {
        bronze_elem_set(targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                        valRoot.get().rawBits(), strict);
        return;
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[4] = {targetRoot.get().rawBits(), keyRoot.get().rawBits(),
                              valRoot.get().rawBits(), proxyRoot.get().rawBits()};
    const uint64_t result = bronze_dynamic_call(trapRoot.get().rawBits(),
                                                handlerRoot.get().rawBits(), 4, args);
    if (rtExceptionPending()) return;
    // 13.15.2 via 10.5.9 step 6: a trap that answers false refused the write,
    // and strict code turns that refusal into a TypeError.
    if (strict && !bronze_truthy(result)) {
        rtThrowTypeError("'set' on proxy: trap returned falsish");
    }
}

bool rtProxyHas(Value proxyVal, Value keyVal) {
    Rooted<Value> proxyRoot{proxyVal};
    Rooted<Value> keyRoot{keyVal};
    Rooted<Value> handlerRoot{proxyVal.asObject<ProxyHeader>()->handler};
    Rooted<Value> targetRoot{proxyVal.asObject<ProxyHeader>()->target};

    Value trap = trapOf(handlerRoot, "has");
    if (trap.isUndefined()) {
        return bronze_has_property(keyRoot.get().rawBits(), targetRoot.get().rawBits());
    }
    Rooted<Value> trapRoot{trap};
    const uint64_t args[2] = {targetRoot.get().rawBits(), keyRoot.get().rawBits()};
    return bronze_truthy(bronze_dynamic_call(trapRoot.get().rawBits(),
                                             handlerRoot.get().rawBits(), 2, args));
}

}  // namespace runtime
}  // namespace bronze
