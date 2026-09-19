#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

class Realm {
public:
    explicit Realm(bool deferInit = false);
    explicit Realm(Value customGlobal);
    ~Realm() = default;

    Realm(const Realm&) = delete;
    Realm& operator=(const Realm&) = delete;

    // The realm's globalThis object.
    Value globalObject() const noexcept { return globalObject_; }
    void setGlobalObject(Value glob) noexcept { globalObject_ = glob; }

    // Optional environment cell associated with this realm.
    Value globalEnv() const noexcept { return globalEnv_; }
    void setGlobalEnv(Value env) noexcept { globalEnv_ = env; }

    // The module instances this realm holds: canonical file path -> the
    // module namespace object a compilation unit left behind for it. See
    // runtime/module_registry.h for why a realm is the right owner and what a
    // later compilation unit does with the list. A vector of pairs rather than
    // a map for the reason the host-global registry is one: it is written once
    // per module per unit and read once per specifier at compile time.
    std::vector<std::pair<std::string, Value>>& moduleRegistry() noexcept {
        return moduleRegistry_;
    }

    // GC root tracing for this realm's held heap values.
    void visitRoots(const Heap::RootVisitor& visit);

    // Populate the global object with builtins, host globals, and globalThis reference.
    void initGlobalObject(Value customGlobal = Value::fromUndefined());

private:
    Value globalObject_{Value::fromUndefined()};
    Value globalEnv_{Value::fromUndefined()};
    std::vector<std::pair<std::string, Value>> moduleRegistry_;
};

// Thread-local active Realm accessors and stack operations.
Realm* rtCurrentRealm();
Realm* rtDefaultRealm();
Realm* rtCreateRealm(Value customGlobal = Value::fromUndefined());
void rtDestroyRealm(Realm* realm);
void rtEnterRealm(Realm* realm);
void rtExitRealm();

// Trace all live realms on the calling thread during GC.
void rtVisitRealmRoots(const Heap::RootVisitor& visit);

// Define a host-registered global as an own property of every realm's global
// object on this thread — the realms that ALREADY exist, because one created
// later copies the whole registry in `initGlobalObject` and forcing the default
// realm into being from a registration would build every builtin at whatever
// moment the host happened to call.
//
// Without this a host global was reachable only by BARE NAME: the registry is
// consulted by `bronze_global_get`, and nothing ever wrote the name into the
// shape that `globalThis['x']`, `'x' in globalThis` and
// `Object.getOwnPropertyNames(globalThis)` read. A host that registers after
// the default realm exists — which is every host that registers more than one
// name, since the first lookup of `globalThis` creates it — had every
// subsequent name invisible to feature detection.
//
// A name the BUILTIN ladder answers is not written, because the ladder is
// asked first (deliberately: a host must not swap out `Math` under compiled
// code) and the property would then disagree with the bare name. `performance`
// is the one name a host may shadow, so it is the one exception.
void rtDefineHostGlobalOnLiveRealms(const std::string& name, Value value);

}  // namespace bronze
