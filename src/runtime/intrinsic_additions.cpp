// A realm's additions to the frozen intrinsics (intrinsic_additions.h).

#include "runtime/intrinsic_additions.h"

#include <algorithm>

#include "runtime/builtin_object.h"
#include "runtime/dictionary.h"
#include "runtime/proxy.h"
#include "runtime/heap.h"
#include "runtime/lockdown.h"
#include "runtime/object.h"
#include "runtime/realm.h"
#include "runtime/shape.h"

namespace bronze::runtime {

namespace {

// The table naming a hardened intrinsic, or null for any other object.
const Dictionary* hardenedTable(const ObjectHeader* obj) {
    if (!obj || !obj->shape || !obj->shape->isDictionary()) return nullptr;
    const Dictionary* d = obj->shape->dict;
    return d->hardened ? d : nullptr;
}

}  // namespace

bool rtIntrinsicAdditionSet(ObjectHeader* obj, PropertyKey key, Value value) {
    const Dictionary* table = hardenedTable(obj);
    if (!table || !key.valid() || !rtLockedDown()) return false;
    Realm* realm = rtCurrentRealm();
    if (!realm) return false;
    auto& additions = realm->intrinsicAdditions();
    for (Realm::IntrinsicAddition& a : additions) {
        if (a.table == table && PropertyKey::fromValue(a.key).matches(key)) {
            a.value = value;
            return true;
        }
    }
    const auto& closed = realm->closedIntrinsics();
    if (std::find(closed.begin(), closed.end(), table) != closed.end()) return false;
    additions.push_back({table, key.toValue(), value});
    // An index on an intrinsic is an index on a prototype: an array's holes
    // must look up the chain from now on (rt_array_holes.cpp).
    if (keyCouldBeArrayIndex(key)) tripArrayIndexHazard("index key added to an intrinsic");
    // A read site may hold the key's miss; the epoch is what every cached
    // miss is checked against.
    bumpProtoMutationEpoch();
    return true;
}

bool rtIntrinsicAdditionAt(const ObjectHeader* holder, PropertyKey key, Value& out) {
    Realm* realm = rtCurrentRealm();
    if (!realm || realm->intrinsicAdditions().empty() || !key.valid()) return false;
    const Dictionary* table = hardenedTable(holder);
    if (!table) return false;
    for (const Realm::IntrinsicAddition& a : realm->intrinsicAdditions()) {
        if (a.table == table && PropertyKey::fromValue(a.key).matches(key)) {
            out = a.value;
            return true;
        }
    }
    return false;
}

void rtIntrinsicAdditionDelete(ObjectHeader* obj, PropertyKey key) {
    const Dictionary* table = hardenedTable(obj);
    Realm* realm = rtCurrentRealm();
    if (!table || !realm || !key.valid()) return;
    auto& additions = realm->intrinsicAdditions();
    for (auto it = additions.begin(); it != additions.end(); ++it) {
        if (it->table == table && PropertyKey::fromValue(it->key).matches(key)) {
            additions.erase(it);
            // A site may hold the addition's value through no cache (reads of
            // one are never cached), but a miss cached before it was made
            // must not come back as stale either way.
            bumpProtoMutationEpoch();
            return;
        }
    }
}

bool rtIntrinsicAdditionsAny() {
    Realm* realm = rtCurrentRealm();
    return realm && !realm->intrinsicAdditions().empty();
}

bool rtIntrinsicAdditionHas(Rooted<Value>& obj, PropertyKey key) {
    if (!key.valid()) return false;
    Rooted<Value> link{obj.get()};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!link.get().isObject()) return false;
        if (link.get().asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) return false;
        Value added;
        if (rtIntrinsicAdditionAt(link.get().asObject<ObjectHeader>(), key, added)) return true;
        const uint64_t arg = link.get().rawBits();
        link.set(Value(objectGetPrototypeOf(0, 0, 1, &arg)));
    }
    return false;
}

bool rtIntrinsicAdditionHoldsIndex(const ObjectHeader* obj) {
    Realm* realm = rtCurrentRealm();
    if (!realm || realm->intrinsicAdditions().empty()) return false;
    const Dictionary* table = hardenedTable(obj);
    if (!table) return false;
    for (const Realm::IntrinsicAddition& a : realm->intrinsicAdditions()) {
        if (a.table == table && keyCouldBeArrayIndex(PropertyKey::fromValue(a.key))) return true;
    }
    return false;
}

void rtIntrinsicAdditionsClose(Value obj) {
    if (!obj.isObject()) return;
    const Dictionary* table = hardenedTable(obj.asObject<ObjectHeader>());
    Realm* realm = rtCurrentRealm();
    if (!table || !realm) return;
    auto& closed = realm->closedIntrinsics();
    if (std::find(closed.begin(), closed.end(), table) == closed.end()) closed.push_back(table);
}

bool rtIntrinsicAdditionGet(const ObjectHeader* start, PropertyKey key, Value& out) {
    Realm* realm = rtCurrentRealm();
    if (!realm || realm->intrinsicAdditions().empty() || !key.valid()) return false;
    const ObjectHeader* holder = start;
    for (uint32_t depth = 0; holder && depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (rtIntrinsicAdditionAt(holder, key, out)) return true;
        if (!holder->shape) return false;
        const Value proto = holder->shape->prototypeValue();
        if (!proto.isObject()) return false;
        const auto* protoHdr = proto.asObject<HeapObjectHeader>();
        if (!HeapKind::shapeNamesPrototype(protoHdr->flags)) return false;
        holder = reinterpret_cast<const ObjectHeader*>(protoHdr);
    }
    return false;
}

}  // namespace bronze::runtime
