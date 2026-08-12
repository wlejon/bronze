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
    // outlives every collection.
    PropertyKey key;
    uint32_t slot{0};
    bool enumerable{true};
    // The slot holds the getter and `slot + 1` the setter, either of which may
    // be `undefined`.
    bool accessor{false};
    // The other two attributes of 6.2.6.1, which live HERE and nowhere else: a
    // shape transition is matched on (name, enumerable, accessor), and adding
    // two more bits to that key would fork the transition tree for every object
    // that had reached the node a late `writable: false` was defined on. An
    // object that wants a non-default attribute becomes a dictionary instead,
    // which is the same escape `delete` already takes and is the reason the
    // inline caches need no change: a dictionary's private shape is one no
    // entry has ever seen.
    bool writable{true};
    bool configurable{true};
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
class Dictionary {
public:
    // Insertion order, which is enumeration order for the string half of
    // own-enumerable order — with the difference only a delete can make
    // visible: a key re-added after a delete is a NEW insertion and goes to the
    // end, because `erase` took its old position with it.
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

    // True when an entry was there to remove. The slot goes on the free list
    // only for a data property: an accessor's two slots are adjacent and the
    // free list cannot promise adjacency to the next accessor that asks.
    bool remove(PropertyKey name) noexcept;

    // `width` is 1 for a data property and 2 for an accessor pair. A pair
    // always takes fresh slots, for the adjacency reason above.
    uint32_t allocateSlots(uint32_t width);
};

}  // namespace bronze
