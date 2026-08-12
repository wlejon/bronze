#include <doctest/doctest.h>

#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;

TEST_CASE("ObjectHeader property access and inline cache") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    REQUIRE(obj.get() != nullptr);

    Rooted<Value> key_a(Value::fromString(StringHeader::createFromUTF8(heap, "a")));
    Rooted<Value> key_b(Value::fromString(StringHeader::createFromUTF8(heap, "b")));
    Rooted<Value> val_a(Value::fromDouble(10.0));
    Rooted<Value> val_b(Value::fromDouble(20.0));

    InlineCache ic_set_a;
    InlineCache ic_set_b;
    InlineCache ic_get_a;
    InlineCache ic_get_b;

    obj.get()->setProp(heap, arena, key_a, val_a, &ic_set_a);
    obj.get()->setProp(heap, arena, key_b, val_b, &ic_set_b);

    CHECK(ic_set_a.cached_slot == 0);
    CHECK(ic_set_b.cached_slot == 1);

    Value res_a = obj.get()->getProp(heap, key_a, &ic_get_a);
    Value res_b = obj.get()->getProp(heap, key_b, &ic_get_b);

    CHECK(res_a.isNumber());
    CHECK(res_a.asNumber() == 10.0);
    CHECK(res_b.isNumber());
    CHECK(res_b.asNumber() == 20.0);

    CHECK(ic_get_a.cached_shape == obj.get()->shape);
    CHECK(ic_get_a.cached_slot == 0);
    CHECK(ic_get_b.cached_shape == obj.get()->shape);
    CHECK(ic_get_b.cached_slot == 1);

    // Fast path IC hit
    Value res_a_ic = obj.get()->getProp(heap, key_a, &ic_get_a);
    CHECK(res_a_ic.asNumber() == 10.0);
}

TEST_CASE("properties beyond the inline slots spill to the overflow block") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    constexpr uint32_t kProps = 12;
    static_assert(kProps > ObjectHeader::kInlineSlots);

    for (uint32_t i = 0; i < kProps; ++i) {
        std::string name = "p" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        Rooted<Value> val(Value::fromDouble(static_cast<double>(i) * 1.5));
        ObjectHeader* live = obj.get()->setProp(heap, arena, key, val);
        obj = live;
    }

    CHECK(obj.get()->overflowCapacity() >= kProps - ObjectHeader::kInlineSlots);

    for (uint32_t i = 0; i < kProps; ++i) {
        std::string name = "p" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        Value v = obj.get()->getProp(heap, key);
        CHECK(v.isNumber());
        CHECK(v.asNumber() == static_cast<double>(i) * 1.5);
    }

    // Overwrite one out-of-line property in place (no new transition).
    {
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p9")));
        Rooted<Value> val(Value::fromDouble(-4.25));
        obj = obj.get()->setProp(heap, arena, key, val);
        Rooted<Value> key2(Value::fromString(StringHeader::createFromUTF8(heap, "p9")));
        CHECK(obj.get()->getProp(heap, key2).asNumber() == -4.25);
    }

    // Rooted string values stored out-of-line survive an explicit collection.
    {
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "s")));
        Rooted<Value> val(Value::fromString(StringHeader::createFromUTF8(heap, "spilled")));
        obj = obj.get()->setProp(heap, arena, key, val);
        heap.collect();
        Rooted<Value> key2(Value::fromString(StringHeader::createFromUTF8(heap, "s")));
        Value v = obj.get()->getProp(heap, key2);
        REQUIRE(v.isString());
        CHECK(v.asString<StringHeader>()->length == 7);
        CHECK(v.asString<StringHeader>()->charCodeAt(0) == 's');
    }
}

// docs/0032: the receiver's shape cannot see a property added to an object
// BETWEEN the receiver and the holder, so a depth > 1 entry needs the epoch.
// The oracle case pins the behaviour end to end; this pins the mechanism, so
// that a change which quietly stops bumping fails in the module that owns it
// rather than 300 s later in the three.js run.
TEST_CASE("an inline cache entry above depth 1 is invalidated by a prototype add") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    // leaf -> mid -> top, and only `top` has `p`.
    Rooted<ObjectHeader*> top(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> mid(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(top.get()))));
    Rooted<ObjectHeader*> leaf(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(mid.get()))));

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p")));
    Rooted<Value> fromTop(Value::fromDouble(1.0));
    top.get()->setProp(heap, arena, key, fromTop);

    InlineCache ic;
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 1.0);
    // Warm, and at the depth the receiver's shape alone cannot vouch for.
    CHECK(ic.cached_shape == leaf.get()->shape);
    CHECK(ic.cached_depth == 2);
    const uint64_t filledAt = ic.cached_epoch;

    // Shadow it on the INTERMEDIATE. `mid`'s shape changes; `leaf`'s does not,
    // which is exactly why the shape compare is not enough here.
    Shape* leafShapeBefore = leaf.get()->shape;
    Rooted<Value> fromMid(Value::fromDouble(2.0));
    mid.get()->setProp(heap, arena, key, fromMid);
    CHECK(leaf.get()->shape == leafShapeBefore);
    CHECK(protoMutationEpoch() != filledAt);
    CHECK_FALSE(ic.describes(leaf.get()->shape));

    // The warm site must move to the nearer holder, and agree with a cold one.
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 2.0);
    InlineCache cold;
    CHECK(leaf.get()->getProp(heap, key, &cold).asNumber() == 2.0);
    CHECK(ic.cached_depth == 1);
}

// The other half of the same decision: an add to an object that is NOT a
// prototype must not bump, or every proto cache in the program dies whenever
// a loop constructs anything (docs/0032 decision 2 measured 40% on exactly
// that shape).
TEST_CASE("an ordinary object's property add does not disturb proto caches") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> proto(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> inst(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(proto.get()))));

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p")));
    Rooted<Value> val(Value::fromDouble(7.0));
    proto.get()->setProp(heap, arena, key, val);

    InlineCache ic;
    CHECK(inst.get()->getProp(heap, key, &ic).asNumber() == 7.0);
    const uint64_t filledAt = ic.cached_epoch;

    // A brand new object, never anybody's prototype, gaining two properties.
    Rooted<ObjectHeader*> other(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<Value> keyX(Value::fromString(StringHeader::createFromUTF8(heap, "x")));
    Rooted<Value> keyY(Value::fromString(StringHeader::createFromUTF8(heap, "y")));
    Rooted<Value> one(Value::fromDouble(1.0));
    other.get()->setProp(heap, arena, keyX, one);
    other.get()->setProp(heap, arena, keyY, one);

    CHECK(protoMutationEpoch() == filledAt);
    CHECK(ic.describes(inst.get()->shape));
}
