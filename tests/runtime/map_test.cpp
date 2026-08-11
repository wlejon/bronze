// The Map/Set table (docs/0021 decision 4). Two things are pinned here that
// an oracle case cannot reach: SameValueZero on the edges the language cares
// about, and the hash index agreeing with a linear scan over hundreds of
// mixed-kind keys — including across collections, which is the whole reason
// the index carries an epoch. A wrong bucket answers "not found", which is a
// silent wrong answer, so it is proved against an independent oracle rather
// than against a handful of examples.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;

namespace {

// The answer the table must agree with: a scan of the entry vector in
// insertion order, which is the definition every method here is built on.
uint32_t linearFind(MapHeader* map, Value key) {
    for (uint32_t i = 0, used = map->used(); i < used; ++i) {
        if (!map->liveAt(i)) continue;
        if (sameValueZero(map->keyAt(i), key)) return i;
    }
    return UINT32_MAX;
}

}  // namespace

TEST_CASE("SameValueZero is === with NaN matching itself") {
    const Value nan = Value::fromDouble(std::nan(""));
    CHECK(sameValueZero(nan, nan));
    CHECK(sameValueZero(Value::fromDouble(0.0), Value::fromDouble(-0.0)));
    CHECK(sameValueZero(Value::fromDouble(-0.0), Value::fromDouble(0.0)));
    CHECK_FALSE(sameValueZero(Value::fromDouble(1.0), Value::fromDouble(2.0)));
    CHECK_FALSE(sameValueZero(Value::fromBool(false), Value::fromDouble(0.0)));
    CHECK_FALSE(sameValueZero(Value::fromNull(), Value::fromUndefined()));
}

TEST_CASE("two string objects with the same characters are one key") {
    Heap heap;
    ShadowStackFrame frame;
    Rooted<Value> map{Value::fromObject(MapHeader::create(heap, MapHeader::kMapFlags))};

    Rooted<Value> k1{Value::fromString(StringHeader::createFromUTF8(heap, "hello"))};
    Rooted<Value> k2{Value::fromString(StringHeader::createFromUTF8(heap, "hello"))};
    CHECK(k1.get().rawBits() != k2.get().rawBits());  // distinct objects

    Rooted<Value> v{Value::fromDouble(7.0)};
    MapHeader::set(heap, map, k1, v);
    CHECK(MapHeader::find(heap, map, k2) == 0);
    CHECK(map.get().asObject<MapHeader>()->liveSize() == 1);

    Rooted<Value> v2{Value::fromDouble(8.0)};
    MapHeader::set(heap, map, k2, v2);
    CHECK(map.get().asObject<MapHeader>()->liveSize() == 1);
    CHECK(map.get().asObject<MapHeader>()->valueAt(0).asNumber() == 8.0);
}

TEST_CASE("a re-added key moves to the end and an updated one does not") {
    Heap heap;
    ShadowStackFrame frame;
    Rooted<Value> map{Value::fromObject(MapHeader::create(heap, MapHeader::kMapFlags))};

    for (double d : {1.0, 2.0, 3.0}) {
        Rooted<Value> k{Value::fromDouble(d)};
        Rooted<Value> v{Value::fromDouble(d * 10)};
        MapHeader::set(heap, map, k, v);
    }
    Rooted<Value> two{Value::fromDouble(2.0)};
    Rooted<Value> updated{Value::fromDouble(99.0)};
    MapHeader::set(heap, map, two, updated);
    CHECK(map.get().asObject<MapHeader>()->keyAt(1).asNumber() == 2.0);
    CHECK(map.get().asObject<MapHeader>()->valueAt(1).asNumber() == 99.0);

    CHECK(MapHeader::remove(heap, map, two));
    CHECK_FALSE(MapHeader::remove(heap, map, two));
    Rooted<Value> back{Value::fromDouble(2.0)};
    Rooted<Value> v{Value::fromDouble(20.0)};
    MapHeader::set(heap, map, back, v);

    std::vector<double> order;
    auto* live = map.get().asObject<MapHeader>();
    for (uint32_t i = 0; i < live->used(); ++i) {
        if (live->liveAt(i)) order.push_back(live->keyAt(i).asNumber());
    }
    CHECK(order == std::vector<double>{1.0, 3.0, 2.0});
}

// The index hashes an OBJECT key by its address and the collector moves
// objects, so this is the case the epoch exists for: every lookup after a
// collection must still find every key.
TEST_CASE("the hash index agrees with a linear scan across collections") {
    Heap heap;
    NonMovingArena arena;
    ShadowStackFrame frame;
    Rooted<Value> map{Value::fromObject(MapHeader::create(heap, MapHeader::kMapFlags))};
    // The keys are held in an array of their own as well as by the map, so
    // that "the map lost it" and "the key moved" are different failures — one
    // root covers all of them and the collector forwards both copies.
    Rooted<Value> keys{Value::fromObject(ArrayHeader::create(heap, 8))};
    keys.get().asObject<ArrayHeader>()->header.flags = 1;

    constexpr uint32_t kCount = 300;
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k;
        switch (i % 4) {
            case 0: k.set(Value::fromDouble(static_cast<double>(i))); break;
            case 1:
                k.set(Value::fromString(
                    StringHeader::createFromUTF8(heap, "key-" + std::to_string(i))));
                break;
            case 2:
                k.set(Value::fromObject(
                    ObjectHeader::create(heap, arena, Shape::createRoot(arena))));
                break;
            // A handful of duplicates, so the table has to update rather than
            // insert and the live count has to stop growing.
            default: k.set(Value::fromDouble(static_cast<double>(i % 7))); break;
        }
        keys.get().asObject<ArrayHeader>()->setElem(heap, i, k);
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i) * 2.0)};
        MapHeader::set(heap, map, k, v);
    }

    auto checkAll = [&] {
        for (uint32_t i = 0; i < kCount; ++i) {
            Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
            const uint32_t viaIndex = MapHeader::find(heap, map, k);
            const uint32_t viaScan = linearFind(map.get().asObject<MapHeader>(), k.get());
            REQUIRE(viaIndex == viaScan);
            REQUIRE(viaIndex != UINT32_MAX);
        }
    };
    checkAll();
    heap.collect();
    checkAll();

    // Deleting every third key must leave the rest findable: the buckets of
    // the removed entries are left pointing at tombstones, and every probe
    // that used to run through them still has to reach its own entry.
    uint32_t removed = 0;
    for (uint32_t i = 0; i < kCount; i += 3) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        if (MapHeader::remove(heap, map, k)) ++removed;
    }
    CHECK(removed > 0);
    heap.collect();
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        CHECK(MapHeader::find(heap, map, k) ==
              linearFind(map.get().asObject<MapHeader>(), k.get()));
    }

    // Re-adding after the deletions forces a compaction, which rebuilds both
    // tables from scratch — the one path where a lost key would be silent.
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i))};
        MapHeader::set(heap, map, k, v);
    }
    checkAll();
}
