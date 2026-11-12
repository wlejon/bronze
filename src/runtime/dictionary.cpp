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
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/shape_leave.h"

namespace bronze {

namespace {

// A string key hashes by CONTENT (two interned copies of one name are one key)
// and a symbol by IDENTITY (two symbols with one description are two keys) —
// the same two rules `PropertyKey::matches` applies, restated as a hash. The
// string's hash is cached on the string itself, so an interned key pays for
// it once and a program's key constant once.
//
// The cached word's low two bits are the string's flags and read zero in the
// hash, so they are shifted out: masked as it stands, every key's home cell
// was a multiple of four and the table probed through runs four times as
// dense as its load.
uint32_t hashKey(PropertyKey key) noexcept {
    if (const StringHeader* s = key.string()) return s->hash() >> 2;
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
    // Placed as a shape node's slot is (Shape::placeSlot): the object's
    // inline slots first, then the overflow block's, numbered from the split.
    uint32_t slot = Shape::placeSlot(nextSlot, width, inlineSlots);
    nextSlot = slot + width;
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

// What the object's death releases, through a collector finalizer on it: the
// dictionary and its private shape, which nothing else names once the object
// is gone — no inline cache, enumeration cache or prototype walk ever keeps a
// dictionary shape that is not frozen (Shape::readCacheable). A FROZEN one may
// be in any of those, keyed by its address, so it stays allocated for the
// thread's life (an address reused by another shape could be answered for);
// only its tables go. Its keys were moved to the arena when it froze
// (integrity.cpp), for the same reason. The shape census keeps every shape it
// saw for its report at exit, so while it runs nothing is freed either.
void releaseDictionaryShape(void* shapePtr) {
    auto* shape = static_cast<Shape*>(shapePtr);
    Dictionary* d = shape->dict;
    if (d->level == IntegrityLevel::Frozen || runtime::g_shapeCensusEnabled) {
        d->releaseMemory();
        return;
    }
    shape->dict = nullptr;
    delete d;
    delete shape;
}

// A dictionary object's shape is PRIVATE to that object: minted fresh here,
// never a transition target, never shared. Two consequences the rest of the
// system leans on. It can never collide with an inline cache entry, because no
// entry is ever filled with one. And its `root` is the shape it came from, so
// `prototypeValue()` still reads a prototype slot the collector already walks
// (rt_state.cpp's root source) — a fresh root would need registering, which the
// arena's owner, not this function, knows how to do.
//
// Unlike a transition node it is not an arena object: it lives exactly as long
// as its object (releaseDictionaryShape), so a program that makes and drops
// dictionary objects — a map per request, a record that had a key deleted —
// does not grow the arena by one shape and one table each.
static Shape* createDictionaryShape(Shape* from) {
    Shape* dict = new Shape();
    dict->root = from->root;
    dict->dict = new Dictionary();
    // The object keeps its storage, and with it its inline capacity.
    dict->setInlineSlots(from->inline_slots);
    dict->dict->inlineSlots = from->inline_slots;
    dict->used_as_prototype = from->used_as_prototype;
    return dict;
}

void ObjectHeader::toDictionary(NonMovingArena& arena, Rooted<Value>& self) {
    (void)arena;
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || obj->shape->isDictionary()) return;

    Shape* old = obj->shape;
    // If this object is used as a prototype anywhere, demoting it to dictionary
    // mode invalidates IC entries that walk through or target it.
    if (old->used_as_prototype) bumpProtoMutationEpoch();
    Shape* dictShape = createDictionaryShape(old);
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

    runtime::noteShapeLeave(old, dictShape, runtime::currentDictionaryReason());
    obj->shape = dictShape;
    // An object of a heap other than the thread's (a test's own) keeps its
    // shape and table for the life of the process.
    Heap& heap = runtime::rtHeap();
    if (heap.contains(obj)) {
        heap.gc().add_finalizer(reinterpret_cast<uintptr_t>(obj), &releaseDictionaryShape, dictShape);
    }
}

bool ObjectHeader::deleteProperty(NonMovingArena& arena, PropertyKey name) {
    if (!shape) return true;
    if (runtime::rtMaybeGlobalObjectShape(shape)) runtime::rtNoteGlobalObjectWrite(this, name);
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

    ObjectHeader* live = this;
    if (!shape->isDictionary()) {
        // Nothing below allocates on the heap, so the root is a formality —
        // but toDictionary takes one because the object it edits must be
        // reachable if that ever changes.
        Rooted<Value> self{Value::fromObject(this)};
        runtime::ShapeLeaveReason why(ShapeLeave::Delete);
        toDictionary(arena, self);
        live = self.get().asObject<ObjectHeader>();
    }
    const bool removed = live->shape->dict->remove(name);
    if (removed) {
        live->setSlot(info.slot, Value::fromUndefined());
        if (info.accessor) live->setSlot(info.slot + 1, Value::fromUndefined());
    }
    return removed;
}

// The form of `name` a dictionary entry holds. A transition node's key is an
// arena string, immortal; an entry's is not, because a dictionary is where a
// program's MAP lives (`cache[path] = v; delete cache[path]`), and an arena
// copy per key ever stored grew the process by every key it had ever seen. So
// an entry names an ordinary string of the OLD generation: it never moves, the
// object's trace keeps it alive while the entry does (heap_trace.cpp), and the
// next full collection after a delete frees it.
//
//  - a symbol, or an arena string of this thread, is stored as it stands: both
//    are immortal already, and a symbol is matched by identity, so a copy
//    would be another key;
//  - an old string of this heap is stored as it stands (not a builder, whose
//    slack a later append could still write into);
//  - anything else — a young string, which a minor collection would move, or
//    another heap's — is copied into this heap's old generation.
//
// While a marking cycle is open the object may already be marked, and the
// entry is an edge only its full trace follows, so the key is shaded (an old
// allocation inside a cycle is marked already; a reused string may not be).
PropertyKey ObjectHeader::dictKeyToStore(Heap& heap, NonMovingArena& arena, PropertyKey name) {
    if (name.isSymbol()) return name;
    StringHeader* s = name.string();
    if (s->isArena()) return PropertyKey::forString(StringHeader::internToArena(arena, s));
    if (heap.contains(s) && !heap.is_movable(s) && !s->isBuilder()) {
        gcShadeEdge(Value::fromString(s));
        return name;
    }
    Rooted<Value> src{Value::fromString(s)};
    StringHeader* copy = StringHeader::copyToOld(heap, src);
    gcShadeEdge(Value::fromString(copy));
    return PropertyKey::forString(copy);
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
        // The key the entry keeps (dictKeyToStore): never a young string,
        // which the next minor collection would move under it. Storing may
        // allocate, so the object is re-read from its root afterwards; the
        // table itself is off the heap and does not move.
        PropertyKey stored = dictKeyToStore(heap, arena, name);
        obj = self.get().asObject<ObjectHeader>();
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
        noteKeyAddedToPrototype(obj->shape->used_as_prototype, stored);
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

ObjectHeader* ObjectHeader::dictAddAbsent(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                                          PropertyKey name, bool enumerable, bool writable,
                                          bool configurable, uint32_t& out_slot) {
    PropertyKey stored = dictKeyToStore(heap, arena, name);
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) {
        fatal("internal: a dictionary insert on an object that is not in dictionary mode");
    }
    Dictionary& d = *obj->shape->dict;
    out_slot = d.allocateSlots(1u);
    d.add(stored, out_slot, enumerable, /*accessor=*/false, writable, configurable);
    if (obj->shape->used_as_prototype) bumpProtoMutationEpoch();
    noteKeyAddedToPrototype(obj->shape->used_as_prototype, stored);
    return ensureSlots(heap, self, d.nextSlot);
}

}  // namespace bronze
