#pragma once

// Canonical host-class wrapper on the embed surface: one constructor function +
// one prototype PER REALM, with type-brand verification and base-class
// lineage walks on unwrap()/isInstance(). The brand is carried by each handle
// (the class that made it, in the cell), so it holds on any thread, needs no
// lock, and lives exactly as long as the handle; each make() is paired with
// one destructor call, when that handle dies.
//
// Per realm, not per thread, because a prototype is mutable and the realms of
// one thread must not reach each other's (bronze's intrinsics are shared and
// frozen by lockdown; a host's classes are not frozen, so they are not shared
// either). install() makes the class in the CURRENT realm, and make(),
// prototype(), constructor() and installed() answer for the current realm: an
// instance is born on the prototype of the realm it is made in. A realm that
// never installed the class makes bare handles, exactly as a thread that never
// did. Destroying a realm (embed::destroyRealm) drops its slots.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "embed/embed_object_builder.h"

namespace bronze::embed {

class BRONZE_EMBED_API HostClass {
public:
    struct Slots {
        Persistent* proto = nullptr;
        Persistent* ctor = nullptr;
        Persistent* instanceProto = nullptr;
        std::string name;
        std::vector<std::string> aliases;
        bool isGlobal = true;
    };

    void install(const char* name, uint32_t arity, NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr,
                 bool global = true);

    void init(const char* name, const std::function<void(ObjectBuilder&)>& decorate) {
        install(name, 0, nullptr, decorate);
    }

    template <typename T>
    Value createInstance(std::unique_ptr<T> ptr) const {
        return make(ptr.release(), [](void* p) { delete static_cast<T*>(p); });
    }

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;
    void setInstancePrototype(Value p);

    Value make(void* data, HandleDestructor dtor,
               Finalize when = Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    // The payload of a handle THIS class (or a subclass that inherit()ed from
    // it) made; nullptr for anything else, including another class's handle.
    // Allocates nothing.
    void* unwrap(Value val) const;
    bool isInstance(Value val) const;

    Value prototype() const;
    Value constructor() const;

    // Whether install() has run in the CALLING thread's current realm, and the
    // realm's global still names the constructor.
    bool installed() const;

private:
    // (Private, never touched by a client: MSVC's C4251 about its export
    // does not apply.)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
    mutable std::atomic<const HostClass*> base_{nullptr};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    Slots& slots() const;
    const Slots* slotsIfAny() const;
};

BRONZE_EMBED_API Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

}  // namespace bronze::embed
