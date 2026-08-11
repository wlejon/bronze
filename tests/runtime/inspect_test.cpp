#include <doctest/doctest.h>

#include <string>

#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;
using bronze::runtime::rtInspect;

// The format docs/0013 pins, exercised below the compiler: the oracle case
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
