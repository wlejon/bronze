// The dictionary's INDEX, held at the level the language cannot see.
//
// An oracle case can show that a map-sized object still answers every key in
// the right order; it cannot show that it does so in constant time, that a
// deleted key leaves a tombstone the next probe crosses rather than a hole
// that ends it, or that the table compacts once the dead outnumber the living.
// Those are the properties a later change could quietly undo while every
// oracle case still passed — slowly.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;

namespace {

PropertyKey arenaKey(NonMovingArena& arena, Heap& heap, const std::string& s) {
    Rooted<Value> tmp{Value::fromString(StringHeader::createFromUTF8(heap, s.c_str()))};
    return PropertyKey::forString(
        StringHeader::internToArena(arena, tmp.get().asString<StringHeader>()));
}

// A key the dictionary has never been handed: a fresh heap string, matched by
// content, which is what `o[k]` with a computed `k` presents.
PropertyKey probeKey(Heap& heap, Rooted<Value>& holder, const std::string& s) {
    holder.set(Value::fromString(StringHeader::createFromUTF8(heap, s.c_str())));
    return PropertyKey::fromValue(holder.get());
}

}  // namespace

TEST_CASE("dictionary finds by content through a hashed index") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;
    Dictionary d;

    const uint32_t n = 3000;
    for (uint32_t i = 0; i < n; ++i) {
        d.add(arenaKey(arena, heap, "k" + std::to_string(i)), d.allocateSlots(1),
              /*enumerable=*/true, /*accessor=*/false);
    }
    CHECK(d.liveCount() == n);
    CHECK(d.entries.size() == n);

    Rooted<Value> holder{Value::fromUndefined()};
    for (uint32_t i = 0; i < n; i += 97) {
        const DictEntry* e = d.find(probeKey(heap, holder, "k" + std::to_string(i)));
        REQUIRE(e != nullptr);
        CHECK(e->slot == i);
    }
    CHECK(d.find(probeKey(heap, holder, "k" + std::to_string(n))) == nullptr);
    CHECK(d.find(probeKey(heap, holder, "")) == nullptr);
}

TEST_CASE("a removed key is a tombstone the probe crosses and the order keeps") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;
    Dictionary d;

    // Small enough that no compaction runs (that needs > 32 entries), so the
    // tombstone is what every later lookup walks over.
    for (uint32_t i = 0; i < 16; ++i) {
        d.add(arenaKey(arena, heap, "k" + std::to_string(i)), d.allocateSlots(1), true, false);
    }
    Rooted<Value> holder{Value::fromUndefined()};
    CHECK(d.remove(probeKey(heap, holder, "k5")));
    CHECK_FALSE(d.remove(probeKey(heap, holder, "k5")));
    CHECK(d.liveCount() == 15);
    CHECK(d.entries.size() == 16);
    CHECK_FALSE(d.entries[5].live());
    CHECK(d.find(probeKey(heap, holder, "k5")) == nullptr);
    // Every neighbour on the probe chain is still reachable across the dead
    // cell.
    for (uint32_t i = 0; i < 16; ++i) {
        if (i == 5) continue;
        const DictEntry* e = d.find(probeKey(heap, holder, "k" + std::to_string(i)));
        REQUIRE(e != nullptr);
        CHECK(e->slot == i);
    }
    // The freed slot is what the next single-slot add receives, and the
    // re-added key goes to the END: a delete then a re-add is a new insertion
    // in enumeration order.
    const uint32_t slot = d.allocateSlots(1);
    CHECK(slot == 5);
    d.add(arenaKey(arena, heap, "k5"), slot, true, false);
    CHECK(d.entries.size() == 17);
    CHECK(d.entries.back().key.matches(probeKey(heap, holder, "k5")));
    CHECK(d.find(probeKey(heap, holder, "k5"))->slot == 5);
}

TEST_CASE("the table compacts once the dead outnumber the living") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;
    Dictionary d;

    const uint32_t n = 1000;
    for (uint32_t i = 0; i < n; ++i) {
        d.add(arenaKey(arena, heap, "k" + std::to_string(i)), d.allocateSlots(1), true, false);
    }
    Rooted<Value> holder{Value::fromUndefined()};
    for (uint32_t i = 0; i < n; i += 2) {
        REQUIRE(d.remove(probeKey(heap, holder, "k" + std::to_string(i))));
    }
    CHECK(d.liveCount() == n / 2);
    // Half dead is the trigger, and the last removal is what reached it, so
    // the vector is exactly the living now.
    CHECK(d.entries.size() == n / 2);
    // The survivors keep their relative order and their slots.
    uint32_t expect = 1;
    for (const DictEntry& e : d.entries) {
        if (!e.live()) continue;
        CHECK(e.slot == expect);
        CHECK(e.key.matches(probeKey(heap, holder, "k" + std::to_string(expect))));
        expect += 2;
    }
    CHECK(expect == n + 1);
    for (uint32_t i = 1; i < n; i += 2) {
        REQUIRE(d.find(probeKey(heap, holder, "k" + std::to_string(i))) != nullptr);
    }
    for (uint32_t i = 0; i < n; i += 2) {
        CHECK(d.find(probeKey(heap, holder, "k" + std::to_string(i))) == nullptr);
    }
}

TEST_CASE("a symbol key is found by identity, not by description") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;
    Dictionary d;

    Rooted<Value> desc{Value::fromString(StringHeader::createFromUTF8(heap, "s"))};
    SymbolHeader* a = runtime::rtMakeSymbol(desc.get()).asSymbol<SymbolHeader>();
    SymbolHeader* b = runtime::rtMakeSymbol(desc.get()).asSymbol<SymbolHeader>();
    d.add(PropertyKey::forSymbol(a), d.allocateSlots(1), true, false);
    d.add(arenaKey(arena, heap, "s"), d.allocateSlots(1), true, false);
    REQUIRE(d.find(PropertyKey::forSymbol(a)) != nullptr);
    CHECK(d.find(PropertyKey::forSymbol(a))->slot == 0);
    CHECK(d.find(PropertyKey::forSymbol(b)) == nullptr);
    Rooted<Value> holder{Value::fromUndefined()};
    CHECK(d.find(probeKey(heap, holder, "s"))->slot == 1);
}

// The crossing itself: the add that would mint the transition past the
// threshold moves the object instead, keeps every slot where it was, and the
// next add lands in the table.
TEST_CASE("an object crossing the threshold becomes a dictionary in place") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> self{
        Value::fromObject(ObjectHeader::create(heap, arena, Shape::createRoot(arena)))};
    Rooted<Value> key{Value::fromUndefined()};
    Rooted<Value> val{Value::fromUndefined()};
    const uint32_t n = Shape::kDictionaryThreshold + 5;
    for (uint32_t i = 0; i < n; ++i) {
        key.set(Value::fromString(
            StringHeader::createFromUTF8(heap, ("k" + std::to_string(i)).c_str())));
        val.set(Value::fromDouble(static_cast<double>(i)));
        self.set(Value::fromObject(self.get().asObject<ObjectHeader>()->setProp(
            heap, arena, key, val, /*ic=*/nullptr, /*enumerable=*/true)));
        const bool dict = self.get().asObject<ObjectHeader>()->shape->isDictionary();
        CHECK(dict == (i >= Shape::kDictionaryThreshold));
    }
    auto* obj = self.get().asObject<ObjectHeader>();
    REQUIRE(obj->shape->isDictionary());
    CHECK(obj->shape->dict->liveCount() == n);
    for (uint32_t i = 0; i < n; i += 131) {
        key.set(Value::fromString(
            StringHeader::createFromUTF8(heap, ("k" + std::to_string(i)).c_str())));
        PropertyInfo info;
        REQUIRE(obj->shape->lookupProperty(PropertyKey::fromValue(key.get()), info));
        CHECK(info.slot == i);
        CHECK(obj->getSlot(info.slot).asNumber() == static_cast<double>(i));
    }
    std::vector<PropertyKey> keys = obj->shape->ownKeysInInsertionOrder();
    REQUIRE(keys.size() == n);
    key.set(Value::fromString(StringHeader::createFromUTF8(heap, "k0")));
    CHECK(keys.front().matches(PropertyKey::fromValue(key.get())));
    key.set(Value::fromString(
        StringHeader::createFromUTF8(heap, ("k" + std::to_string(n - 1)).c_str())));
    CHECK(keys.back().matches(PropertyKey::fromValue(key.get())));
}
