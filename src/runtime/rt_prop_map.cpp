// A Map's, Set's, WeakMap's or WeakSet's own NAMED properties — the ordinary
// ones, which have nothing to do with its entries.
//
// 24.1.4 makes a Map an ordinary object with internal slots: `[[MapData]]` is
// reached by `get`, `set`, `has` and `size`, and everything under `m.k` is the
// ordinary property machinery over an ordinary property table. The two stores
// are separate in the language, so they are separate here: `m.foo = 1` puts
// `foo` in the table below and leaves the entry list untouched, and
// `m.set("foo", 1)` does the reverse. `m.get("foo")` after the first is
// `undefined`, `m.size` is 0, and `Object.keys(m)` is `["foo"]`.
//
// The seam is the array's (rt_prop_array.cpp), for the same reason and with
// the same shape: six paths — a read, a write, `in`, `delete`, `for-in` and
// the own-key walks — each reach a collection from their own file, so the
// question "does this Map have an own property called `k`" is answered here
// once instead of restated six times.
//
// WHERE IT LIVES. In `MapHeader::properties`, a plain object with a NULL
// prototype, built on the first named write. Two consequences carry the
// design: a collection that never takes one is the header it always was, and
// the collector needs nothing, because `properties` is a Value in the payload
// the generic scan already forwards.

#include <string>
#include <vector>

#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

bool rtIsMapLike(Value v) {
    if (!v.isObject()) return false;
    const uint16_t flags = v.asObject<HeapObjectHeader>()->flags;
    return flags == MapHeader::kMapFlags || flags == MapHeader::kSetFlags ||
           flags == MapHeader::kWeakMapFlags || flags == MapHeader::kWeakSetFlags;
}

bool rtMapOwnNamed(Value mapVal, PropertyKey name, PropertyInfo& out) {
    const Value props = mapVal.asObject<MapHeader>()->properties;
    if (!props.isObject()) return false;
    ObjectHeader* holder = props.asObject<ObjectHeader>();
    return holder->shape && holder->shape->lookupProperty(name, out);
}

std::vector<StringHeader*> rtMapOwnNamedKeys(Value mapVal, bool enumerableOnly) {
    const Value props = mapVal.asObject<MapHeader>()->properties;
    if (!props.isObject()) return {};
    return rtOwnStringKeysOrdered(props.asObject<ObjectHeader>(), enumerableOnly);
}

SetRefusal rtMapNamedSet(Rooted<Value>& map, Rooted<Value>& key, Rooted<Value>& val) {
    // `size` is the one member of 24.1.3 / 24.2.3 that is an ACCESSOR with no
    // setter, so OrdinarySet finds it on the prototype, finds [[Set]]
    // undefined, and refuses — quietly in sloppy code, as a TypeError in
    // strict. Without this line the write would land in the box below and
    // SHADOW the entry count, which is the one way an expando could lie about
    // the collection it is on. Every other member is a writable data property
    // and IS shadowed by an own one, which is why only this name is here.
    const uint16_t kind = map.get().asObject<HeapObjectHeader>()->flags;
    if ((kind == MapHeader::kMapFlags || kind == MapHeader::kSetFlags) &&
        key.get().isString() && rtUtf8Chars(key.get().asString<StringHeader>()) == "size") {
        return SetRefusal::NoSetter;
    }
    MapHeader::ensureProperties(rtHeap(), rtArena(), map);
    Rooted<Value> props{map.get().asObject<MapHeader>()->properties};
    SetRefusal refusal = SetRefusal::None;
    // The RECEIVER is the collection, not the box its properties live in, for
    // the reason rt_prop_array.cpp gives: a setter reached through this write
    // must be handed the object the program wrote to. No inline cache, for the
    // other reason it gives: the shape here belongs to the box and the site's
    // receiver is the Map.
    props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr,
                                                  /*enumerable=*/true, /*defineOwn=*/false,
                                                  map.slot_ptr(), &refusal);
    return refusal;
}

bool rtMapNamedDelete(Value mapVal, PropertyKey name) {
    const Value props = mapVal.asObject<MapHeader>()->properties;
    if (!props.isObject()) return true;
    return props.asObject<ObjectHeader>()->deleteProperty(rtArena(), name);
}

}  // namespace bronze::runtime
