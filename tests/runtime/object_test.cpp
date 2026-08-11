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
