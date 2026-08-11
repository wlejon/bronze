// Dictionary mode: what an object becomes the first time a property is
// deleted from it (docs/0019 decision 1). The entry table and the transition
// it replaces live here; the property paths that read them are object.cpp's.

#include "runtime/dictionary.h"

#include <algorithm>

#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/shape.h"

namespace bronze {

const DictEntry* Dictionary::find(const StringHeader* name) const noexcept {
    if (!name) return nullptr;
    for (const DictEntry& e : entries) {
        if (e.name && e.name->equals(*name)) return &e;
    }
    return nullptr;
}

DictEntry* Dictionary::find(const StringHeader* name) noexcept {
    return const_cast<DictEntry*>(static_cast<const Dictionary*>(this)->find(name));
}

bool Dictionary::remove(const StringHeader* name) noexcept {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->name || !it->name->equals(*name)) continue;
        // Only a single slot goes back on the free list. An accessor owns two
        // ADJACENT slots, and a free list of individual slots cannot promise
        // the next accessor two adjacent ones; leaking the pair keeps the
        // allocator honest rather than nearly right.
        if (!it->accessor) freeSlots.push_back(it->slot);
        entries.erase(it);
        return true;
    }
    return false;
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

// A dictionary object's shape is PRIVATE to that object: minted fresh here,
// never a transition target, never shared. Two consequences the rest of the
// system leans on. It can never collide with an inline cache entry, because
// no entry is ever filled with one (docs/0019 decision 5). And its `root` is
// the shape it came from, so `prototypeValue()` still reads a prototype slot
// the collector already walks (rt_state.cpp's root source) — a fresh root
// would need registering, which the arena's owner, not this function, knows
// how to do.
static Shape* createDictionaryShape(NonMovingArena& arena, Shape* from) {
    Shape* dict = arena.create<Shape>();
    dict->root = from->root;
    dict->dict = arena.create<Dictionary>();
    return dict;
}

void ObjectHeader::toDictionary(NonMovingArena& arena, Rooted<Value>& self) {
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || obj->shape->isDictionary()) return;

    Shape* old = obj->shape;
    Shape* dictShape = createDictionaryShape(arena, old);
    Dictionary& d = *dictShape->dict;

    // Walk the chain root-ward, then reverse: the same insertion order
    // ownKeysInInsertionOrder recovers, made explicit because from here on
    // nothing recovers it. The SLOTS are kept exactly as they were, so the
    // conversion moves no data and the object's storage already covers them.
    for (const Shape* curr = old; curr != nullptr; curr = curr->parent) {
        if (!curr->property_name) continue;
        d.entries.push_back(DictEntry{curr->property_name, curr->slot_index, curr->enumerable,
                                      curr->accessor});
        const uint32_t past = curr->slot_index + curr->slotWidth();
        if (past > d.nextSlot) d.nextSlot = past;
    }
    std::reverse(d.entries.begin(), d.entries.end());

    obj->shape = dictShape;
}

bool ObjectHeader::deleteProperty(NonMovingArena& arena, StringHeader* name) {
    if (!shape) return true;
    PropertyInfo info;
    // Absent, or present only on a prototype: already in the state delete
    // wants, and answering true without touching the object is what keeps
    // `delete o.missing` from demoting a record to a dictionary.
    if (!shape->lookupProperty(name, info)) return true;

    // 13.5.1.2 -> 10.1.10.1: a non-configurable property refuses, and in
    // sloppy mode the operator simply answers false. This is the ONLY way
    // `delete` in bronze can answer false, and it is why docs/0019's
    // "delete never answers false" line is retired rather than merely
    // qualified.
    if (!info.configurable) return false;

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
                                       StringHeader* name, bool enumerable, bool accessor,
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
    } else {
        // The entry lives in the arena and outlives every collection, so the
        // name it holds must too — the same rule shape nodes follow, and the
        // reason a computed key (`o[k] = v`, whose key is a fresh heap
        // string) cannot simply be pointed at.
        StringHeader* interned = StringHeader::internToArena(arena, name);
        out_slot = d.allocateSlots(accessor ? 2u : 1u);
        d.entries.push_back(DictEntry{interned, out_slot, enumerable, accessor});
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
