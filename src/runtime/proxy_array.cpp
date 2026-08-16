// The receiver-generic Array.prototype methods a proxy over an ARRAY needs,
// and the refusals for the ones bronze has not made receiver-generic.
//
// A builtin found through the array member table demands a real ArrayHeader as
// `this`, and a read through a proxy is about to hand it the PROXY. So a
// reader is bound to the target — identical by construction, since with no
// `get` trap the read forwards there anyway — a built MUTATOR is swapped for
// the form below that writes through [[Set]] on the receiver, so the `set`
// trap sees every store, and the rest are refused by name. That last part is
// the whole reason this file exists: running an unadapted mutator against the
// target would skip the trap, which is the silent wrong answer.

#include <cstring>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

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

}  // namespace

Value rtProxyAdaptArrayMember(Rooted<Value>& targetRoot, Rooted<Value>& keyRoot,
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


}  // namespace bronze::runtime
