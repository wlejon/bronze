// ECMA-262 7.3.35 GroupBy and its two poured-out forms, `Object.groupBy`
// (20.1.2.13) and `Map.groupBy` (24.1.2.1).
//
// One file rather than a half in builtin_object.cpp and a half in
// builtin_map.cpp, because the two members ARE one abstract operation with the
// keyCoercion argument flipped: the walk, the callback protocol, the
// first-occurrence group order and the iterator-close-on-throw discipline are
// identical, and the only difference is what a key is (a property key for the
// object form, a SameValueZero collection key for the map form) and what the
// groups are poured into. Splitting it would have put that one difference in
// two places and the other five in both.
//
// The container is filled AS the walk goes rather than built into a list of
// groups first. 7.3.35 step 6.g appends to a group in `groups` or starts a new
// one at the END, and both containers here already have that ordering — a
// plain object's own string keys are in creation order and a Map's entries are
// in insertion order — so the intermediate list would be a copy of the answer.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// What the two forms do with one (key, value) pair. The walk below owns the
// iteration; this owns the container.
struct GroupSink {
    // Both containers allocate on every add, so the sink is reached through a
    // root and never through a raw header.
    virtual void add(Rooted<Value>& key, Rooted<Value>& value) = 0;
    virtual ~GroupSink() = default;
};

// 7.3.35, with the coercion and the container both supplied by the caller.
// Answers false when it left an exception pending, in which case the container
// is whatever the walk had reached — the caller returns undefined and the
// pending cell is what its caller tests.
bool groupByWalk(Rooted<Value>& items, Rooted<Value>& callback, GroupSink& sink,
                 bool propertyKeys) {
    // Step 1 RequireObjectCoercible and step 2 IsCallable, in that order: a
    // null `items` is diagnosed before a bad callback is, which is what a
    // program feature-testing `Object.groupBy(null, f)` observes.
    if (items.get().isNull() || items.get().isUndefined()) {
        rtThrowTypeError("groupBy called on null or undefined");
        return false;
    }
    if (!isCallable(callback.get())) {
        rtThrowTypeError("groupBy: the callback is not a function");
        return false;
    }

    Rooted<Value> rec{Value(bronze_iter_open(items.get().rawBits()))};
    if (rtExceptionPending()) return false;

    double index = 0;
    while (bronze_iter_step(rec.get().rawBits())) {
        if (rtExceptionPending()) break;
        Rooted<Value> value{Value(bronze_iter_value(rec.get().rawBits()))};
        if (rtExceptionPending()) break;

        // Step 6.e: the callback sees (value, index). It is user code, so
        // everything live here is already in a root and the result goes into
        // one before the coercion below allocates.
        Value block[2] = {value.get(), Value::fromDouble(index)};
        Rooted<Value> key{Value(bronze_dynamic_call(callback.get().rawBits(),
                                                    Value::fromUndefined().rawBits(), 2,
                                                    reinterpret_cast<const uint64_t*>(block)))};
        if (rtExceptionPending()) break;

        if (propertyKeys) {
            // Step 6.f: ToPropertyKey, which is where `-0` and `0` become the
            // same group — both spell the string "0" — and where an object key
            // runs its own `toString`.
            key.set(rtToPropertyKey(key));
            if (rtExceptionPending()) break;
            // `rtToPropertyKey` hands a non-object back UNTOUCHED, because the
            // element fast paths want a number to stay a number. A property
            // NAME is a string or a symbol and nothing else, so the rest of
            // 7.1.19 step 3 is done here — which is also where `-0` and `0`
            // become the one group "0", since ToString of either is "0".
            if (!key.get().isString() && !key.get().isSymbol()) {
                key.set(rtValueToString(key.get()));
                if (rtExceptionPending()) break;
            }
        } else {
            // 7.3.35 step 6.g's CanonicalizeKeyedCollectionKey (24.5.1): the
            // ONE normalization a keyed collection performs, so a group keyed
            // by `-0` is found by `map.get(0)`. NaN needs no step here —
            // SameValueZero already matches it against itself.
            if (key.get().isNumber() && key.get().asNumber() == 0.0) {
                key.set(Value::fromDouble(0.0));
            }
        }
        sink.add(key, value);
        if (rtExceptionPending()) break;
        index += 1;
    }
    if (rtExceptionPending()) {
        // Step 6.e.i / 6.f.i: a throw from the callback or the coercion closes
        // the iterator, with the pending exception suppressed so the ORIGINAL
        // failure is the one the program catches.
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return false;
    }
    return true;
}

// 20.1.2.13's container: an object from OrdinaryObjectCreate(NULL) — no
// prototype, so a grouped result inherits neither `toString` nor
// `hasOwnProperty`, and `Object.getPrototypeOf` of one answers null. That is
// not a detail of the pouring; it is what keeps a group named "toString" from
// colliding with an inherited member.
struct ObjectSink final : GroupSink {
    Rooted<Value> out;

    ObjectSink()
        : out{Value::fromObject(ObjectHeader::create(
              rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))} {
        out.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    }

    void add(Rooted<Value>& key, Rooted<Value>& value) override {
        // The object has a null prototype and carries only the arrays this
        // loop puts there, so this read is an own-property read that cannot
        // run user code — but it still goes through the root, because the
        // append below allocates and moves the object.
        Rooted<Value> existing{out.get().asObject<ObjectHeader>()->getProp(rtHeap(), key)};
        if (existing.get().isObject() &&
            existing.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
            bronze_array_append(existing.get().rawBits(), value.get().rawBits());
            return;
        }
        Rooted<Value> group{Value(bronze_create_array(0))};
        bronze_array_append(group.get().rawBits(), value.get().rawBits());
        // CreateDataPropertyOrThrow: enumerable, so `Object.keys` of the
        // result is the group list, which is how the member is read.
        out.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, group,
                                                    /*ic=*/nullptr, /*enumerable=*/true,
                                                    /*defineOwn=*/true);
    }
};

// 24.1.2.1's container: a real Map, so the groups are keyed by SameValueZero
// and an object key groups by identity rather than by "[object Object]".
struct MapSink final : GroupSink {
    Rooted<Value> out;

    MapSink() : out{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))} {}

    void add(Rooted<Value>& key, Rooted<Value>& value) override {
        const uint32_t slot = MapHeader::find(rtHeap(), out, key);
        if (slot != UINT32_MAX) {
            Rooted<Value> group{out.get().asObject<MapHeader>()->valueAt(slot)};
            bronze_array_append(group.get().rawBits(), value.get().rawBits());
            return;
        }
        Rooted<Value> group{Value(bronze_create_array(0))};
        bronze_array_append(group.get().rawBits(), value.get().rawBits());
        MapHeader::set(rtHeap(), out, key, group);
    }
};

}  // namespace

uint64_t rtObjectGroupBy(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> items{args[0]};
    Rooted<Value> callback{args[1]};
    ObjectSink sink;
    if (!groupByWalk(items, callback, sink, /*propertyKeys=*/true)) {
        return Value::fromUndefined().rawBits();
    }
    return sink.out.get().rawBits();
}

uint64_t rtMapGroupBy(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> items{args[0]};
    Rooted<Value> callback{args[1]};
    MapSink sink;
    if (!groupByWalk(items, callback, sink, /*propertyKeys=*/false)) {
        return Value::fromUndefined().rawBits();
    }
    return sink.out.get().rawBits();
}

}  // namespace bronze::runtime
