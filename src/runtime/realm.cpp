#include "runtime/realm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/host_globals.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze {

static thread_local std::vector<Realm*> g_realmStack;
static thread_local std::vector<Realm*> g_liveRealms;
static thread_local Realm* g_defaultRealm = nullptr;

static const char* const kGlobalObjectNames[] = {
    "Math", "Object", "Number", "JSON", "Array", "String", "Boolean", "Symbol", "BigInt", "RegExp",
    "Promise", "Map", "Set", "WeakMap", "WeakSet", "Error", "TypeError", "AggregateError",
    "RangeError", "SyntaxError", "ReferenceError", "URIError", "isNaN", "isFinite", "parseInt",
    "parseFloat", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
    "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "DataView",
    "Float16Array", "BigInt64Array", "BigUint64Array", "SharedArrayBuffer", "Atomics",
    "Function", "Proxy", "Reflect", "Date", "encodeURI", "encodeURIComponent", "decodeURI",
    "decodeURIComponent", "escape", "unescape", "Iterator", "WeakRef", "FinalizationRegistry",
    "eval", "performance",
};

Realm::Realm(bool deferInit) {
    if (!deferInit) {
        initGlobalObject(Value::fromUndefined());
    }
}

Realm::Realm(Value customGlobal) {
    initGlobalObject(customGlobal);
}

void Realm::initGlobalObject(Value customGlobal) {
    ShadowStackFrame frame;
    Rooted<Value> rootCustom{customGlobal};
    if (rootCustom.get().isObject()) {
        globalObject_ = rootCustom.get();
    } else {
        Rooted<Value> glob{Value::fromObject(
            ObjectHeader::create(runtime::rtHeap(), runtime::rtArena(),
                                 runtime::rtRootShapeForPrototype(Value::fromNull())))};
        glob.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        globalObject_ = glob.get();
    }

    Rooted<Value> globRoot{globalObject_};

    for (const char* name : kGlobalObjectNames) {
        Value resolved = Value::fromUndefined();
        if (!runtime::rtResolveBuiltinGlobal(name, resolved)) {
            fatal((std::string("internal: global object population lists '") + name +
                            "', a name the builtin ladder cannot resolve")
                               .c_str());
        }
        Rooted<Value> val{resolved};
        Rooted<Value> key{runtime::rtMakeString(name)};
        globRoot.get().asObject<ObjectHeader>()->setProp(
            runtime::rtHeap(), runtime::rtArena(), key, val, nullptr,
            /*enumerable=*/false, /*defineOwn=*/true);
    }

    for (const auto& entry : runtime::rtHostGlobalEntries()) {
        // Skipped for the same reason `rtDefineHostGlobalOnLiveRealms` skips
        // it: the bare-name ladder asks the builtins first, so a host global
        // named `Math` is unreachable by name — and writing it here made the
        // property disagree with the name on every realm built after the
        // registration, which is one object called `Math` in a program and
        // another one in the realm beside it.
        if (entry.first != "performance") {
            if (Value builtin = Value::fromUndefined();
                runtime::rtResolveBuiltinGlobal(entry.first, builtin)) {
                continue;
            }
        }
        Rooted<Value> key{runtime::rtMakeString(entry.first)};
        Rooted<Value> val{entry.second};
        globRoot.get().asObject<ObjectHeader>()->setProp(
            runtime::rtHeap(), runtime::rtArena(), key, val, nullptr,
            /*enumerable=*/false, /*defineOwn=*/true);
    }

    {
        const std::pair<const char*, Value> values[] = {
            {"Infinity", Value::fromDouble(std::numeric_limits<double>::infinity())},
            {"NaN", Value::fromDouble(std::numeric_limits<double>::quiet_NaN())},
            {"undefined", Value::fromUndefined()},
        };
        for (const auto& entry : values) {
            Rooted<Value> val{entry.second};
            Rooted<Value> key{runtime::rtMakeString(entry.first)};
            globRoot.get().asObject<ObjectHeader>()->setProp(
                runtime::rtHeap(), runtime::rtArena(), key, val, nullptr,
                /*enumerable=*/false, /*defineOwn=*/true, /*receiver=*/nullptr,
                /*refused=*/nullptr, /*writable=*/false, /*configurable=*/false);
        }
    }

    Rooted<Value> key{runtime::rtMakeString("globalThis")};
    globRoot.get().asObject<ObjectHeader>()->setProp(
        runtime::rtHeap(), runtime::rtArena(), key, globRoot, nullptr,
        /*enumerable=*/false, /*defineOwn=*/true);

    globalObject_ = globRoot.get();
}

void Realm::visitRoots(const Heap::RootVisitor& visit) {
    if (globalObject_.isObject()) {
        visit(globalObject_);
    }
    if (globalEnv_.isObject()) {
        visit(globalEnv_);
    }
    // The published module namespaces. A root SOURCE and not a fixed slot:
    // the namespace objects live in the moving heap, and a later unit reads
    // them through the registry long after the unit that published them ran.
    for (auto& entry : moduleRegistry_) {
        if (entry.second.isObject()) visit(entry.second);
    }
}

Realm* rtDefaultRealm() {
    if (!g_defaultRealm) {
        auto* r = new Realm(/*deferInit=*/true);
        g_defaultRealm = r;
        g_liveRealms.push_back(r);
        r->initGlobalObject(Value::fromUndefined());
    }
    return g_defaultRealm;
}

Realm* rtCurrentRealm() {
    if (!g_realmStack.empty()) {
        return g_realmStack.back();
    }
    return rtDefaultRealm();
}

Realm* rtCreateRealm(Value customGlobal) {
    ShadowStackFrame frame;
    Rooted<Value> rootCustom{customGlobal};
    (void)rtDefaultRealm();
    auto* realm = new Realm(/*deferInit=*/true);
    g_liveRealms.push_back(realm);
    realm->initGlobalObject(rootCustom.get());
    return realm;
}

void rtDestroyRealm(Realm* realm) {
    if (!realm || realm == g_defaultRealm) return;
    bool wasActive = (!g_realmStack.empty() && g_realmStack.back() == realm);
    std::erase(g_realmStack, realm);
    std::erase(g_liveRealms, realm);
    delete realm;
    if (wasActive) {
        runtime::rtInvalidateGlobalCaches();
    }
}

void rtEnterRealm(Realm* realm) {
    if (!realm) return;
    g_realmStack.push_back(realm);
    runtime::rtInvalidateGlobalCaches();
}

void rtExitRealm() {
    if (!g_realmStack.empty()) {
        g_realmStack.pop_back();
        runtime::rtInvalidateGlobalCaches();
    }
}

void rtVisitRealmRoots(const Heap::RootVisitor& visit) {
    for (Realm* r : g_liveRealms) {
        if (r) {
            r->visitRoots(visit);
        }
    }
}

void rtDefineHostGlobalOnLiveRealms(const std::string& name, Value value) {
    if (g_liveRealms.empty()) return;
    if (name == "globalThis") return;
    // The bare-name ladder's order, mirrored exactly (rt_state.cpp
    // `bronze_global_get`): `performance` is the one builtin a host may shadow,
    // and every other builtin wins over the registry — so writing one here
    // would make `globalThis.Math` and bare `Math` two different objects.
    if (name != "performance") {
        if (Value builtin = Value::fromUndefined();
            runtime::rtResolveBuiltinGlobal(name, builtin)) {
            return;
        }
    }
    ShadowStackFrame frame;
    Rooted<Value> val{value};
    Rooted<Value> key{runtime::rtMakeString(name)};
    for (Realm* r : g_liveRealms) {
        if (!r) continue;
        Rooted<Value> glob{r->globalObject()};
        if (!glob.get().isObject()) continue;
        glob.get().asObject<ObjectHeader>()->setProp(runtime::rtHeap(), runtime::rtArena(), key,
                                                     val, nullptr,
                                                     /*enumerable=*/false, /*defineOwn=*/true);
        // setProp can transition the shape and the write allocates, so the
        // realm's handle is refreshed from the rooted copy rather than left
        // pointing at whatever the collector moved out from under it.
        r->setGlobalObject(glob.get());
    }
}

}  // namespace bronze
