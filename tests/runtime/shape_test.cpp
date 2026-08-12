#include <doctest/doctest.h>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;

TEST_CASE("Shape property addition and transitions") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Shape* root = Shape::createRoot(arena);
    REQUIRE(root != nullptr);
    CHECK(root->parent == nullptr);
    CHECK_FALSE(root->key.valid());

    Rooted<Value> key_a(Value::fromString(StringHeader::createFromUTF8(heap, "a")));
    Rooted<Value> key_b(Value::fromString(StringHeader::createFromUTF8(heap, "b")));

    uint32_t slot_a = 999;
    Shape* shape_a = root->addProperty(arena, heap, key_a, slot_a);
    CHECK(slot_a == 0);
    CHECK(shape_a->parent == root);

    uint32_t slot_b = 999;
    Shape* shape_ab = shape_a->addProperty(arena, heap, key_b, slot_b);
    CHECK(slot_b == 1);
    CHECK(shape_ab->parent == shape_a);

    // Verify lookup
    uint32_t found_slot = 999;
    StringHeader* str_a = key_a.get().asString<StringHeader>();
    StringHeader* str_b = key_b.get().asString<StringHeader>();

    CHECK(shape_ab->lookupProperty(str_a, found_slot));
    CHECK(found_slot == 0);

    CHECK(shape_ab->lookupProperty(str_b, found_slot));
    CHECK(found_slot == 1);

    Rooted<Value> key_c(Value::fromString(StringHeader::createFromUTF8(heap, "c")));
    StringHeader* str_c = key_c.get().asString<StringHeader>();
    CHECK_FALSE(shape_ab->lookupProperty(str_c, found_slot));

    // Verify transition caching in shape_a
    uint32_t slot_b_cached = 999;
    Shape* shape_ab2 = shape_a->addProperty(arena, heap, key_b, slot_b_cached);
    CHECK(shape_ab2 == shape_ab);
    CHECK(slot_b_cached == 1);
}
