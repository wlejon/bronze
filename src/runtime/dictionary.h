#pragma once

#include <cstdint>
#include <vector>

#include "runtime/property_key.h"
#include "runtime/string.h"

namespace bronze {

class NonMovingArena;
class Shape;

// One own property of a dictionary-mode object: the three facts a shape node
// carries, in a container something can be removed from the MIDDLE of.
//
// That is the whole reason dictionary mode exists. A shape node's `slot_index`
// is implied by its position in the transition chain, and shapes are immortal
// and shared, so unlinking one would renumber every shape below it for every
// object that ever took that path. An entry vector owned by ONE object
// renumbers nobody.
struct DictEntry {
    // Arena-interned (a string) or arena-allocated (a symbol) either way, so
    // it is immortal and non-moving — which it has to be, since a dictionary
    // outlives every collection. INVALID on a removed entry: a delete leaves
    // the entry in place as a tombstone so that nothing after it renumbers
    // (the hash index below names entries by position), and a walk over the
    // vector skips it by asking `live()`.
    PropertyKey key;
    uint32_t slot{0};
    bool enumerable{true};
    // The slot holds the getter and `slot + 1` the setter, either of which may
    // be `undefined`.
    bool accessor{false};
    // The other two attributes of 6.2.6.1. A Shape carries them too, and a
    // transition matches on the full attribute tuple — but REDESCRIBING one
    // on a live property is a change a shared shape cannot express for one
    // object of many, so that object diverges into dictionary mode, where the
    // entry is private and can simply be edited. The same escape `delete`
    // takes, and the reason the inline caches need no change: a dictionary's
    // private shape is one no IC entry has ever seen.
    bool writable{true};
    bool configurable{true};

    bool live() const noexcept { return key.valid(); }
};

// What SetIntegrityLevel (ECMA-262 7.3.14) last stamped on the own properties a
// Dictionary does NOT list — an array's ELEMENTS, which live in the element
// block, and a function's `prototype`, which lives in a slot of its own.
//
// One level rather than a bit per property, because nothing in bronze can give
// one element different attributes from its neighbour: `Object.defineProperty`
// takes a plain object only, so `freeze`, `seal` and `preventExtensions` are the
// only operations that reach these, they apply to all of them at once, and they
// only ever move one way. The three states below are therefore the only three
// such storage ever reaches.
enum class IntegrityLevel : uint8_t {
    Open,    // writable and configurable: an ordinary array's elements
    Sealed,  // non-configurable — `delete a[i]` refuses, a write still lands
    Frozen,  // and non-writable, which also takes `length` / `prototype` writes
};

// The own-property table of one object, hung off that object's own private
// Shape. It lives in the non-moving arena beside the shape, so nothing here
// is a GC root: `name` points into the arena and the VALUES stay in the
// object's ordinary slot storage, where the collector already scans them.
//
// Two structures, one table. `entries` is the insertion order, which is what
// enumeration reads; `index_` is an open-addressed hash over it, which is what
// a lookup reads. An object arrives here for one of two reasons — a delete or
// a redefinition the transition tree cannot express, where it is a record with
// a few dozen names, or `Shape::kDictionaryThreshold` own properties, where it
// is a MAP the program keys by id — and the second is the one a linear scan
// cannot serve: a cache of ten thousand entries read once per key would cost
// a hundred million compares to fill.
class Dictionary {
public:
    // Insertion order, which is enumeration order for the string half of
    // own-enumerable order — with the difference only a delete can make
    // visible: a key re-added after a delete is a NEW insertion and goes to the
    // end, because the delete took its old position with it. Removed entries
    // stay as tombstones (`DictEntry::live`) until `compact` drops them; every
    // walk over this vector must skip them.
    std::vector<DictEntry> entries;

    // Slots a delete released. Reused by a later single-slot property so
    // that `o.k = v; delete o.k;` in a loop does not grow the object's slot
    // storage without bound.
    std::vector<uint32_t> freeSlots;

    // One past the highest slot ever handed out — the object's slot storage
    // must cover it.
    uint32_t nextSlot{0};

    // [[Extensible]] (6.1.7.2). False after `Object.freeze` /
    // `Object.preventExtensions`, and the reason those move an object here:
    // an object with a shape has nowhere to record it, and a bit in the
    // header would have to be somewhere the generated fast path's flags word
    // already is.
    bool extensible{true};

    // The level for the storage `entries` cannot describe. `Open` on a plain
    // object's table always — a plain object has no such storage — and the
    // whole of an array's or a function's answer for the parts of it that are
    // not named properties (integrity.h says why the level is kept HERE).
    IntegrityLevel level{IntegrityLevel::Open};

    const DictEntry* find(PropertyKey name) const noexcept;
    DictEntry* find(PropertyKey name) noexcept;

    // Appends a property the caller has already found absent, and indexes it.
    // The key must already be arena-resident (interned string or symbol).
    DictEntry& add(PropertyKey stored, uint32_t slot, bool enumerable, bool accessor,
                   bool writable = true, bool configurable = true);

    // True when an entry was there to remove. The slot goes on the free list
    // only for a data property: an accessor's two slots are adjacent and the
    // free list cannot promise adjacency to the next accessor that asks.
    bool remove(PropertyKey name) noexcept;

    // `width` is 1 for a data property and 2 for an accessor pair. A pair
    // always takes fresh slots, for the adjacency reason above.
    uint32_t allocateSlots(uint32_t width);

    // How many entries are live — `entries.size()` counts tombstones too.
    uint32_t liveCount() const noexcept { return live_; }

    // Rebuilds the index over `entries`, sized for `liveCount()`. `toDictionary`
    // calls it once after filling the vector directly; `add` and `remove` keep
    // the index current themselves.
    void reindex();

private:
    // Drops the tombstones and reindexes. Called from `remove` once the dead
    // outnumber the living, so a delete-heavy map stays bounded at twice its
    // live size and the amortized cost of a delete stays constant.
    void compact();
    void insertIndex(uint32_t entryPos, uint32_t hash) noexcept;

    // Open addressing, capacity a power of two, each cell an entry position
    // plus one (0 = never used). A cell whose entry is a tombstone stays in
    // the probe chain, so a lookup keeps probing past it and a removal never
    // has to reorder anything. Load is kept under 3/4 counting tombstones.
    std::vector<uint32_t> index_;
    uint32_t used_{0};  // cells holding an entry position, dead ones included
    uint32_t live_{0};
};

}  // namespace bronze
