#include "runtime/rt_replaceable_builtins.h"

#include <cstdint>

#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/realm.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"

namespace bronze::runtime {

namespace {

constexpr size_t kCount = Realm::kReplaceableBuiltins;

// Per thread, each a permanent root: the object holding the builtin, the
// property's name, and the builtin itself (what a realm that replaced nothing
// reads once the property is an accessor).
struct Entry {
    Value holder = Value::fromUndefined();
    Value key = Value::fromUndefined();
    Value original = Value::fromUndefined();
    bool converted = false;
};
thread_local Entry t_entries[kCount];
thread_local bool t_anyRegistered = false;

template <size_t W>
uint64_t replaceableGet(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    const Value replaced = rtCurrentRealm()->replacedBuiltin(W);
    return (replaced.isHole() ? t_entries[W].original : replaced).rawBits();
}

template <size_t W>
uint64_t replaceableSet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    const Value v = argc > 0 ? Value::fromRawBits(argv[0]) : Value::fromUndefined();
    rtCurrentRealm()->setReplacedBuiltin(W, v);
    return Value::fromUndefined().rawBits();
}

struct Accessors {
    bronze_fn_code get;
    bronze_fn_code set;
    const char* getName;
    const char* setName;
};

const Accessors kAccessors[kCount] = {
    {replaceableGet<0>, replaceableSet<0>, "get random", "set random"},
    {replaceableGet<1>, replaceableSet<1>, "get now", "set now"},
};

// The data property becomes the accessor pair, in place (a dictionary entry
// keeps its position and its attributes), and the holder takes a fresh shape.
void convert(size_t w) {
    Entry& e = t_entries[w];
    const Accessors& a = kAccessors[w];
    Rooted<Value> holder{e.holder};
    Rooted<Value> key{e.key};
    Rooted<Value> getter{rtNativeFunction(a.get, 0, a.getName, 0)};
    Rooted<Value> setter{rtNativeFunction(a.set, 1, a.setName, 1)};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), holder, key, getter, setter,
                                 /*enumerable=*/false, /*configurable=*/false);
    ObjectHeader::reshapeDictionary(holder);
    e.holder = holder.get();
    e.converted = true;
}

}  // namespace

void rtRegisterReplaceableBuiltin(Value holder, const char* name, ReplaceableBuiltin which,
                                  Value original) {
    Entry& e = t_entries[static_cast<size_t>(which)];
    e.holder = holder;
    e.original = original;
    e.key = rtMakeString(name);
    rtHeap().add_permanent_root(&e.holder);
    rtHeap().add_permanent_root(&e.original);
    rtHeap().add_permanent_root(&e.key);
    t_anyRegistered = true;
}

bool rtReplaceBuiltinOnWrite(ObjectHeader* obj, PropertyKey name, Value val) {
    if (!t_anyRegistered) return false;
    for (size_t w = 0; w < kCount; ++w) {
        Entry& e = t_entries[w];
        if (!e.holder.isObject() || e.holder.asObject<ObjectHeader>() != obj) continue;
        if (!PropertyKey::fromValue(e.key).matches(name)) continue;
        Rooted<Value> v{val};
        if (!e.converted) convert(w);
        rtCurrentRealm()->setReplacedBuiltin(w, v.get());
        return true;
    }
    return false;
}

}  // namespace bronze::runtime
