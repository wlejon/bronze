#include "embed/embed_host_class.h"

#include <span>
#include <unordered_map>

#include "runtime/fatal.h"
#include "runtime/native_handle.h"
#include "runtime/realm.h"

namespace bronze::embed {

namespace {

// (class, realm) -> that realm's constructor and prototype. Thread-local
// because a realm belongs to one thread, and never destroyed: its Persistents
// would otherwise be released at thread exit after the runtime is gone.
struct SlotKey {
    const HostClass* cls;
    const Realm* realm;
    bool operator==(const SlotKey& o) const noexcept { return cls == o.cls && realm == o.realm; }
};
struct SlotKeyHash {
    size_t operator()(const SlotKey& k) const noexcept {
        return std::hash<const void*>()(k.cls) * 31u ^ std::hash<const void*>()(k.realm);
    }
};
using SlotTable = std::unordered_map<SlotKey, HostClass::Slots, SlotKeyHash>;

SlotTable& threadSlots() {
    static thread_local auto* t = new SlotTable();
    return *t;
}

void releaseSlots(HostClass::Slots& s) {
    delete s.ctor;
    delete s.proto;
    delete s.instanceProto;
    s.ctor = nullptr;
    s.proto = nullptr;
    s.instanceProto = nullptr;
}

// A destroyed realm's classes go with it, so its prototypes are collectable and
// a later realm at the same address starts with none.
void dropRealmSlots(Realm* realm) {
    SlotTable& t = threadSlots();
    for (auto it = t.begin(); it != t.end();) {
        if (it->first.realm == realm) {
            releaseSlots(it->second);
            it = t.erase(it);
        } else {
            ++it;
        }
    }
}

void publishGlobal(const char* name, Value val) {
    Persistent v(val);
    registerGlobal(name, v.get());
    GlobalValue gt = globalValue("globalThis");
    if (gt.found && isObject(gt.value)) {
        Persistent global(gt.value);
        setProperty(global.get(), name, v.get());
    }
}

}  // namespace

HostClass::Slots& HostClass::slots() const {
    return threadSlots()[SlotKey{this, currentRealm()}];
}

const HostClass::Slots* HostClass::slotsIfAny() const {
    auto& t = threadSlots();
    auto it = t.find(SlotKey{this, currentRealm()});
    return it == t.end() ? nullptr : &it->second;
}

bool HostClass::installed() const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor || !s->proto) return false;
    GlobalValue gt = globalValue("globalThis");
    if (!gt.found || !isObject(gt.value)) return false;
    Persistent gtP(gt.value);
    if (s->isGlobal && !s->name.empty()) {
        Value cur = getProperty(gtP.get(), s->name);
        if (cur != s->ctor->get()) return false;
    }
    return true;
}

void HostClass::install(const char* name, uint32_t arity, NativeFn body,
                        const std::function<void(ObjectBuilder&)>& decorate,
                        bool global) {
    NativeFn ctorBody = body;
    if (!ctorBody) {
        std::string msg = std::string("TypeError: ") + (name ? name : "") + " is not a constructor";
        ctorBody = [msg](Value, std::span<const Value>) { return throwTypeError(msg); };
    }

    bronze::rtAddRealmDestroyHook(&dropRealmSlots);
    Slots& s = slots();
    releaseSlots(s);
    s.name = name ? name : "";
    s.aliases.clear();
    s.isGlobal = global;

    Persistent ctor(makeFunction(std::move(ctorBody), arity, s.name));
    s.ctor = new Persistent(ctor.get());

    {
        ObjectBuilder proto(getProperty(ctor.get(), "prototype"));
        proto.set("constructor", ctor.get());
        if (decorate) decorate(proto);
        s.proto = new Persistent(proto.get());
    }

    if (global && !s.name.empty()) {
        publishGlobal(s.name.c_str(), s.ctor->get());
    }
}

void HostClass::alias(const char* name) const {
    if (!name || !*name) return;
    Slots& s = const_cast<HostClass*>(this)->slots();
    if (!s.ctor) return;
    s.aliases.emplace_back(name);
    publishGlobal(name, s.ctor->get());
}

void HostClass::inherit(const HostClass& base) const {
    base_.store(&base, std::memory_order_release);
    const Slots* s = slotsIfAny();
    const Slots* b = base.slotsIfAny();
    if (!s || !s->proto || !b || !b->proto) return;
    GlobalValue objectCtor = globalValue("Object");
    if (!objectCtor.found || !isObject(objectCtor.value)) return;
    Persistent objectNs(objectCtor.value);
    Persistent setProto(getProperty(objectNs.get(), "setPrototypeOf"));
    if (!isFunction(setProto.get())) return;
    const Value args[2] = {s->proto->get(), b->proto->get()};
    call(setProto.get(), undefined(), std::span<const Value>(args, 2));
}

void HostClass::setInstancePrototype(Value p) {
    Slots& s = slots();
    delete s.instanceProto;
    s.instanceProto = new Persistent(p);
}

bool HostClass::isInstance(Value val) const {
    return unwrap(val) != nullptr;
}

// The brand is the cell's own (runtime::rtHandleBrand): the class that made
// the handle, written into it at make() and gone with it, so an unwrap is two
// loads and a walk up the lineage, with no table and no lock, on whatever
// thread holds the handle. A HostClass is a process-lifetime object, so the
// pointer a cell carries stays valid as long as any cell does.
void* HostClass::unwrap(Value val) const {
    const void* brand = nullptr;
    void* data = runtime::rtHandleDataAndBrand(val, &brand);
    if (!data) return nullptr;
    for (const auto* c = static_cast<const HostClass*>(brand); c; c = c->base_.load(std::memory_order_acquire)) {
        if (c == this) return data;
    }
    return nullptr;
}

Value HostClass::make(void* data, HandleDestructor dtor, Finalize when) const {
    const Slots* s = slotsIfAny();
    Value p = undefined();
    if (s) {
        p = s->instanceProto ? s->instanceProto->get()
                             : (s->proto ? s->proto->get() : undefined());
    }
    if (!data) {
        if (isUndefined(p)) return makeHandle(data, dtor, when);
        return makeHandle(data, dtor, when, p);
    }
    // Each make() pairs with exactly one dtor(data), when THIS handle dies:
    // several live handles may share a payload (a refcounted one counts each),
    // and each keeps its brand until it dies. (The brand table this replaces
    // was keyed by the payload: the first of two such handles to die erased
    // the brand of the other, which then unwrapped to nullptr while alive and
    // never ran its destructor.) A null dtor registers no finalizer, as
    // before.
    if (!isUndefined(p) && !isObject(p)) {
        fatal("embed: a host class's instance prototype must be a plain object");
    }
    return runtime::rtMakeBrandedHandle(data, dtor, when, isUndefined(p) ? Value::fromNull() : p, this);
}

void HostClass::setStatic(const char* name, Value v) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    Persistent vP(v);
    s->ctor->set(setProperty(s->ctor->get(), name, vP.get()));
    for (const auto& a : s->aliases) {
        publishGlobal(a.c_str(), s->ctor->get());
    }
}

Value HostClass::prototype() const {
    const Slots* s = slotsIfAny();
    return (s && s->proto) ? s->proto->get() : undefined();
}

Value HostClass::constructor() const {
    const Slots* s = slotsIfAny();
    return (s && s->ctor) ? s->ctor->get() : undefined();
}

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
    Value arr = makeArray(static_cast<uint32_t>(count));
    if (!isObject(arr)) {
        return undefined();
    }
    Persistent persistentArr(arr);
    for (size_t i = 0; i < count; ++i) {
        Persistent v(make(i));
        persistentArr.set(setElement(persistentArr.get(), static_cast<uint32_t>(i), v.get()));
    }
    return persistentArr.get();
}

}  // namespace bronze::embed
