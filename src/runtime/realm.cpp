#include "runtime/realm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
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
#include "runtime/stack_locals.h"
#include "runtime/string.h"
#include "runtime/module_instance.h"
#include "runtime/tls_block.h"

namespace bronze {

static thread_local std::vector<Realm*> g_realmStack;
static thread_local std::vector<Realm*> g_liveRealms;
static thread_local Realm* g_defaultRealm = nullptr;
// The realm whose module instances activateTopRealm last made active.
static thread_local const Realm* g_activeRealm = nullptr;

// Realm slots on this thread: index = slot, the realm while it lives, null
// once destroyed (a slot is never handed out twice, so a function or handle
// of a destroyed realm can never be taken for a later realm's). Slot 0 is
// "no realm".
static thread_local std::vector<Realm*> g_slotRealms;

static void assignSlot(Realm* r) {
    if (g_slotRealms.empty()) g_slotRealms.push_back(nullptr);
    if (g_slotRealms.size() > kMaxRealmSlot) {
        fatal("realm: more realms were made on this thread than a slot can name");
    }
    r->setSlot(static_cast<uint32_t>(g_slotRealms.size()));
    g_slotRealms.push_back(r);
}

// The TLS block's copy of rtCurrentRealmSlot (bronze_abi_tls.h, realm_slot),
// which a call from compiled code compares a callee's realm against before it
// enters the callee's code itself. Written wherever the answer can change: the
// realm stack's top (activateTopRealm, which every push, pop, destroy and
// stack switch that moves the top goes through), the default realm's creation,
// and the thread's teardown. A stale copy that matched a callee's slot would
// run that callee outside its realm, which is why there is no other writer.
static uint32_t currentRealmSlotOf() noexcept {
    if (!g_realmStack.empty()) return g_realmStack.back()->slot();
    return g_defaultRealm ? g_defaultRealm->slot() : 0;
}

static void syncTlsRealmSlot() noexcept { runtime::rtTls()->realm_slot = currentRealmSlotOf(); }

// Generated code addresses a module's writable tables through the TLS
// block's delta array, which is the CURRENT realm's: each realm has its own
// instance of every module entered in it (module_instance.cpp). The default
// realm keeps the thread's own table (realm slot 0 there).
static void activateTopRealm() {
    Realm* top = g_realmStack.empty() ? g_defaultRealm : g_realmStack.back();
    g_activeRealm = top;
    runtime::rtActivateRealmModuleInstances(top && top != g_defaultRealm ? top->slot() : 0);
    syncTlsRealmSlot();
}

// The realm stack is per STACK (runtime/stack_locals.h): a compartment's fiber
// enters its own realm, and the stack it switches to finds its own realm on
// top again, with that realm's module instances active and the global caches
// (which name one realm's globals) dropped.
static const int g_realmStackLocal = [] {  // NOLINT: registered for its effect
    brass::gc::StackLocal local;
    local.save = [](void** slot) {
        auto* box = static_cast<std::vector<Realm*>*>(*slot);
        if (box == nullptr) {
            box = new std::vector<Realm*>();
            *slot = box;
        }
        box->swap(g_realmStack);
    };
    local.restore = [](void** slot, bool fresh) {
        auto* box = static_cast<std::vector<Realm*>*>(*slot);
        if (fresh || box == nullptr) {
            g_realmStack.clear();
        } else {
            box->swap(g_realmStack);
        }
        // Only a change of the active realm costs anything: a switch between
        // stacks running the same realm (or none) keeps its caches.
        const Realm* top = g_realmStack.empty() ? g_defaultRealm : g_realmStack.back();
        if (top != g_activeRealm) {
            activateTopRealm();
            runtime::rtInvalidateGlobalCaches();
        }
        syncTlsRealmSlot();
    };
    local.release = [](void* value) { delete static_cast<std::vector<Realm*>*>(value); };
    return brass::gc::register_stack_local(local);
}();

uint32_t rtCurrentRealmSlot() noexcept { return currentRealmSlotOf(); }

CalleeRealmScope::CalleeRealmScope(uint32_t slot) {
    if (slot == 0 || slot == rtCurrentRealmSlot()) return;
    Realm* r = rtRealmForSlot(slot);
    if (!r) {
        dead_ = rtRealmSlotDestroyed(slot);
        return;
    }
    rtEnterRealm(r);
    entered_ = true;
}

CalleeRealmScope::~CalleeRealmScope() {
    if (entered_) rtExitRealm();
}

bool rtRealmSlotInCurrentGroup(uint32_t slot) noexcept {
    if (slot == 0) return true;
    const Realm* current = !g_realmStack.empty() ? g_realmStack.back() : g_defaultRealm;
    if (!current) return false;
    if (slot == current->slot()) return true;
    const Realm* owner = rtRealmForSlot(slot);
    return owner != nullptr && owner->group() == current->group();
}

Realm* rtRealmForSlot(uint32_t slot) noexcept {
    if (slot == 0 || slot >= g_slotRealms.size()) return nullptr;
    return g_slotRealms[slot];
}

bool rtRealmSlotDestroyed(uint32_t slot) noexcept {
    return slot != 0 && slot < g_slotRealms.size() && g_slotRealms[slot] == nullptr;
}

static const char* const kGlobalObjectNames[] = {
    "Math", "Object", "Number", "JSON", "Array", "String", "Boolean", "Symbol", "BigInt", "RegExp",
    "Promise", "Map", "Set", "WeakMap", "WeakSet", "Error", "TypeError", "AggregateError",
    "RangeError", "SyntaxError", "ReferenceError", "URIError", "isNaN", "isFinite", "parseInt",
    "parseFloat", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
    "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "DataView",
    "Float16Array", "BigInt64Array", "BigUint64Array", "SharedArrayBuffer", "Atomics",
    "Function", "Proxy", "Reflect", "Date", "Intl", "encodeURI", "encodeURIComponent", "decodeURI",
    "decodeURIComponent", "escape", "unescape", "Iterator", "WeakRef", "FinalizationRegistry",
    "eval", "performance",
};

std::span<const char* const> rtBuiltinGlobalNames() { return kGlobalObjectNames; }

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

    for (const auto& entry : hostGlobals_) {
        // Skipped for the same reason `rtDefineHostGlobalOnLiveRealms` skips
        // it: the bare-name ladder asks the builtins first, so a host global
        // named `Math` is unreachable by name — and writing it here made the
        // property disagree with the name on every realm built after the
        // registration, which is one object called `Math` in a program and
        // another one in the realm beside it.
        if (!runtime::rtHostShadowsBuiltin(entry.first)) {
            if (Value builtin = Value::fromUndefined();
                runtime::rtResolveBuiltinGlobal(entry.first, builtin)) {
                continue;
            }
        }
        if (entry.second.isHole()) {
            // A pending lazy name: its accessor pair, not a value.
            if (const runtime::LazyGlobalAccessorHook hook = runtime::rtLazyGlobalAccessorHook()) {
                globRoot.set(hook(globRoot.get(), entry.first));
            }
            continue;
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
    for (auto& entry : hostGlobals_) visit(entry.second);
}

Realm* rtDefaultRealm() {
    if (!g_defaultRealm) {
        runtime::rtArmThreadTeardown();
        auto* r = new Realm(/*deferInit=*/true);
        g_defaultRealm = r;
        g_liveRealms.push_back(r);
        assignSlot(r);
        syncTlsRealmSlot();
        runtime::rtAdoptPreRealmHostGlobals(*r);
        r->initGlobalObject(Value::fromUndefined());
    }
    return g_defaultRealm;
}

Realm* rtHostGlobalRealm() {
    if (!g_realmStack.empty()) return g_realmStack.back();
    return g_defaultRealm;
}

Realm* rtRealmOfGlobal(Value glob) {
    if (!glob.isObject()) return nullptr;
    for (Realm* r : g_liveRealms) {
        if (r && r->globalObject().rawBits() == glob.rawBits()) return r;
    }
    return nullptr;
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
    assignSlot(realm);
    realm->initGlobalObject(rootCustom.get());
    return realm;
}

static std::mutex g_realmDestroyHooksMu;

static std::vector<RealmDestroyHook>& realmDestroyHooks() {
    static auto* hooks = new std::vector<RealmDestroyHook>();
    return *hooks;
}

void rtAddRealmDestroyHook(RealmDestroyHook hook) {
    if (!hook) return;
    std::lock_guard<std::mutex> lock(g_realmDestroyHooksMu);
    auto& hooks = realmDestroyHooks();
    if (std::find(hooks.begin(), hooks.end(), hook) == hooks.end()) hooks.push_back(hook);
}

void rtDestroyRealm(Realm* realm) {
    if (!realm || realm == g_defaultRealm) return;
    std::vector<RealmDestroyHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_realmDestroyHooksMu);
        hooks = realmDestroyHooks();
    }
    for (RealmDestroyHook hook : hooks) hook(realm);
    bool wasActive = (!g_realmStack.empty() && g_realmStack.back() == realm);
    std::erase(g_realmStack, realm);
    // A suspended stack (a compartment's fiber) may have it entered too.
    runtime::forEachSwitchedStackLocal(g_realmStackLocal, [realm](void* box) {
        if (box) std::erase(*static_cast<std::vector<Realm*>*>(box), realm);
    });
    if (g_activeRealm == realm) g_activeRealm = nullptr;
    std::erase(g_liveRealms, realm);
    const uint32_t slot = realm->slot();
    if (slot != 0 && slot < g_slotRealms.size()) g_slotRealms[slot] = nullptr;
    if (wasActive) activateTopRealm();
    // Its module instances (environments, caches) and its function
    // singletons go with it; nothing of it can run again, since a call into
    // one of its functions is refused (embed::call).
    runtime::rtReleaseRealmModuleInstances(slot);
    delete realm;
    if (wasActive) {
        runtime::rtInvalidateGlobalCaches();
    }
}

void rtEnterRealm(Realm* realm) {
    if (!realm) return;
    g_realmStack.push_back(realm);
    activateTopRealm();
    runtime::rtInvalidateGlobalCaches();
}

void rtExitRealm() {
    if (!g_realmStack.empty()) {
        g_realmStack.pop_back();
        activateTopRealm();
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

void rtDefineHostGlobalOnRealm(Realm* r, const std::string& name, Value value) {
    if (!r) return;
    if (name == "globalThis") return;
    // The bare-name ladder's order, mirrored exactly (rt_state.cpp
    // `bronze_global_get`): the builtins a host may shadow
    // (rtHostShadowsBuiltin: `performance`, `Intl`) take the host's value, and
    // every other builtin wins over the registry — so writing one here would
    // make `globalThis.Math` and bare `Math` two different objects.
    if (!runtime::rtHostShadowsBuiltin(name)) {
        if (Value builtin = Value::fromUndefined();
            runtime::rtResolveBuiltinGlobal(name, builtin)) {
            return;
        }
    }
    ShadowStackFrame frame;
    Rooted<Value> val{value};
    Rooted<Value> key{runtime::rtMakeString(name)};
    {
        Rooted<Value> glob{r->globalObject()};
        if (!glob.get().isObject()) return;
        // A lazy name's accessor (host_globals.h) gives way to the value:
        // setProp will not turn an accessor into a data property.
        {
            const PropertyKey pkey = runtime::rtInternPropertyKey(key.get());
            auto* obj = glob.get().asObject<ObjectHeader>();
            PropertyInfo own;
            if (obj->shape && obj->shape->lookupProperty(pkey, own) && own.accessor) {
                bronze_elem_delete(glob.get().rawBits(), key.get().rawBits(), /*strict=*/false);
            }
        }
        glob.get().asObject<ObjectHeader>()->setProp(runtime::rtHeap(), runtime::rtArena(), key,
                                                     val, nullptr,
                                                     /*enumerable=*/false, /*defineOwn=*/true);
        // setProp can transition the shape and the write allocates, so the
        // realm's handle is refreshed from the rooted copy rather than left
        // pointing at whatever the collector moved out from under it.
        r->setGlobalObject(glob.get());
    }
}

void rtDefineLazyHostGlobalOnRealm(Realm* r, const std::string& name) {
    const runtime::LazyGlobalAccessorHook hook = runtime::rtLazyGlobalAccessorHook();
    if (!hook || !r || name == "globalThis") return;
    if (!runtime::rtHostShadowsBuiltin(name)) {
        if (Value builtin = Value::fromUndefined();
            runtime::rtResolveBuiltinGlobal(name, builtin)) {
            return;
        }
    }
    ShadowStackFrame frame;
    Rooted<Value> glob{r->globalObject()};
    if (!glob.get().isObject()) return;
    glob.set(hook(glob.get(), name));
    r->setGlobalObject(glob.get());
}

void rtTeardownThreadRealms() {
    g_realmStack.clear();
    g_realmStack.shrink_to_fit();
    g_defaultRealm = nullptr;
    g_activeRealm = nullptr;
    syncTlsRealmSlot();
    g_slotRealms.clear();
    g_slotRealms.shrink_to_fit();
    for (Realm* r : g_liveRealms) {
        delete r;
    }
    g_liveRealms.clear();
    g_liveRealms.shrink_to_fit();
}

}  // namespace bronze
