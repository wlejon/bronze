#include "runtime/map.h"

#include <cmath>
#include <cstring>

#include "runtime/fatal.h"
#include "runtime/string.h"

namespace bronze {

namespace {

constexpr uint32_t kInitialCapacity = 8;

// Buckets per entry slot. Two keeps the load factor at or below 0.5, which is
// what makes linear probing's clustering bounded rather than quadratic.
constexpr uint32_t kBucketsPerSlot = 2;

uint32_t bucketCountFor(uint32_t capacity) noexcept {
    uint32_t n = 8;
    while (n < capacity * kBucketsPerSlot) n *= 2;
    return n;
}

uint32_t mix64(uint64_t x) noexcept {
    // splitmix64's finalizer: cheap, and it moves every input bit into the low
    // ones, which is what a power-of-two bucket mask reads.
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<uint32_t>(x);
}

// The hash SameValueZero implies. Two keys that compare equal must hash equal,
// so `-0` is normalized to `+0` and every NaN to the canonical one before the
// bits are read, and a string hashes by its CHARACTERS rather than by its
// address — two string objects with the same characters are one key.
uint32_t hashKey(Value v) noexcept {
    if (v.isNumber()) {
        double d = v.asNumber();
        if (d == 0.0) d = 0.0;                       // -0 and +0 are one key
        if (std::isnan(d)) return mix64(kCanonicalNaNBits);
        return mix64(std::bit_cast<uint64_t>(d));
    }
    if (v.isString()) return mix64(v.asString<StringHeader>()->hash());
    // An object, a symbol, a boolean or a singleton: identity, which for a
    // heap pointer means its CURRENT address. That is what makes the epoch
    // check in reindex() load-bearing rather than defensive.
    return mix64(v.rawBits());
}

// The map's current address, as a double. Heap pointers are below 2^47
// (heap.cpp reserves in the low range and refuses anything else), so the
// conversion is exact and the comparison is not an approximation.
double selfAddress(Rooted<Value>& self) noexcept {
    return static_cast<double>(
        reinterpret_cast<uintptr_t>(self.get().asObject<HeapObjectHeader>()));
}

uint32_t* bucketsOf(MapHeader* map) noexcept {
    if (!map->index.isPointer()) return nullptr;
    return map->index.asObject<HeapObjectHeader>()->payload<uint32_t>();
}

uint32_t bucketCountOf(const MapHeader* map) noexcept {
    if (!map->index.isPointer()) return 0;
    const auto* hdr = map->index.asObject<HeapObjectHeader>();
    return static_cast<uint32_t>((hdr->size - sizeof(HeapObjectHeader)) / sizeof(uint32_t));
}

// Rebuilds the bucket table from the entry table. Allocates the block when
// there is none, so the map is reached through the root afterwards.
void reindex(Heap& heap, Rooted<Value>& self) {
    auto* map = self.get().asObject<MapHeader>();
    const uint32_t wanted = bucketCountFor(map->capacity());
    if (bucketCountOf(map) != wanted) {
        HeapObjectHeader* block = heap.allocate(wanted * sizeof(uint32_t), Tag::RawBytes);
        map = self.get().asObject<MapHeader>();  // the allocation may have moved it
        map->index = Value::fromObject(block);
    }
    uint32_t* buckets = bucketsOf(map);
    const uint32_t mask = wanted - 1;
    std::memset(buckets, 0, wanted * sizeof(uint32_t));

    const uint32_t used = map->used();
    for (uint32_t slot = 0; slot < used; ++slot) {
        if (!map->liveAt(slot)) continue;
        uint32_t b = hashKey(map->keyAt(slot)) & mask;
        while (buckets[b] != 0) b = (b + 1) & mask;
        buckets[b] = slot + 1;
    }
    map->indexEpoch = Value::fromDouble(static_cast<double>(heap.relocation_epoch()));
    map->indexAnchor = Value::fromDouble(selfAddress(self));
}

// The index is valid only while nothing has moved, for the reason hashKey's
// last branch gives. Two independent tests, because either one alone is a
// number somebody has to remember to maintain: the epoch is bumped by the
// collector's own copy, and the anchor is this map's address, which a
// collector cannot relocate anything without also changing.
void ensureIndex(Heap& heap, Rooted<Value>& self) {
    auto* map = self.get().asObject<MapHeader>();
    if (map->index.isPointer() &&
        map->indexEpoch.asNumber() == static_cast<double>(heap.relocation_epoch()) &&
        map->indexAnchor.asNumber() == selfAddress(self)) {
        return;
    }
    reindex(heap, self);
}

// Grow (or, when tombstones are what filled the table, compact in place). The
// entry block is rebuilt live-entries-first, so a delete-heavy map does not
// grow without bound — and the surviving entries keep their relative order,
// which is the whole contract of the table.
void growEntries(Heap& heap, Rooted<Value>& self) {
    auto* map = self.get().asObject<MapHeader>();
    const uint32_t live = map->liveSize();
    // Sized from the LIVE count, not the old capacity, so a table whose used
    // slots are mostly tombstones compacts instead of growing — which is what
    // makes `m.set(k, v); m.delete(k);` in a loop bounded. The factor of two
    // leaves room for at least `live` more inserts before the next rebuild,
    // which is what keeps the amortized cost of an insert constant.
    uint32_t newCap = kInitialCapacity;
    while (newCap < (live + 1) * 2) newCap *= 2;

    HeapObjectHeader* block = heap.allocate(newCap * 2 * sizeof(Value), Tag::Object);
    Value* dst = block->payload<Value>();
    for (uint32_t i = 0; i < newCap * 2; ++i) dst[i] = Value::fromUndefined();

    map = self.get().asObject<MapHeader>();  // the allocation may have moved it
    const Value* src = map->entryData();
    uint32_t at = 0;
    for (uint32_t slot = 0, used = map->used(); slot < used; ++slot) {
        if (src[slot * 2].isHole()) continue;
        dst[at * 2] = src[slot * 2];
        dst[at * 2 + 1] = src[slot * 2 + 1];
        ++at;
    }
    map->entries = Value::fromObject(block);
    map->usedCount = Value::fromDouble(static_cast<double>(at));
    map->liveCount = Value::fromDouble(static_cast<double>(at));
    reindex(heap, self);
}

}  // namespace

bool sameValueZero(Value a, Value b) noexcept {
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber();
        const double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y)) return true;
        return x == y;  // +0 == -0, which SameValueZero wants and SameValue does not
    }
    if (a.isString() && b.isString()) {
        return a.asString<StringHeader>()->equals(*b.asString<StringHeader>());
    }
    return a.rawBits() == b.rawBits();
}

uint32_t MapHeader::capacity() const noexcept {
    if (!entries.isPointer()) return 0;
    const auto* hdr = entries.asObject<HeapObjectHeader>();
    return static_cast<uint32_t>((hdr->size - sizeof(HeapObjectHeader)) / (sizeof(Value) * 2));
}

MapHeader* MapHeader::create(Heap& heap, uint16_t flags) {
    HeapObjectHeader* raw = heap.allocate(sizeof(MapHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* map = reinterpret_cast<MapHeader*>(raw);
    map->header.flags = flags;
    map->entries = Value::fromUndefined();
    map->index = Value::fromUndefined();
    map->liveCount = Value::fromDouble(0.0);
    map->usedCount = Value::fromDouble(0.0);
    map->indexEpoch = Value::fromDouble(-1.0);
    map->indexAnchor = Value::fromDouble(-1.0);
    return map;
}

uint32_t MapHeader::find(Heap& heap, Rooted<Value>& self, Rooted<Value>& key) {
    if (self.get().asObject<MapHeader>()->liveSize() == 0) return UINT32_MAX;
    ensureIndex(heap, self);
    auto* map = self.get().asObject<MapHeader>();
    const uint32_t count = bucketCountOf(map);
    if (count == 0) return UINT32_MAX;
    const uint32_t mask = count - 1;
    const uint32_t* buckets = bucketsOf(map);
    uint32_t b = hashKey(key.get()) & mask;
    // Terminates because the table is never more than half full — tombstoned
    // buckets included, since a slot is handed out once — and every probe
    // either finds an empty bucket or moves on.
    while (buckets[b] != 0) {
        const uint32_t slot = buckets[b] - 1;
        if (map->liveAt(slot) && sameValueZero(map->keyAt(slot), key.get())) return slot;
        b = (b + 1) & mask;
    }
    return UINT32_MAX;
}

void MapHeader::set(Heap& heap, Rooted<Value>& self, Rooted<Value>& key, Rooted<Value>& val) {
    const uint32_t existing = find(heap, self, key);
    if (existing != UINT32_MAX) {
        self.get().asObject<MapHeader>()->entryData()[existing * 2 + 1] = val.get();
        return;
    }
    {
        auto* map = self.get().asObject<MapHeader>();
        if (map->used() >= map->capacity()) growEntries(heap, self);
    }
    ensureIndex(heap, self);

    auto* map = self.get().asObject<MapHeader>();
    const uint32_t slot = map->used();
    map->entryData()[slot * 2] = key.get();
    map->entryData()[slot * 2 + 1] = val.get();
    map->usedCount = Value::fromDouble(static_cast<double>(slot + 1));
    map->liveCount = Value::fromDouble(static_cast<double>(map->liveSize() + 1));

    uint32_t* buckets = bucketsOf(map);
    const uint32_t mask = bucketCountOf(map) - 1;
    uint32_t b = hashKey(key.get()) & mask;
    while (buckets[b] != 0) b = (b + 1) & mask;
    buckets[b] = slot + 1;
}

bool MapHeader::remove(Heap& heap, Rooted<Value>& self, Rooted<Value>& key) {
    const uint32_t slot = find(heap, self, key);
    if (slot == UINT32_MAX) return false;
    auto* map = self.get().asObject<MapHeader>();
    // The Hole marks the tombstone AND drops both references, so a deleted
    // key stops being kept alive by a map that no longer contains it.
    map->entryData()[slot * 2] = Value::fromHole();
    map->entryData()[slot * 2 + 1] = Value::fromUndefined();
    map->liveCount = Value::fromDouble(static_cast<double>(map->liveSize() - 1));
    // The bucket is LEFT pointing at the tombstone, which `find` probes past.
    // Repairing a linear-probe chain in place is the part of open addressing
    // that is easy to get subtly wrong, and the alternative — invalidating the
    // index — would make a delete-then-lookup loop rebuild the whole table
    // every iteration. The slots are reclaimed by the next compaction.
    return true;
}

void MapHeader::clear(Rooted<Value>& self) {
    auto* map = self.get().asObject<MapHeader>();
    Value* data = map->entryData();
    for (uint32_t i = 0, used = map->used(); i < used; ++i) {
        data[i * 2] = Value::fromHole();
        data[i * 2 + 1] = Value::fromUndefined();
    }
    map->liveCount = Value::fromDouble(0.0);
    map->usedCount = Value::fromDouble(0.0);
    map->indexEpoch = Value::fromDouble(-1.0);
    map->indexAnchor = Value::fromDouble(-1.0);
}

}  // namespace bronze
