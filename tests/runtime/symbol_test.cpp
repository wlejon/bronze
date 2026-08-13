#include <doctest/doctest.h>

#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

Value symbolNamed(const char* text) {
    Rooted<Value> desc{Value::fromString(StringHeader::createFromUTF8(rtHeap(), text))};
    return rtMakeSymbol(desc.get());
}

}  // namespace

TEST_CASE("A symbol is identity, not content") {
    Rooted<Value> a{symbolNamed("tag")};
    Rooted<Value> b{symbolNamed("tag")};

    CHECK(a.get().isSymbol());
    CHECK(a.get().rawBits() != b.get().rawBits());
    CHECK(a.get().rawBits() == a.get().rawBits());
    CHECK(rtSymbolDescriptiveString(a.get()) == "Symbol(tag)");
    CHECK(rtSymbolDescriptiveString(b.get()) == "Symbol(tag)");

    // No description at all, which prints the same as an empty one and is a
    // different state from it.
    Value bare = rtMakeSymbol(Value::fromUndefined());
    CHECK(rtSymbolDescriptiveString(bare) == "Symbol()");
    CHECK(bare.asSymbol<SymbolHeader>()->description == nullptr);
    CHECK(a.get().asSymbol<SymbolHeader>()->description != nullptr);
}

TEST_CASE("PropertyKey matches strings by content and symbols by identity") {
    Heap heap;
    ShadowStackFrame frame;

    // Rooted, and not held as raw pointers: under BRONZE_GC_STRESS each
    // `createFromUTF8` collects, and an unrooted first string is freed before
    // the second is made — which hands the second the SAME address and makes
    // "two distinct strings" quietly untrue.
    Rooted<Value> s1{Value::fromString(StringHeader::createFromUTF8(heap, "k"))};
    Rooted<Value> s2{Value::fromString(StringHeader::createFromUTF8(heap, "k"))};
    Rooted<Value> s3{Value::fromString(StringHeader::createFromUTF8(heap, "other"))};
    StringHeader* one = s1.get().asString<StringHeader>();
    StringHeader* two = s2.get().asString<StringHeader>();
    REQUIRE(one != two);

    PropertyKey ks1 = PropertyKey::forString(one);
    PropertyKey ks2 = PropertyKey::forString(two);
    PropertyKey ks3 = PropertyKey::forString(s3.get().asString<StringHeader>());
    CHECK(ks1.isString());
    CHECK_FALSE(ks1.isSymbol());
    // Two DIFFERENT string objects with the same characters are one key; that
    // is what lets two objects share a shape.
    CHECK(ks1.matches(ks2));
    CHECK_FALSE(ks1.matches(ks3));

    Rooted<Value> a{symbolNamed("k")};
    Rooted<Value> b{symbolNamed("k")};
    PropertyKey sy1 = PropertyKey::fromValue(a.get());
    PropertyKey sy2 = PropertyKey::fromValue(b.get());
    CHECK(sy1.isSymbol());
    CHECK_FALSE(sy1.isString());
    CHECK(sy1.matches(sy1));
    // Same description, different key — the exact opposite of the string rule
    // three lines up, on keys that spell the same thing.
    CHECK_FALSE(sy1.matches(sy2));
    // And the two kinds never match each other, whatever they spell.
    CHECK_FALSE(sy1.matches(ks1));
    CHECK_FALSE(ks1.matches(sy1));

    CHECK_FALSE(PropertyKey().valid());
    CHECK_FALSE(PropertyKey().matches(ks1));
    CHECK_FALSE(PropertyKey::fromValue(Value::fromDouble(1.0)).valid());
    CHECK(sy1.toValue().rawBits() == a.get().rawBits());
}

TEST_CASE("Shape transitions keep symbol keys apart") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Shape* root = Shape::createRoot(arena);
    Rooted<Value> symA{symbolNamed("dup")};
    Rooted<Value> symB{symbolNamed("dup")};
    Rooted<Value> name{Value::fromString(StringHeader::createFromUTF8(heap, "dup"))};

    uint32_t slotA = 0;
    uint32_t slotB = 0;
    uint32_t slotName = 0;
    Shape* afterA = root->addProperty(arena, heap, symA, slotA);
    Shape* afterB = afterA->addProperty(arena, heap, symB, slotB);
    Shape* afterName = afterB->addProperty(arena, heap, name, slotName);
    CHECK(slotA == 0);
    CHECK(slotB == 1);
    CHECK(slotName == 2);

    PropertyInfo info;
    REQUIRE(afterName->lookupProperty(PropertyKey::fromValue(symA.get()), info));
    CHECK(info.slot == 0);
    REQUIRE(afterName->lookupProperty(PropertyKey::fromValue(symB.get()), info));
    CHECK(info.slot == 1);
    // The STRING "dup" is a third key, and finds neither of the two symbols
    // that describe themselves with the same characters.
    REQUIRE(afterName->lookupProperty(name.get().asString<StringHeader>(), info));
    CHECK(info.slot == 2);
    Rooted<Value> stranger{symbolNamed("dup")};
    CHECK_FALSE(afterName->lookupProperty(PropertyKey::fromValue(stranger.get()), info));

    // The transition is REUSED for the same symbol: two objects that add the
    // same symbol key in the same order share a hidden class.
    uint32_t again = 99;
    CHECK(root->addProperty(arena, heap, symA, again) == afterA);
    CHECK(again == 0);
    // And is NOT reused for a different symbol with the same description.
    uint32_t forked = 99;
    CHECK(root->addProperty(arena, heap, symB, forked) != afterA);

    // The symbol goes into the shape AS IT IS rather than interned into a
    // copy — a copy would be a key nothing could look up again.
    CHECK(afterA->key.symbol() == symA.get().asSymbol<SymbolHeader>());

    const auto keys = afterName->ownKeysInInsertionOrder();
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].isSymbol());
    CHECK(keys[1].isSymbol());
    CHECK(keys[2].isString());
}

TEST_CASE("Symbol keys survive dictionary mode") {
    ShadowStackFrame frame;

    Rooted<Value> obj{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromUndefined())))};
    obj.get().asObject<ObjectHeader>()->header.flags = 0;

    Rooted<Value> symA{symbolNamed("d")};
    Rooted<Value> symB{symbolNamed("d")};
    Rooted<Value> plain{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "plain"))};
    Rooted<Value> one{Value::fromDouble(1.0)};
    Rooted<Value> two{Value::fromDouble(2.0)};
    Rooted<Value> three{Value::fromDouble(3.0)};

    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), symA, one);
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), symB, two);
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), plain, three);

    auto read = [&](Rooted<Value>& key) {
        return obj.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
    };
    CHECK(read(symA).asNumber() == 1.0);
    CHECK(read(symB).asNumber() == 2.0);
    CHECK(read(plain).asNumber() == 3.0);

    // A delete moves the object out of the transition tree and into the entry
    // table, where the same two rules have to hold again.
    CHECK(obj.get().asObject<ObjectHeader>()->deleteProperty(
        rtArena(), plain.get().asString<StringHeader>()));
    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->isDictionary());
    CHECK(read(symA).asNumber() == 1.0);
    CHECK(read(symB).asNumber() == 2.0);
    CHECK(read(plain).isUndefined());

    CHECK(obj.get().asObject<ObjectHeader>()->deleteProperty(
        rtArena(), PropertyKey::fromValue(symA.get())));
    CHECK(read(symA).isUndefined());
    CHECK(read(symB).asNumber() == 2.0);

    // 6.1.7.1: the symbol half comes back separately from the string half, and
    // `rtOwnStringKeysOrdered` never yields a symbol.
    const auto all = rtOwnKeysOrdered(obj.get().asObject<ObjectHeader>(), false);
    REQUIRE(all.size() == 1);
    CHECK(all[0].isSymbol());
    CHECK(rtOwnStringKeysOrdered(obj.get().asObject<ObjectHeader>(), false).empty());
}

TEST_CASE("The symbol registry answers by content and reverses by identity") {
    ShadowStackFrame frame;

    Rooted<Value> key{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "unit-shared"))};
    Rooted<Value> first{rtSymbolFor(key)};
    Rooted<Value> second{rtSymbolFor(key)};
    CHECK(first.get().rawBits() == second.get().rawBits());

    // A separately built string with the same characters reaches the same
    // entry: the lookup going IN is by content.
    Rooted<Value> rebuilt{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "unit-shared"))};
    CHECK(rtSymbolFor(rebuilt).rawBits() == first.get().rawBits());

    Rooted<Value> keyBack{rtSymbolKeyFor(first.get())};
    REQUIRE(keyBack.get().isString());
    CHECK(rtUtf8Chars(keyBack.get().asString<StringHeader>()) == "unit-shared");

    // An unregistered symbol with the SAME description reverses to nothing:
    // coming back out, the registry is searched by identity.
    Rooted<Value> impostor{symbolNamed("unit-shared")};
    CHECK(impostor.get().rawBits() != first.get().rawBits());
    CHECK(rtSymbolKeyFor(impostor.get()).isUndefined());
    CHECK(rtSymbolKeyFor(Value::fromDouble(1.0)).isUndefined());
}

// ---- the well-known symbols -------------------------------------------------
//
// 20.4.2.5 and 20.4.2.14 make `Symbol.iterator` and `Symbol.toStringTag` two
// values in the specification's Well-Known Symbols table, and the table's whole
// content is that each name is ONE value everywhere it appears. Every property
// of a symbol above is identity, so the only thing that could go wrong here is
// a second interning — a reader and a writer of the same hook holding two
// symbols that describe themselves identically and match nothing.
//
// The oracle cases see the consequence (`cases/to_string_tag` pins
// `Symbol.toStringTag === Symbol.toStringTag`); this sees the cause.

TEST_CASE("a well-known symbol is one interned identity, described by its name") {
    ShadowStackFrame frame;

    SymbolHeader* tag = rtSymbolToStringTag();
    REQUIRE(tag != nullptr);
    CHECK(rtSymbolToStringTag() == tag);
    CHECK(rtSymbolIterator() == rtSymbolIterator());
    // Two names, two symbols. They are told apart by ADDRESS, so this is the
    // assertion that would fail if one stood in for the other.
    CHECK(rtSymbolIterator() != tag);

    REQUIRE(tag->description != nullptr);
    CHECK(rtUtf8Chars(tag->description) == "Symbol.toStringTag");
    CHECK(rtUtf8Chars(rtSymbolIterator()->description) == "Symbol.iterator");
    CHECK(rtSymbolDescriptiveString(Value::fromSymbol(tag)) == "Symbol(Symbol.toStringTag)");

    // A symbol built with the same description is a DIFFERENT key, which is
    // what stops a program reaching a well-known hook by spelling it.
    Rooted<Value> impostor{symbolNamed("Symbol.toStringTag")};
    CHECK(impostor.get().asSymbol<SymbolHeader>() != tag);
    CHECK_FALSE(PropertyKey::fromValue(impostor.get())
                    .matches(PropertyKey::fromValue(Value::fromSymbol(tag))));

    // And it is not in the `Symbol.for` registry: 20.4.2.2 answers `undefined`
    // for a well-known symbol, which is the observable half of "the table and
    // the registry are two different tables".
    CHECK(rtSymbolKeyFor(Value::fromSymbol(tag)).isUndefined());
}
