#pragma once

#include <cstdint>
#include <functional>
#include <span>
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

    // The host globals this realm was given (host_globals.h): registered into
    // the realm that is current at the registration, and into no other. The
    // builtins are the thread's and shared (lockdown.h freezes them); what a
    // host installs — its namespaces, its classes, its document — is mutable,
    // so one realm reaching another's would let it change what the other
    // calls. A realm therefore starts with none but the ones registered before
    // any realm existed, which the default realm adopts. A pending lazy name's
    // value is the hole and its init is in `lazyHostGlobals`.
    std::vector<std::pair<std::string, Value>>& hostGlobals() noexcept { return hostGlobals_; }
    std::vector<std::pair<std::string, std::function<void()>>>& lazyHostGlobals() noexcept {
        return lazyHostGlobals_;
    }

    // GC root tracing for this realm's held heap values.
    void visitRoots(const Heap::RootVisitor& visit);

    // Populate the global object with builtins, host globals, and globalThis reference.
    void initGlobalObject(Value customGlobal = Value::fromUndefined());

    // The number naming this realm on its thread (1 and up, never reused while
    // the thread lives; 0 is "no realm"). Every function object and native
    // handle made while the realm is current carries it: a host's call into a
    // function enters the function's realm (embed::call), a handle unwraps
    // only in the realm that made it (rtHandleData), and the realm's compiled
    // modules have instances of their own (module_instance.cpp).
    uint32_t slot() const noexcept { return slot_; }
    void setSlot(uint32_t slot) noexcept { slot_ = slot; }

    // The realm's ownership group: realms of one group (a host's frames of
    // one app, as the web's same-origin frames) unwrap each other's native
    // handles; a realm of another group (another app) does not. A realm
    // starts in a group of its own, named by its slot.
    uint32_t group() const noexcept { return group_ != 0 ? group_ : slot_; }
    void setGroup(uint32_t group) noexcept { group_ = group; }

private:
    uint32_t slot_ = 0;
    uint32_t group_ = 0;
    Value globalObject_{Value::fromUndefined()};
    Value globalEnv_{Value::fromUndefined()};
    std::vector<std::pair<std::string, Value>> moduleRegistry_;
    std::vector<std::pair<std::string, Value>> hostGlobals_;
    std::vector<std::pair<std::string, std::function<void()>>> lazyHostGlobals_;
};

// The builtin names every realm's global object is populated with, each one a
// name `rtResolveBuiltinGlobal` answers. Lockdown starts its walk here.
std::span<const char* const> rtBuiltinGlobalNames();

// Thread-local active Realm accessors and stack operations.
Realm* rtCurrentRealm();
Realm* rtDefaultRealm();
Realm* rtCreateRealm(Value customGlobal = Value::fromUndefined());
void rtDestroyRealm(Realm* realm);
void rtEnterRealm(Realm* realm);
void rtExitRealm();

// Trace all live realms on the calling thread during GC.
void rtVisitRealmRoots(const Heap::RootVisitor& visit);

// The largest realm slot: a function object keeps its realm's slot in three
// bytes of padding whose top byte must never spell a heap tag (fn.h).
inline constexpr uint32_t kMaxRealmSlot = 0x7FFFFF;

// The current realm's slot WITHOUT creating the default realm: 0 while no
// realm exists on this thread (the builtins made before one, which belong to
// every realm). Allocates nothing.
uint32_t rtCurrentRealmSlot() noexcept;
// The live realm on this thread with `slot`, or null for 0, a slot never
// handed out, or one whose realm was destroyed.
Realm* rtRealmForSlot(uint32_t slot) noexcept;
// Whether `slot` named a realm on this thread that has since been destroyed.
bool rtRealmSlotDestroyed(uint32_t slot) noexcept;
// A call into a function made in realm `slot` runs it in that realm, as
// 10.2.1 PrepareForOrdinaryCall makes the callee's [[Realm]] the current
// one: its free names are its own realm's globals and its module data its
// realm's instance, whoever called it. Every generic call path takes one
// (bronze_dynamic_call, FunctionHeader::call, embed::call); a method cache
// never latches another realm's function, so its fast path stays
// same-realm. Slot 0 (a native, an intrinsic) and the current realm's own
// slot enter nothing. A function whose realm was destroyed cannot run:
// dead() answers true and the caller throws kDeadRealmCall.
class CalleeRealmScope {
public:
    explicit CalleeRealmScope(uint32_t slot);
    ~CalleeRealmScope();
    CalleeRealmScope(const CalleeRealmScope&) = delete;
    CalleeRealmScope& operator=(const CalleeRealmScope&) = delete;
    bool dead() const noexcept { return dead_; }

private:
    bool entered_ = false;
    bool dead_ = false;
};
inline constexpr const char* kDeadRealmCall = "the function's realm was destroyed (its app has stopped)";

// Whether what a realm of `slot` made belongs to the current realm's
// ownership group (Realm::group): true for 0 (made before any realm) and for
// the current realm's own slot, false for a destroyed realm's.
bool rtRealmSlotInCurrentGroup(uint32_t slot) noexcept;

// The realm a host-global registration on this thread reaches: the current
// one, or null while no realm exists yet. Never creates the default realm —
// forcing it into being from a registration would build every builtin at
// whatever moment the host happened to call — so a registration made before
// any realm is held by the thread and adopted by the default realm when it is
// made (Realm::hostGlobals).
Realm* rtHostGlobalRealm();

// The live realm on this thread whose global object `glob` is, or null.
Realm* rtRealmOfGlobal(Value glob);

// Define a host-registered global as an own property of `realm`'s global
// object, the realm the registration went to.
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
void rtDefineHostGlobalOnRealm(Realm* realm, const std::string& name, Value value);
// A pending lazy host global (host_globals.h) as the accessor pair the embed
// layer's hook builds, on `realm`'s global object.
void rtDefineLazyHostGlobalOnRealm(Realm* realm, const std::string& name);
void rtTeardownThreadRealms();

// Called by the realm code when a realm is about to be destroyed, before its
// tables go: a host that keeps per-realm state (embed's HostClass slots) drops
// what it holds for that realm. Process-wide hooks, run on the destroying
// thread; registered once at startup.
using RealmDestroyHook = void (*)(Realm* realm);
void rtAddRealmDestroyHook(RealmDestroyHook hook);

}  // namespace bronze
