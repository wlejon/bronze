#include <doctest/doctest.h>

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

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena));
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
