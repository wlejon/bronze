// Dictionary mode: what an object becomes the first time a property is deleted
// from it, the first time a definition needs an attribute the shared shape
// cannot carry for one object, or once it holds more own properties than a
// record ever has (Shape::kDictionaryThreshold). The entry table and the
// transition it replaces live here; the property paths that read them are
// object.cpp's.

#include "runtime/dictionary.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "runtime/fatal.h"
#include "runtime/heap.h"
#include "runtime/ic_harvest.h"
#include "runtime/intrinsic_additions.h"
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

// Two runs of one-byte units, compared a word at a time: a map's keys are
// short, and memcmp is a call through the PLT and a dispatch for them.
inline bool bytesEqual(const char* a, const char* b, uint32_t n) noexcept {
    while (n >= 8) {
        uint64_t x = 0;
        uint64_t y = 0;
        std::memcpy(&x, a, 8);
        std::memcpy(&y, b, 8);
        if (x != y) return false;
        a += 8;
        b += 8;
        n -= 8;
    }
    if (n >= 4) {
        uint32_t x = 0;
        uint32_t y = 0;
        std::memcpy(&x, a, 4);
        std::memcpy(&y, b, 4);
        if (x != y) return false;
        std::memcpy(&x, a + n - 4, 4);
        std::memcpy(&y, b + n - 4, 4);
        return x == y;
    }
    for (uint32_t k = 0; k < n; ++k) {
        if (a[k] != b[k]) return false;
    }
    return true;
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

// Not a standard-layout class (its index is private), so offsetof is only
// conditionally supported; GCC and Clang both answer it for this layout.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(Dictionary, generation) == BRONZE_ABI_DICT_GENERATION_OFFSET,
              "generated code reads a dictionary's generation at this offset");
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

uint64_t Dictionary::nextGeneration() noexcept {
    // Per thread, as the tables are; never 0, which an unfilled cell holds.
    static thread_local uint64_t t_next = 0;
    return ++t_next;
}

const DictEntry* Dictionary::find(PropertyKey name) const noexcept {
    if (!name.valid() || index_.empty()) return nullptr;
    // A string's hash is cached on it (hashed once per string object): read
    // here rather than through a call, as is the compare below — a map's
    // key built per access (`cache[a + ':' + b]`) is a new string object
    // every time, so its every hit is a content compare.
    const StringHeader* ns = name.string();
    if (ns != nullptr && ns->length > maxStringLen_) return nullptr;
    uint32_t hash = 0;
    if (ns != nullptr) {
        const uint32_t f = ns->flags;
        hash = ((f & StringHeader::kHasHashFlag) != 0 ? f & 0xFFFFFFFCU : ns->hash()) >> 2;
    } else {
        hash = hashKey(name);
    }
    if ((hashBloom_ & (uint64_t{1} << (hash & 63u))) == 0) return nullptr;
    const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
    for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
        const uint64_t cell = index_[i];
        if (cell == 0) return nullptr;
        // The cell's hash settles every other key in the chain without
        // reading its entry or its string, each a cache miss in a map of
        // thousands; only an equal hash pays for the compare.
        if (static_cast<uint32_t>(cell >> 32) != hash) continue;
        const DictEntry& e = entries[static_cast<uint32_t>(cell) - 1];
        if (e.key.identity() == name.identity()) return &e;
        // A symbol is only itself; a tombstone's key is no string.
        const StringHeader* es = e.key.string();
        if (ns == nullptr || es == nullptr || es->length != ns->length) continue;
        if (((es->flags | ns->flags) & StringHeader::kUTF16Flag) == 0) {
            if (bytesEqual(es->latin1Data(), ns->latin1Data(), ns->length)) return &e;
        } else if (es->equals(*ns)) {
            return &e;
        }
    }
}

DictEntry* Dictionary::find(PropertyKey name) noexcept {
    return const_cast<DictEntry*>(static_cast<const Dictionary*>(this)->find(name));
}

DictEntry* Dictionary::findLatin1(const char* bytes, uint32_t length, uint32_t hash) noexcept {
    if (index_.empty() || length > maxStringLen_) return nullptr;
    const uint32_t keyHash = hash >> 2;  // hashKey's form
    if ((hashBloom_ & (uint64_t{1} << (keyHash & 63u))) == 0) return nullptr;
    const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
    for (uint32_t i = keyHash & mask;; i = (i + 1) & mask) {
        const uint64_t cell = index_[i];
        if (cell == 0) return nullptr;
        if (static_cast<uint32_t>(cell >> 32) != keyHash) continue;
        DictEntry& e = entries[static_cast<uint32_t>(cell) - 1];
        const StringHeader* s = e.key.string();
        if (s == nullptr || s->length != length) continue;
        if (s->isLatin1()) {
            if (bytesEqual(s->latin1Data(), bytes, length)) return &e;
            continue;
        }
        // A two-byte spelling of a one-byte key (string.cpp hashWide).
        const uint16_t* u = s->utf16Data();
        uint32_t k = 0;
        while (k < length && u[k] == static_cast<unsigned char>(bytes[k])) ++k;
        if (k == length) return &e;
    }
}

void Dictionary::insertIndex(uint32_t entryPos, uint32_t hash) noexcept {
    const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
    uint32_t i = hash & mask;
    while (index_[i] != 0) i = (i + 1) & mask;
    index_[i] = (static_cast<uint64_t>(hash) << 32) | (entryPos + 1);
    ++used_;
    hashBloom_ |= uint64_t{1} << (hash & 63u);
    if (const StringHeader* s = entries[entryPos].key.string(); s != nullptr && s->length > maxStringLen_) {
        maxStringLen_ = s->length;
    }
}

void Dictionary::reindex() {
    live_ = 0;
    for (const DictEntry& e : entries) {
        if (e.live()) ++live_;
    }
    index_.assign(capacityFor(live_), uint64_t{0});
    used_ = 0;
    hashBloom_ = 0;
    maxStringLen_ = 0;
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
    if (notifiesGlobalReads) runtime::rtBumpGlobalReadEpoch();
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
    removeEntry(e);
    return true;
}

void Dictionary::removeEntry(DictEntry* e) noexcept {
    // Only a single slot goes back on the free list. An accessor owns two
    // ADJACENT slots, and a free list of individual slots cannot promise
    // the next accessor two adjacent ones; leaking the pair keeps the
    // allocator honest rather than nearly right.
    if (!e->accessor) freeSlots.push_back(e->slot);
    // Remove from open-addressed index immediately via Knuth Algorithm R
    // backward-shift deletion so lookups never traverse hash tombstones.
    if (!index_.empty() && e->key.valid()) {
        const uint32_t entryPos = static_cast<uint32_t>(e - entries.data());
        const uint32_t hash = hashKey(e->key);
        const uint32_t mask = static_cast<uint32_t>(index_.size()) - 1;
        const uint64_t target = (static_cast<uint64_t>(hash) << 32) | (entryPos + 1);
        uint32_t hole = hash & mask;
        while (index_[hole] != 0 && index_[hole] != target) hole = (hole + 1) & mask;
        if (index_[hole] == target) {
            uint32_t cur = (hole + 1) & mask;
            while (index_[cur] != 0) {
                const uint32_t home = static_cast<uint32_t>(index_[cur] >> 32) & mask;
                if (((hole - home) & mask) < ((cur - home) & mask)) {
                    index_[hole] = index_[cur];
                    hole = cur;
                }
                cur = (cur + 1) & mask;
            }
            index_[hole] = 0;
            --used_;
        }
    }
    e->key = PropertyKey();
    --live_;
    // A key a running for-in snapshotted may be this one (rt_enumerate.cpp).
    noteForInKeyRemoved();
    retireAnswers();
    // Once the dead have caught up with the living, compact the entries vector
    // so iteration and allocation stay bounded.
    if (entries.size() > 32 && live_ * 2 <= entries.size()) compact();
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
    hashBloom_ = 0;
    maxStringLen_ = 0;
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
// saw for its report at exit, so while it runs nothing is freed either; nor
// while a harvest is armed, whose sites keep their last receiver's shape
// (il/feedback_site.h kLastShapeArm), a dictionary one included, and name
// it when the harvest is written.
void releaseDictionaryShape(void* shapePtr) {
    auto* shape = static_cast<Shape*>(shapePtr);
    Dictionary* d = shape->dict;
    if (d->level == IntegrityLevel::Frozen || runtime::g_shapeCensusEnabled || runtime::rtIcHarvestArmed()) {
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
    dict->copyInlineSlots(from);
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
    if (old->used_as_prototype) {
        bumpProtoMutationEpoch();
        runtime::rtNoteGlobalShapeChange(old);
    }
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

void ObjectHeader::reshapeDictionary(Rooted<Value>& self) {
    auto* obj = self.get().asObject<ObjectHeader>();
    Shape* old = obj->shape;
    if (!old || !old->isDictionary()) return;
    if (old->used_as_prototype) bumpProtoMutationEpoch();
    Shape* fresh = new Shape();
    fresh->root = old->root;
    fresh->dict = new Dictionary(*old->dict);
    fresh->dict->retireAnswers();
    fresh->copyInlineSlots(old);
    fresh->used_as_prototype = old->used_as_prototype;
    obj->shape = fresh;
    // The old shape is left to the finalizer already registered for it; a
    // frozen table's shape outlives its object (releaseDictionaryShape), so
    // no cache entry can come to name another object at its address.
    Heap& heap = runtime::rtHeap();
    if (heap.contains(obj)) {
        heap.gc().add_finalizer(reinterpret_cast<uintptr_t>(obj), &releaseDictionaryShape, fresh);
    }
}

bool ObjectHeader::deleteProperty(NonMovingArena& arena, PropertyKey name) {
    if (!shape) return true;
    if (runtime::rtMaybeGlobalObjectShape(shape)) runtime::rtNoteGlobalObjectWrite(this, name);
    if (shape->isDictionary()) {
        // A dictionary-mode object (a program's map) answers the lookup and
        // the removal from one probe of its table; the steps are the ones
        // below.
        DictEntry* e = shape->dict->find(name);
        // Absent from a frozen intrinsic: the realm's addition, if it made
        // one, is what goes (intrinsic_additions.h).
        if (e == nullptr) {
            runtime::rtIntrinsicAdditionDelete(this, name);
            return true;
        }
        if (!e->configurable) return false;
        if (shape->used_as_prototype) bumpProtoMutationEpoch();
        const uint32_t slot = e->slot;
        const bool accessor = e->accessor;
        shape->dict->removeEntry(e);
        setSlot(slot, Value::fromUndefined());
        if (accessor) setSlot(slot + 1, Value::fromUndefined());
        return true;
    }
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
                                       uint32_t& out_slot, bool configurable) {
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->isDictionary()) {
        fatal("internal: a dictionary definition on an object that is not in dictionary mode");
    }
    Dictionary& d = *obj->shape->dict;

    if (DictEntry* existing = d.find(name)) {
        existing->enumerable = enumerable;
        existing->configurable = configurable;
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
        d.retireAnswers();
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
        d.add(stored, out_slot, enumerable, accessor, /*writable=*/true, configurable);
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
