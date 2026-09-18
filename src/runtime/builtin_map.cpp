// `Map` and `Set` — the constructors and the member tables the property path
// answers from. The method bodies are builtin_map_methods.cpp and the iterator
// objects builtin_map_iterator.cpp; builtin_map_internal.h is the seam between
// the three.
//
// The table itself is map.{h,cpp}; what is here is the JS surface over it.
// The seam is that a Map's METHODS are ordinary bronze function objects
// reached through `bronze_prop_get`, exactly as `Array.prototype`'s are —
// there is no Map.prototype object, because a Map carries no shape and so has
// no prototype link to hang one on. That is a real divergence and is recorded
// as one: `m instanceof Map` is false, and `Map.prototype` is a named error.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/builtin_map_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

static_assert(offsetof(MapHeader, entries) == BRONZE_ABI_MAP_HEADER_ENTRIES_OFFSET);
static_assert(offsetof(MapHeader, usedCount) == BRONZE_ABI_MAP_HEADER_USED_OFFSET);
static_assert(MapHeader::kMapFlags == BRONZE_ABI_OBJ_FLAGS_MAP);
static_assert(MapHeader::kSetFlags == BRONZE_ABI_OBJ_FLAGS_SET);

bool rtIsMapOrSet(Value v) {
    if (!v.isObject()) return false;
    const uint16_t f = v.asObject<HeapObjectHeader>()->flags;
    return f == MapHeader::kMapFlags || f == MapHeader::kSetFlags;
}

bool rtIsSetKind(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == MapHeader::kSetFlags;
}

namespace {

// ---- the constructors -------------------------------------------------------

// `new Map(iterable)` and `new Set(iterable)`. 24.1.1.1 step 2 is
// OrdinaryCreateFromConstructor over NewTarget, and bronze performs it at the
// one allocation site every construction goes through — so `receiver` is
// ALREADY the collection this body has to fill, whether the program wrote
// `new Map()` or `new (class extends Map)()`. Filling it in place rather than
// building one and returning it is what makes the derived case work at all: the
// derived constructor's `this` is that object, and its class fields initialize
// on it after `super()` returns.
//
// A plain CALL — `Map()` — has no receiver and is a TypeError; the check is the
// language's step 1 (NewTarget undefined), read through the only witness the
// uniform calling convention offers.
//
// `arg` arrives through a ROOT, not by value: filling the collection is an
// allocation, so an iterable held as raw bits would be read after a
// collection had moved it — which is exactly what `new Set([3, 1, 3, 2])`
// did under BRONZE_GC_STRESS=1 before it did.
uint64_t buildCollection(Rooted<Value>& receiver, Rooted<Value>& arg, uint16_t flags) {
    if (!rtIsNativeConstructReceiver(receiver.get()) ||
        receiver.get().asObject<HeapObjectHeader>()->flags != flags) {
        return rtThrowTypeError(std::string("Constructor ") +
                                (flags == MapHeader::kSetFlags ? "Set" : "Map") +
                                " requires 'new'")
            .rawBits();
    }
    Rooted<Value> self{receiver.get()};
    if (arg.get().isUndefined() || arg.get().isNull()) return self.get().rawBits();

    Rooted<Value> rec{Value(bronze_iter_open(arg.get().rawBits()))};
    if (rtExceptionPending()) return self.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        if (flags == MapHeader::kSetFlags) {
            MapHeader::set(rtHeap(), self, item, item);
        } else {
            if (!item.get().isObject()) {
                rtThrowTypeError("Iterator value is not an entry object");
                break;
            }
            Rooted<Value> k{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(0.0).rawBits()))};
            Rooted<Value> v{
                Value(bronze_elem_get(item.get().rawBits(), Value::fromDouble(1.0).rawBits()))};
            MapHeader::set(rtHeap(), self, k, v);
        }
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return self.get().rawBits();
}

uint64_t mapConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildCollection(receiver, arg, MapHeader::kMapFlags);
}

uint64_t setConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> arg{args[0]};
    return buildCollection(receiver, arg, MapHeader::kSetFlags);
}

using Method = NativeMethod;

const Method kMapMethods[] = {
    {"get", rtMapGetBody, 1, 1},        {"set", rtMapSetBody, 2, 2},
    {"has", rtMapHasBody, 1, 1},        {"delete", rtMapDeleteBody, 1, 1},
    {"clear", rtMapClearBody, 0, 0},    {"forEach", rtMapForEachBody, 1, 1},
    {"keys", rtMapKeysBody, 0, 0},      {"values", rtMapValuesBody, 0, 0},
    {"entries", rtMapEntriesBody, 0, 0},
};

// 24.2.3.8: `Set.prototype.keys` IS `Set.prototype.values` — the same function
// object, named "values" — which the code-pointer interning gives for free
// once both rows name `rtMapValuesBody`. A Set's keys and values are one
// column, so the body does not care which row reached it.
const Method kSetMethods[] = {
    {"add", rtSetAddBody, 1, 1},        {"has", rtMapHasBody, 1, 1},
    {"delete", rtMapDeleteBody, 1, 1},  {"clear", rtMapClearBody, 0, 0},
    {"forEach", rtMapForEachBody, 1, 1}, {"values", rtMapValuesBody, 0, 0},
    {"keys", rtMapValuesBody, 0, 0},    {"entries", rtMapEntriesBody, 0, 0},
};

// Real members of `Map` / `Set` that bronze has not built. `prototype` is on
// both lists deliberately: a Map has no prototype OBJECT here (see the file
// header), and answering `undefined` for it would let a program install a
// method that nothing would ever find.
const char* const kMapUnimplemented[] = {
    "constructor", "prototype",
};
// 24.2.4's seven set operations left this list when builtin_set_ops.cpp landed;
// what remains is what a Set has and bronze has not built.
const char* const kSetUnimplemented[] = {
    "constructor", "prototype",
};

}  // namespace

Value rtMapConstructor(const std::string& name) {
    if (name == "Map") return rtNativeFunction(mapConstructor, 0, "Map", 0);
    if (name == "Set") return rtNativeFunction(setConstructor, 0, "Set", 0);
    return Value::fromUndefined();
}

const char* rtMapConstructorName(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return nullptr;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    if (code == mapConstructor) return "Map";
    if (code == setConstructor) return "Set";
    return nullptr;
}

// 24.1.2.1 `Map.groupBy`, the one own member of the `Map` constructor beyond
// the two ECMA-262 gives every function. It is answered beside the value
// rather than installed on the function object because that object is an
// interned singleton with no property object of its own — the same arrangement
// `rtTypedArrayStatic` answers `Float64Array.from` from. The body is 7.3.35 in
// builtin_group_by.cpp, shared with `Object.groupBy`.
//
// A TABLE for one entry, because two loops read it: the answer below, and the
// installer under it that makes the same member reachable from a subclass.
// One array so the two can never come to disagree about what `Map` carries.
struct ConstructorStatic {
    const char* owner;
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
    uint32_t length;
};

const ConstructorStatic kMapStatics[] = {
    {"Map", "groupBy", rtMapGroupBy, 2, 2},
};

bool rtMapStatic(Value fn, const std::string& key, Value& out) {
    const char* name = rtMapConstructorName(fn);
    if (!name) return false;
    for (const ConstructorStatic& s : kMapStatics) {
        if (std::string(name) != s.owner || key != s.name) continue;
        out = rtNativeFunction(s.code, s.arity, s.name, s.length);
        return true;
    }
    return false;
}

bool rtInstallMapStatics(Rooted<Value>& ctor) {
    const char* name = rtMapConstructorName(ctor.get());
    if (!name) return false;
    const std::string owner(name);
    rtEnsureFunctionProperties(ctor);
    Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
    if (!props.get().isObject()) return false;
    for (const ConstructorStatic& s : kMapStatics) {
        if (owner != s.owner) continue;
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> fn{rtNativeFunction(s.code, s.arity, s.name, s.length)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fn,
                                                     /*ic=*/nullptr, /*enumerable=*/false,
                                                     /*defineOwn=*/true);
    }
    return true;
}

Value rtMapMethod(bool isSetReceiver, const std::string& key) {
    if (isSetReceiver) {
        for (const Method& m : kSetMethods) {
            if (key == m.name) return rtNativeFunction(m.code, m.arity, m.name, m.length);
        }
        // 24.2.4's set operations, from the table beside their bodies — the
        // only members of a Set that read a second collection, and the reason
        // they are a translation unit of their own (builtin_set_ops.cpp).
        size_t opCount = 0;
        const NativeMethod* ops = rtSetOperationMethods(opCount);
        for (size_t i = 0; i < opCount; ++i) {
            if (const NativeMethod& m = ops[i]; key == m.name) {
                return rtNativeFunction(m.code, m.arity, m.name, m.length);
            }
        }
        return Value::fromUndefined();
    }
    for (const Method& m : kMapMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity, m.name, m.length);
    }
    return Value::fromUndefined();
}

// What `in` must answer about a Map or a Set, off the SAME two tables the read
// path answers from — because the two are one question, and a second list here
// is how they would come to disagree.
//
// A name in neither table is not silently false. `rtCheckMapMember` refuses it
// by name if bronze knows the member and has not built it, which is exactly
// what a READ of that name does: `'constructor' in m` and `m.constructor` are
// both the named hard error, and the day the member lands both answers change
// together. Only a name the read path answers `undefined` for reaches the
// `false` below.
bool rtMapHasMember(bool isSetReceiver, const std::string& key) {
    // 24.1.3.10 / 24.2.3.9 make `size` an accessor on the prototype. `in` does
    // not read it, so the getter bronze has not got costs nothing here — the
    // property exists either way, which is the whole difference between this
    // question and a property read.
    if (key == "size") return true;
    if (isSetReceiver) {
        for (const Method& m : kSetMethods) {
            if (key == m.name) return true;
        }
        // Off the same table `rtMapMethod` answers the set operations from, so
        // `'union' in s` and `s.union` cannot come to disagree.
        size_t opCount = 0;
        const NativeMethod* ops = rtSetOperationMethods(opCount);
        for (size_t i = 0; i < opCount; ++i) {
            if (key == ops[i].name) return true;
        }
    } else {
        for (const Method& m : kMapMethods) {
            if (key == m.name) return true;
        }
    }
    rtCheckMapMember(isSetReceiver, key);
    return false;
}

void rtCheckMapMember(bool isSetReceiver, const std::string& key) {
    if (isSetReceiver) {
        rtCheckUnimplementedMember("Set", kSetUnimplemented, std::size(kSetUnimplemented), key);
        return;
    }
    rtCheckUnimplementedMember("Map", kMapUnimplemented, std::size(kMapUnimplemented), key);
}

Value rtMapDefaultIterator(bool isSetReceiver) {
    return isSetReceiver ? rtNativeFunction(rtMapValuesBody, 0, "values", 0)
                         : rtNativeFunction(rtMapEntriesBody, 0, "entries", 0);
}

}  // namespace bronze::runtime
