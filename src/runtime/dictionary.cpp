// Dictionary mode: what an object becomes the first time a property is deleted
// from it, the first time a definition needs an attribute the shared shape
// cannot carry for one object, or once it holds more own properties than a
// record ever has (Shape::kDictionaryThreshold). The entry table and the
// transition it replaces live here; the property paths that read them are
// object.cpp's.

#include "runtime/dictionary.h"

#include <algorithm>

#include "runtime/fatal.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"

namespace bronze {

namespace {

// A string key hashes by CONTENT (two interned copies of one name are one key)
// and a symbol by IDENTITY (two symbols with one description are two keys) —
// the same two rules `PropertyKey::matches` applies, restated as a hash. The
// string's hash is cached on the string itself, so an interned key pays for
// it once and a program's key constant once.
uint32_t hashKey(PropertyKey key) noexcept {
    if (const StringHeader* s = key.string()) return s->hash();
    auto bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.symbol()));
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 33;
    return static_cast<uint32_t>(bits);
}

constexpr uint32_t kMinIndexCapacity = 8;

uint32_t capacityFor(uint32_t live) noexcept {
    // Under 3/4 load at the size asked for, with room to grow before the
    // next rebuild: the next power of two above twice the count.
    uint32_t cap = kMinIndexCapacity;
    while (cap < live * 2 + 2) cap <<= 1;
    return cap;
}

}  // namespace

const DictEntry* Dictionary::find(PropertyKey name) const noexcept {
    if (!name.valid() || index_.empty()) return nullptr;
    const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
    for (uint32_t i = hashKey(name) & mask;; i = (i + 1) & mask) {
        const uint32_t cell = index_[i];
        if (cell == 0) return nullptr;
        const DictEntry& e = entries[cell - 1];
        if (e.live() && e.key.matches(name)) return &e;
    }
}

DictEntry* Dictionary::find(PropertyKey name) noexcept {
    return const_cast<DictEntry*>(static_cast<const Dictionary*>(this)->find(name));
}

void Dictionary::insertIndex(uint32_t entryPos, uint32_t hash) noexcept {
    const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
    uint32_t i = hash & mask;
    while (index_[i] != 0) i = (i + 1) & mask;
    index_[i] = entryPos + 1;
    ++used_;
}

void Dictionary::reindex() {
    live_ = 0;
    for (const DictEntry& e : entries) {
        if (e.live()) ++live_;
    }
    index_.assign(capacityFor(live_), 0u);
    used_ = 0;
    for (uint32_t pos = 0; pos < entries.size(); ++pos) {
        if (entries[pos].live()) insertIndex(pos, hashKey(entries[pos].key));
    }
}

void Dictionary::compact() {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const DictEntry& e) { return !e.live(); }),
                  entries.end());
    reindex();
}

DictEntry& Dictionary::add(PropertyKey stored, uint32_t slot, bool enumerable, bool accessor,
                           bool writable, bool configurable) {
    entries.push_back(DictEntry{stored, slot, enumerable, accessor, writable, configurable});
    ++live_;
    // Tombstoned cells count toward the load, because a probe crosses them
    // too; once they and the living fill three quarters, the rebuild drops
    // the dead and resizes for the living.
    if (index_.empty() || (used_ + 1) * 4 > index_.size() * 3) {
        reindex();
    } else {
        insertIndex(static_cast<uint32_t>(entries.size() - 1), hashKey(stored));
    }
    return entries.back();
}

bool Dictionary::remove(PropertyKey name) noexcept {
    DictEntry* e = find(name);
    if (e == nullptr) return false;
    // Only a single slot goes back on the free list. An accessor owns two
    // ADJACENT slots, and a free list of individual slots cannot promise
    // the next accessor two adjacent ones; leaking the pair keeps the
    // allocator honest rather than nearly right.
    if (!e->accessor) freeSlots.push_back(e->slot);
    // The tombstone: the cell that names this entry stays in every probe
    // chain it sits on, and the entry keeps its position so nothing after it
    // is renumbered.
    e->key = PropertyKey();
    --live_;
    // Once the dead have caught up with the living, the vector is half
    // tombstones and every enumeration walks twice what it needs.
    if (entries.size() > 32 && live_ * 2 <= entries.size()) compact();
    return true;
}

uint32_t Dictionary::allocateSlots(uint32_t width) {
    if (width == 1 && !freeSlots.empty()) {
        uint32_t slot = freeSlots.back();
        freeSlots.pop_back();
        return slot;
    }
    uint32_t slot = nextSlot;
    nextSlot += width;
    return slot;
}

void Dictionary::releaseMemory() noexcept {
    entries.clear();
    entries.shrink_to_fit();
    freeSlots.clear();
    freeSlots.shrink_to_fit();
    index_.clear();
    index_.shrink_to_fit();
    used_ = 0;
    live_ = 0;
}

struct TrackedDictionary {
    ObjectHeader* owner;
    Dictionary* dict;
};

thread_local std::vector<TrackedDictionary> g_dictionaries;

void sweepDictionaries() {
    Heap& heap = runtime::rtHeap();
    size_t keep = 0;
    for (size_t i = 0; i < g_dictionaries.size(); ++i) {
        auto& item = g_dictionaries[i];
        HeapObjectHeader* live = heap.survivor_of(&item.owner->header);
        if (!live) {
            item.dict->releaseMemory();
            continue;
        }
        item.owner = reinterpret_cast<ObjectHeader*>(live);
        g_dictionaries[keep++] = item;
    }
    g_dictionaries.resize(keep);
}

void ensureDictionarySweep() {
    static thread_local const bool registered = [] {
        runtime::rtHeap().add_post_collection_hook(sweepDictionaries);
        return true;
    }();
    (void)registered;
}

// A dictionary object's shape is PRIVATE to that object: minted fresh here,
// never a transition target, never shared. Two consequences the rest of the
// system leans on. It can never collide with an inline cache entry, because no
// entry is ever filled with one. And its `root` is the shape it came from, so
// `prototypeValue()` still reads a prototype slot the collector already walks
// (rt_state.cpp's root source) — a fresh root would need registering, which the
// arena's owner, not this function, knows how to do.
static Shape* createDictionaryShape(NonMovingArena& arena, Shape* from) {
    Shape* dict = arena.create<Shape>();
    dict->root = from->root;
    dict->dict = arena.create<Dictionary>();
    dict->used_as_prototype = from->used_as_prototype;
    return dict;
}

void ObjectHeader::toDictionary(NonMovingArena& arena, Rooted<Value>& self) {
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || obj->shape->isDictionary()) return;

    Shape* old = obj->shape;
    // If this object is used as a prototype anywhere, demoting it to dictionary
    // mode invalidates IC entries that walk through or target it.
    if (old->used_as_prototype) bumpProtoMutationEpoch();
    Shape* dictShape = createDictionaryShape(arena, old);
    Dictionary& d = *dictShape->dict;

    // Walk the chain root-ward, then reverse: the same insertion order
    // ownKeysInInsertionOrder recovers, made explicit because from here on
    // nothing recovers it. The SLOTS are kept exactly as they were, so the
    // conversion moves no data and the object's storage already covers them.
    for (const Shape* curr = old; curr != nullptr; curr = curr->parent) {
        if (!curr->key.valid()) continue;
        // All FOUR attributes, because a shape node carries all four: a
        // property defined `writable: false` stays in shape mode
        // (builtin_object_descriptor.cpp), so copying only `enumerable` and
        // `accessor` re-granted writability and configurability to it the
        // moment anything demoted the object — `Object.preventExtensions`, or
        // a `delete` of some unrelated key. `Object.freeze` and
        // `Object.seal` hid it by re-stamping every entry afterwards;
        // `preventExtensions` does not stamp, and left a non-writable
        // property that could be written and a non-configurable one that
        // could be deleted, with a descriptor that agreed.
        d.entries.push_back(DictEntry{curr->key, curr->slot_index, curr->enumerable,
                                      curr->accessor, curr->writable, curr->configurable});
        const uint32_t past = curr->slot_index + curr->slotWidth();
        if (past > d.nextSlot) d.nextSlot = past;
    }
    std::reverse(d.entries.begin(), d.entries.end());
    d.reindex();

    obj->shape = dictShape;
    ensureDictionarySweep();
    g_dictionaries.push_back({obj, dictShape->dict});
}

bool ObjectHeader::deleteProperty(NonMovingArena& arena, PropertyKey name) {
    if (!shape) return true;
    PropertyInfo info;
    // Absent, or present only on a prototype: already in the state delete
    // wants, and answering true without touching the object is what keeps
    // `delete o.missing` from demoting a record to a dictionary.
    if (!shape->lookupProperty(name, info)) return true;

    // 13.5.1.2 -> 10.1.10.1: a non-configurable property refuses, and in sloppy
    // mode the operator simply answers false. This is the ONLY way `delete` in
    // bronze can answer false, and it is why the "delete never answers false"
    // line is retired rather than merely qualified.
    if (!info.configurable) return false;

    // If deleting a property from an object used as a prototype, any inline
    // cache that resolved through or to this object must be invalidated.
    if (shape->used_as_prototype) bumpProtoMutationEpoch();

    if (!shape->isDictionary()) {
        // Nothing below allocates on the heap, so the root is a formality —
        // but toDictionary takes one because the object it edits must be
        // reachable if that ever changes.
        Rooted<Value> self{Value::fromObject(this)};
        toDictionary(arena, self);
        return self.get().asObject<ObjectHeader>()->shape->dict->remove(name);
    }
    return shape->dict->remove(name);
}

ObjectHeader* ObjectHeader::dictDefine(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                                       PropertyKey name, bool enumerable, bool accessor,
                                       uint32_t& out_slot) {
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) {
        fatal("internal: a dictionary definition on an object that is not in dictionary mode");
    }
    Dictionary& d = *obj->shape->dict;

    if (DictEntry* existing = d.find(name)) {
        existing->enumerable = enumerable;
        if (existing->accessor == accessor) {
            out_slot = existing->slot;
            return obj;
        }
        // The kind changed. The entry keeps its position — DefineOwnProperty
        // redefines a property, it does not re-insert it — and only the
        // storage behind it is re-allocated.
        if (!existing->accessor) d.freeSlots.push_back(existing->slot);
        existing->accessor = accessor;
        existing->slot = d.allocateSlots(accessor ? 2u : 1u);
        out_slot = existing->slot;
        bumpProtoMutationEpoch();
    } else {
        // The entry lives in the arena and outlives every collection, so the
        // name it holds must too — the same rule shape nodes follow, and the
        // reason a computed key (`o[k] = v`, whose key is a fresh heap
        // string) cannot simply be pointed at. A SYMBOL key is already in the
        // arena and is stored as it stands: interning a copy would mint a key
        // that is no longer the symbol the program holds (Shape::addProperty
        // says the same thing for the transition tree).
        PropertyKey stored =
            name.isSymbol() ? name
                            : PropertyKey::forString(StringHeader::internToArena(arena, name.string()));
        out_slot = d.allocateSlots(accessor ? 2u : 1u);
        d.add(stored, out_slot, enumerable, accessor);
        // A depth > 0 entry can never be walking THROUGH this object — a
        // dictionary anywhere on the path is what `cachedProtoHolder` refuses
        // — and a negative entry never speaks for a chain with one on it
        // (`chainIsCacheable`). The bump is therefore owed only where the
        // transition path owes it: an object that is somebody's prototype.
        // It used to be unconditional, which was harmless while only a delete
        // put an object here; a MAP filled by the key lands here too now, and
        // an epoch bump per insert would retire every proto-hit cache in the
        // program once per `cache[id] = v`.
        if (obj->shape->used_as_prototype) bumpProtoMutationEpoch();
    }

    const uint32_t needed = d.nextSlot;
    obj = ensureSlots(heap, self, needed);
    if (accessor) {
        // A fresh pair starts empty so that a half-written accessor reads as
        // the absent half rather than as whatever the slot last held.
        obj->setSlot(out_slot, Value::fromUndefined());
        obj->setSlot(out_slot + 1, Value::fromUndefined());
    }
    return obj;
}

}  // namespace bronze
