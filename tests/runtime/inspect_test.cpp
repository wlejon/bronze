#include <doctest/doctest.h>

#include <string>

#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;
using bronze::runtime::rtInspect;

// The pinned inspect format, exercised below the compiler: the oracle case
// covers the whole program, these cover the rules one at a time.

TEST_CASE("primitives inspect as node prints them inside a container") {
    Heap heap;
    ShadowStackFrame frame;

    CHECK(rtInspect(Value::fromDouble(1.5)) == "1.5");
    CHECK(rtInspect(Value::fromDouble(-0.0)) == "-0");
    CHECK(rtInspect(Value::fromBool(true)) == "true");
    CHECK(rtInspect(Value::fromNull()) == "null");
    CHECK(rtInspect(Value::fromUndefined()) == "undefined");

    // A string is QUOTED here; the top-level print path writes it raw.
    Rooted<Value> plain(Value::fromString(StringHeader::createFromUTF8(heap, "hi")));
    CHECK(rtInspect(plain.get()) == "'hi'");
}

TEST_CASE("the quote character is chosen by what the string contains") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> apostrophe(Value::fromString(StringHeader::createFromUTF8(heap, "it's")));
    CHECK(rtInspect(apostrophe.get()) == "\"it's\"");

    Rooted<Value> both(Value::fromString(StringHeader::createFromUTF8(heap, "it's \"x\"")));
    CHECK(rtInspect(both.get()) == "`it's \"x\"`");

    Rooted<Value> control(Value::fromString(StringHeader::createFromUTF8(heap, "a\tb\nc")));
    CHECK(rtInspect(control.get()) == "'a\\tb\\nc'");
}

TEST_CASE("an array prints with one space inside non-empty brackets") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> empty(Value::fromObject(ArrayHeader::create(heap, 0)));
    empty.get().asObject<ArrayHeader>()->header.flags = 1;
    CHECK(rtInspect(empty.get()) == "[]");

    Rooted<Value> arr(Value::fromObject(ArrayHeader::create(heap, 2)));
    arr.get().asObject<ArrayHeader>()->header.flags = 1;
    Rooted<Value> one(Value::fromDouble(1.0));
    Rooted<Value> two(Value::fromDouble(2.0));
    arr.get().asObject<ArrayHeader>()->setElem(heap, 0, one);
    arr.get().asObject<ArrayHeader>()->setElem(heap, 1, two);
    CHECK(rtInspect(arr.get()) == "[ 1, 2 ]");
}

TEST_CASE("a cycle is marked, not followed") {
    // Not a formatting nicety: without the ancestor check this is a hang.
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> obj(Value::fromObject(ObjectHeader::create(heap, arena, Shape::createRoot(arena))));
    obj.get().asObject<ObjectHeader>()->header.flags = 0;
    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "self")));
    Rooted<Value> self(obj.get());
    obj.get().asObject<ObjectHeader>()->setProp(heap, arena, key, self);

    CHECK(rtInspect(obj.get()) == "<ref *1> { self: [Circular *1] }");
}

TEST_CASE("a Map prints its entries with an arrow and a Set prints a list") {
    // The arm this exercises replaced a `default:` that cast a MapHeader to an
    // ObjectHeader and read a shape word that is not there — a segfault, not a
    // wrong answer. The `Ctor(size)` prefix is the one a typed array already
    // prints, and an empty collection keeps it, which is what distinguishes it
    // in output from the `{}` of a property-less object.
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> map{Value::fromObject(MapHeader::create(heap, MapHeader::kMapFlags))};
    CHECK(rtInspect(map.get()) == "Map(0) {}");

    Rooted<Value> ka{Value::fromString(StringHeader::createFromUTF8(heap, "a"))};
    Rooted<Value> one{Value::fromDouble(1.0)};
    MapHeader::set(heap, map, ka, one);
    Rooted<Value> kb{Value::fromString(StringHeader::createFromUTF8(heap, "b"))};
    Rooted<Value> two{Value::fromDouble(2.0)};
    MapHeader::set(heap, map, kb, two);
    CHECK(rtInspect(map.get()) == "Map(2) { 'a' => 1, 'b' => 2 }");

    // A Set is the same layout with no second half to separate, so its entries
    // print as a plain list.
    Rooted<Value> set{Value::fromObject(MapHeader::create(heap, MapHeader::kSetFlags))};
    CHECK(rtInspect(set.get()) == "Set(0) {}");
    Rooted<Value> e1{Value::fromDouble(1.0)};
    Rooted<Value> e2{Value::fromDouble(2.0)};
    MapHeader::set(heap, set, e1, e1);
    MapHeader::set(heap, set, e2, e2);
    CHECK(rtInspect(set.get()) == "Set(2) { 1, 2 }");

    // A deleted entry is tombstoned rather than erased — the table keeps its
    // position so a live iterator's cursor stays meaningful — and the printer
    // must not show one.
    Rooted<Value> gone{Value::fromDouble(1.0)};
    CHECK(MapHeader::remove(heap, set, gone));
    CHECK(rtInspect(set.get()) == "Set(1) { 2 }");
}

TEST_CASE("a Map that contains itself is marked, not followed") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> map{Value::fromObject(MapHeader::create(heap, MapHeader::kMapFlags))};
    Rooted<Value> key{Value::fromString(StringHeader::createFromUTF8(heap, "self"))};
    Rooted<Value> self{map.get()};
    MapHeader::set(heap, map, key, self);

    CHECK(rtInspect(map.get()) == "<ref *1> Map(1) { 'self' => [Circular *1] }");
}

TEST_CASE("a key that is not an identifier is quoted") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> obj(Value::fromObject(ObjectHeader::create(heap, arena, Shape::createRoot(arena))));
    obj.get().asObject<ObjectHeader>()->header.flags = 0;
    Rooted<Value> dashed(Value::fromString(StringHeader::createFromUTF8(heap, "a-b")));
    Rooted<Value> one(Value::fromDouble(1.0));
    obj.get().asObject<ObjectHeader>()->setProp(heap, arena, dashed, one);

    CHECK(rtInspect(obj.get()) == "{ 'a-b': 1 }");
}
