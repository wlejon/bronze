// The host-global registry (host_globals.h) and the host's dynamic-code seams
// (CreateDynamicFunction, eval, import()): per-thread runtime state with no
// heap Values of its own beyond the pre-realm registrations, which
// rt_state.cpp's root source visits through rtVisitPreRealmHostGlobals.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "runtime/fatal.h"
#include "runtime/host_globals.h"
#include "runtime/native_registry.h"
#include "runtime/realm.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Globals an embedding host registered (host_globals.h) live on the REALM they
// were registered in (Realm::hostGlobals). A vector of pairs rather than a map:
// registration happens a handful of times at host startup, and a lookup is a
// read `bronze_global_get` reaches only for a name the builtin ladder did not
// answer. These two hold what a host registered before the thread had any
// realm, which the default realm adopts when it is made.
static thread_local std::vector<std::pair<std::string, Value>> g_preRealmHostGlobals;
static thread_local std::vector<std::pair<std::string, std::function<void()>>>
    g_preRealmLazyHostGlobals;

namespace {

// The tables a registration or a lookup on this thread reads: the current
// realm's, or the thread's own before any realm exists.
struct HostGlobalTables {
    std::vector<std::pair<std::string, Value>>& values;
    std::vector<std::pair<std::string, std::function<void()>>>& lazy;
    Realm* realm;
};

HostGlobalTables hostGlobalTables() {
    if (Realm* r = bronze::rtHostGlobalRealm()) {
        return {r->hostGlobals(), r->lazyHostGlobals(), r};
    }
    return {g_preRealmHostGlobals, g_preRealmLazyHostGlobals, nullptr};
}

}  // namespace

void rtAdoptPreRealmHostGlobals(Realm& realm) {
    for (auto& entry : g_preRealmHostGlobals) realm.hostGlobals().push_back(std::move(entry));
    for (auto& entry : g_preRealmLazyHostGlobals) {
        realm.lazyHostGlobals().push_back(std::move(entry));
    }
    g_preRealmHostGlobals.clear();
    g_preRealmLazyHostGlobals.clear();
}

void rtVisitPreRealmHostGlobals(const Heap::RootVisitor& visit) {
    for (auto& entry : g_preRealmHostGlobals) visit(entry.second);
}

void rtRegisterHostGlobal(const std::string& name, Value value) {
    // A name is a native or a host global, never both: `f(1)` compiled as a
    // direct native call while `f` read as a host value would be two
    // different things under one name. Refused loudly here and in
    // rtRegisterNative, whichever comes second.
    if (rtNativeBareNameRegistered(name)) {
        fatal(("registerGlobal(\"" + name +
               "\"): the name is a registered native (embed::registerNative); a name is a "
               "native or a host global, not both")
                  .c_str());
    }
    // Before the write, unconditionally: a REPLACED name has cells holding
    // the old value, and a NEW name can have cells too — `performance` is a
    // builtin until a host provides its own, and every module that read the
    // builtin cached it.
    rtInvalidateGlobalCaches();
    HostGlobalTables t = hostGlobalTables();
    // A pending lazy name registered for real: its init has nothing left to
    // do for it (host_globals.h).
    for (auto it = t.lazy.begin(); it != t.lazy.end(); ++it) {
        if (it->first == name) {
            t.lazy.erase(it);
            break;
        }
    }
    for (auto& entry : t.values) {
        if (entry.first == name) {
            entry.second = value;
            rtDefineHostGlobalOnRealm(t.realm, name, value);
            return;
        }
    }
    t.values.emplace_back(name, value);
    // The registry answers the BARE name; a realm's global object is what
    // `globalThis[name]`, `name in globalThis` and every reflective walk read.
    // Both, or a host global is invisible to feature detection (realm.h states
    // the rule and why a builtin name is not written).
    rtDefineHostGlobalOnRealm(t.realm, name, value);
}

bool rtHostGlobalLookup(const std::string& name, Value& out) {
    HostGlobalTables t = hostGlobalTables();
    for (const auto& entry : t.values) {
        if (entry.first == name) {
            if (entry.second.isHole()) {
                rtForceLazyHostGlobal(name);
                // The init may have registered names, growing the vector:
                // read the entry again.
                for (const auto& again : t.values) {
                    if (again.first == name) {
                        out = again.second.isHole() ? Value::fromUndefined() : again.second;
                        return true;
                    }
                }
                out = Value::fromUndefined();
                return true;
            }
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool rtHostGlobalRegistered(const std::string& name) {
    for (const auto& entry : hostGlobalTables().values) {
        if (entry.first == name) return true;
    }
    return false;
}

void rtRegisterLazyHostGlobal(const std::string& name, std::function<void()> init) {
    if (rtNativeBareNameRegistered(name)) {
        fatal(("registerLazyGlobal(\"" + name +
               "\"): the name is a registered native (embed::registerNative); a name is a "
               "native or a host global, not both")
                  .c_str());
    }
    HostGlobalTables t = hostGlobalTables();
    // Already registered for real: there is nothing to defer.
    for (const auto& entry : t.values) {
        if (entry.first == name && !entry.second.isHole()) return;
    }
    bool replaced = false;
    for (auto& pending : t.lazy) {
        if (pending.first == name) {
            pending.second = std::move(init);
            replaced = true;
            break;
        }
    }
    if (!replaced) t.lazy.emplace_back(name, std::move(init));
    if (!rtHostGlobalRegistered(name)) {
        rtInvalidateGlobalCaches();
        t.values.emplace_back(name, Value::fromHole());
        rtDefineLazyHostGlobalOnRealm(t.realm, name);
    }
}

static thread_local uint32_t g_lazyInitDepth = 0;

bool rtInLazyGlobalInit() { return g_lazyInitDepth != 0; }

void rtForceLazyHostGlobal(const std::string& name) {
    std::function<void()> init;
    auto& lazy = hostGlobalTables().lazy;
    for (auto it = lazy.begin(); it != lazy.end(); ++it) {
        if (it->first == name) {
            init = std::move(it->second);
            lazy.erase(it);
            break;
        }
    }
    if (!init) return;
    // The name's accessor goes first, so that the init's module may assign
    // the global as a plain data property; registered `undefined` until the
    // init registers what it made.
    rtRegisterHostGlobal(name, Value::fromUndefined());
    rtRunAsLazyInit(init);
}

void rtRunAsLazyInit(const std::function<void()>& init) {
    ++g_lazyInitDepth;
    struct Leave {
        ~Leave() { --g_lazyInitDepth; }
    } leave;
    init();
}

static LazyGlobalAccessorHook g_lazyGlobalAccessorHook = nullptr;

void rtSetLazyGlobalAccessorHook(LazyGlobalAccessorHook hook) { g_lazyGlobalAccessorHook = hook; }

LazyGlobalAccessorHook rtLazyGlobalAccessorHook() { return g_lazyGlobalAccessorHook; }

// The host's CreateDynamicFunction, if it has one (host_globals.h says why the
// runtime offers the seam at all). Not a root source: whatever the host closed
// over is the host's to keep alive, and the Values it hands back per call are
// rooted by the caller like any other builtin result.
static thread_local DynamicFunctionHost g_dynamicFunctionHost;
static DynamicFunctionHost g_defaultDynamicFunctionHost;

void rtSetDynamicFunctionHost(DynamicFunctionHost host) {
    g_dynamicFunctionHost = std::move(host);
}

void rtSetDefaultDynamicFunctionHost(DynamicFunctionHost host) {
    g_defaultDynamicFunctionHost = std::move(host);
}

const DynamicFunctionHost& rtDynamicFunctionHost() {
    if (g_dynamicFunctionHost) return g_dynamicFunctionHost;
    return g_defaultDynamicFunctionHost;
}

// The host's eval, if it has one — the same seam, for `Function`'s sibling.
static thread_local DynamicEvalHost g_dynamicEvalHost;
static DynamicEvalHost g_defaultDynamicEvalHost;

void rtSetDynamicEvalHost(DynamicEvalHost host) { g_dynamicEvalHost = std::move(host); }

void rtSetDefaultDynamicEvalHost(DynamicEvalHost host) {
    g_defaultDynamicEvalHost = std::move(host);
}

const DynamicEvalHost& rtDynamicEvalHost() {
    if (g_dynamicEvalHost) return g_dynamicEvalHost;
    return g_defaultDynamicEvalHost;
}

// The host's `import()`, if it has one — the seam for the specifier the module
// graph could not read at compile time.
static thread_local DynamicImportHost g_dynamicImportHost;

void rtSetDynamicImportHost(DynamicImportHost host) { g_dynamicImportHost = std::move(host); }

const DynamicImportHost& rtDynamicImportHost() { return g_dynamicImportHost; }

const std::vector<std::pair<std::string, Value>>& rtHostGlobalEntries() {
    return hostGlobalTables().values;
}

}  // namespace bronze::runtime
