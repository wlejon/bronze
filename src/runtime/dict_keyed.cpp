// MAPS: computed keys of a dictionary-mode object — `cache[key]`,
// `cache[key] = v`, `delete cache[key]` (rt_property.h, "maps").
//
// A program keeps a map in a plain object (`{}` or `Object.create(null)`) and
// keys it by ids or by strings it builds per access (`${text}:${style}`); such
// an object is in dictionary mode (a delete, the transition bound, an index
// key: shape.h). The three operations here are the whole of 10.1.8.1 [[Get]],
// 10.1.9.2 OrdinarySet and 10.1.10.1 [[Delete]] for the cases they take, and
// what makes them cheap is what they leave out:
//
//  - a Number key is not made a string. ToPropertyKey of a uint32 integer is
//    its decimal digits, so the digits are written into a buffer, hashed as
//    StringHeader::hash would hash that string (hashLatin1), and compared
//    against the entries' keys there (Dictionary::findLatin1). A map keyed by
//    ids past the small-integer strings (rtSmallIntString's 1024) allocated a
//    string per read and took the generic walk; it now allocates only the key
//    a write stores, in the old generation where an entry's key lives
//    (ObjectHeader::dictKeyToStore).
//  - a String key, fresh or not, is hashed once (cached on the string) and
//    probed; nothing interns it.
//  - the receivers are the ones nothing else can observe the shortcut on: a
//    bare plain object (no exotic storage, no internal slots), in dictionary
//    mode, that is no prototype (so no inline cache or epoch walks through it,
//    and it is no global object: rtMaybeGlobalObjectShape answers only for a
//    marked shape). Anything else, and every refusal (a non-writable or
//    non-configurable entry, an accessor, a non-extensible map), is the
//    generic path's, which reports it per the caller's strictness.
//
// The chain half of a read that misses and of a write that adds: every link
// must be a bare plain object, asked for the name. A Number key asks the TLS
// array index hazard instead (object.h): while it is clear, no object that is
// or was a prototype holds a key of digits, and no link of any chain is an
// exotic object, so no link holds the key.

#include <charconv>
#include <cstdint>

#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The key, as the table is asked for it: the string, or a uint32 Number's
// digits and their hash.
struct MapKey {
    StringHeader* str = nullptr;
    uint32_t length = 0;
    uint32_t hash = 0;
    char digits[12];
};

// False for a key kind these paths do not take (a symbol, a Number that is
// no uint32 integer, anything else).
inline bool resolveKey(Value key, MapKey& k) noexcept {
    if (key.isString()) {
        k.str = key.asString<StringHeader>();
        return true;
    }
    if (!key.isNumber()) return false;
    const double d = key.asNumber();
    if (!(d >= 0.0 && d < 4294967296.0)) return false;
    const auto n = static_cast<uint32_t>(d);
    if (static_cast<double>(n) != d) return false;  // -0 passes: ToString(-0) is "0"
    k.length = static_cast<uint32_t>(std::to_chars(k.digits, k.digits + sizeof(k.digits), n).ptr - k.digits);
    k.hash = StringHeader::hashLatin1(k.digits, k.length);
    return true;
}

inline DictEntry* findKey(Dictionary& d, const MapKey& k) noexcept {
    if (k.str != nullptr) return d.find(PropertyKey::forString(k.str));
    return d.findLatin1(k.digits, k.length, k.hash);
}

// The receiver these paths take, or null.
inline ObjectHeader* mapReceiver(Value objVal) noexcept {
    if (!objVal.isObject()) return nullptr;
    auto* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return nullptr;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    const Shape* sh = obj->shape;
    if (sh == nullptr || sh->dict == nullptr || sh->used_as_prototype || !obj->isBarePlain()) return nullptr;
    return obj;
}

// What the chain above a map answers for a key the map lacks: Absent when no
// link holds it, Data (with the holder and slot) when the first that does
// holds it as a writable data property, Unknown for anything else (an
// accessor, a non-writable property, a link these paths do not read).
enum class ChainAnswer { Absent, Data, Unknown };

ChainAnswer askChain(const Shape* sh, const MapKey& k, const ObjectHeader*& holder, uint32_t& slot) {
    if (k.str == nullptr) {
        return rtTls()->array_index_hazard == 0 ? ChainAnswer::Absent : ChainAnswer::Unknown;
    }
    const PropertyKey pk = PropertyKey::forString(k.str);
    Value proto = sh->prototypeValue();
    for (int link = 0; link < 8; ++link) {
        if (proto.isNull()) return ChainAnswer::Absent;
        if (!proto.isObject() || proto.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
            return ChainAnswer::Unknown;
        }
        const auto* p = proto.asObject<ObjectHeader>();
        const Shape* ps = p->shape;
        if (ps == nullptr || !p->isBarePlain() || rtMaybeGlobalObjectShape(ps)) return ChainAnswer::Unknown;
        PropertyInfo info;
        if (ps->lookupProperty(pk, info)) {
            if (info.accessor || !info.writable) return ChainAnswer::Unknown;
            holder = p;
            slot = info.slot;
            return ChainAnswer::Data;
        }
        proto = ps->prototypeValue();
    }
    return ChainAnswer::Unknown;
}

}  // namespace

bool rtIdKeyedRead(Value objVal, Value key, Value& out, uint32_t* ownSlot) {
    ObjectHeader* obj = mapReceiver(objVal);
    if (obj == nullptr) return false;
    MapKey k;
    if (!resolveKey(key, k)) return false;
    if (const DictEntry* e = findKey(*obj->shape->dict, k)) {
        if (e->accessor) return false;
        out = obj->getSlot(e->slot);
        if (ownSlot != nullptr) *ownSlot = e->slot;
        return true;
    }
    const ObjectHeader* holder = nullptr;
    uint32_t slot = 0;
    switch (askChain(obj->shape, k, holder, slot)) {
        case ChainAnswer::Absent: out = Value::fromUndefined(); return true;
        case ChainAnswer::Data: out = holder->getSlot(slot); return true;
        case ChainAnswer::Unknown: return false;
    }
    return false;
}

bool rtIdKeyedWrite(Value objVal, Value key, Value val) {
    ObjectHeader* obj = mapReceiver(objVal);
    if (obj == nullptr) return false;
    MapKey k;
    if (!resolveKey(key, k)) return false;
    Dictionary& d = *obj->shape->dict;
    if (DictEntry* e = findKey(d, k)) {
        if (e->accessor || !e->writable) return false;
        obj->setSlot(e->slot, val);
        return true;
    }
    if (!d.extensible) return false;
    const ObjectHeader* holder = nullptr;
    uint32_t inherited = 0;
    if (askChain(obj->shape, k, holder, inherited) == ChainAnswer::Unknown) return false;
    // A new own property (an inherited writable data property is shadowed by
    // it). The steps of ObjectHeader::dictAddAbsent, with the stored key made
    // straight from the digits for a Number. Both allocations can collect, so
    // the map and the value are reached through roots; the table itself is
    // off the heap and stays where it is.
    Heap& heap = rtHeap();
    Rooted<Value> self{objVal};
    Rooted<Value> valRoot{val};
    PropertyKey stored;
    if (k.str != nullptr) {
        stored = ObjectHeader::dictKeyToStore(heap, rtArena(), PropertyKey::forString(k.str));
    } else {
        StringHeader* s = StringHeader::createLatin1Old(heap, k.digits, k.length);
        gcShadeEdge(Value::fromString(s));
        stored = PropertyKey::forString(s);
    }
    const uint32_t slot = d.allocateSlots(1u);
    d.add(stored, slot, /*enumerable=*/true, /*accessor=*/false);
    ObjectHeader* live = ObjectHeader::ensureSlots(heap, self, d.nextSlot);
    live->setSlot(slot, valRoot.get());
    return true;
}

bool rtIdKeyedDelete(Value objVal, Value key) {
    ObjectHeader* obj = mapReceiver(objVal);
    if (obj == nullptr) return false;
    MapKey k;
    if (!resolveKey(key, k)) return false;
    Dictionary& d = *obj->shape->dict;
    DictEntry* e = findKey(d, k);
    if (e == nullptr) return true;
    if (!e->configurable) return false;
    const uint32_t slot = e->slot;
    const bool accessor = e->accessor;
    d.removeEntry(e);
    obj->setSlot(slot, Value::fromUndefined());
    if (accessor) obj->setSlot(slot + 1, Value::fromUndefined());
    return true;
}

}  // namespace bronze::runtime
