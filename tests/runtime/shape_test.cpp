#include <doctest/doctest.h>

#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"

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

TEST_CASE("Shape::lookupProperty memoizes positive and negative arena and heap string lookups") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Shape* curr = Shape::createRoot(arena);
    CHECK_FALSE(curr->has_symbol_keys);
    uint32_t expectedSlots[60]{};
    SlotRepr expectedReprs[60]{};
    for (int i = 0; i < 60; ++i) {
        const std::string name = "prop_" + std::to_string(i);
        Rooted<Value> k(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        uint32_t slot = 0;
        curr = curr->addProperty(arena, heap, k, slot, /*is_enumerable=*/true,
                                 /*is_accessor=*/false, /*is_writable=*/true,
                                 /*is_configurable=*/true,
                                 (i % 2 == 0) ? SlotRepr::Double : SlotRepr::Boxed);
        expectedSlots[i] = slot;
        expectedReprs[i] = curr->repr;
    }
    CHECK_FALSE(curr->has_symbol_keys);

    for (int round = 0; round < 3; ++round) {
        heap.collect();
        for (int i = 0; i < 60; ++i) {
            const std::string name = "prop_" + std::to_string(i);
            Rooted<Value> heapKey(Value::fromString(StringHeader::createFromUTF8(heap, name)));
            PropertyInfo info;
            REQUIRE(curr->lookupProperty(PropertyKey::forString(heapKey.get().asString<StringHeader>()),
                                         info));
            CHECK(info.slot == expectedSlots[i]);
            CHECK(info.repr == expectedReprs[i]);

            const std::string missName = "miss_" + std::to_string(i);
            Rooted<Value> missKey(Value::fromString(StringHeader::createFromUTF8(heap, missName)));
            CHECK_FALSE(
                curr->lookupProperty(PropertyKey::forString(missKey.get().asString<StringHeader>()), info));
        }
        Rooted<Value> emptyKey(Value::fromString(StringHeader::createFromUTF8(heap, "")));
        PropertyInfo emptyInfo;
        CHECK_FALSE(
            curr->lookupProperty(PropertyKey::forString(emptyKey.get().asString<StringHeader>()), emptyInfo));
    }

    // Generalizing a slot produces a distinct Shape* whose memo answers Boxed.
    Shape* boxed0 = Shape::withSlotBoxed(arena, curr, 0);
    REQUIRE(boxed0 != curr);
    {
        Rooted<Value> k0(Value::fromString(StringHeader::createFromUTF8(heap, "prop_0")));
        PropertyInfo info;
        REQUIRE(boxed0->lookupProperty(PropertyKey::forString(k0.get().asString<StringHeader>()), info));
        CHECK(info.slot == 0);
        CHECK(info.repr == SlotRepr::Boxed);
    }

    // Adding a symbol sets has_symbol_keys on the node and its descendants.
    Rooted<Value> symVal(runtime::rtMakeSymbol(Value::fromUndefined()));
    SymbolHeader* sym = symVal.get().asSymbol<SymbolHeader>();
    uint32_t symSlot = 0;
    Shape* withSym = curr->addProperty(arena, heap, symVal, symSlot);
    CHECK(withSym->has_symbol_keys);
    CHECK_FALSE(curr->has_symbol_keys);

    Rooted<Value> tailKey(Value::fromString(StringHeader::createFromUTF8(heap, "after_sym")));
    uint32_t tailSlot = 0;
    Shape* afterSym = withSym->addProperty(arena, heap, tailKey, tailSlot);
    CHECK(afterSym->has_symbol_keys);
    CHECK(Shape::withSlotBoxed(arena, afterSym, 2)->has_symbol_keys);

    PropertyInfo symInfo;
    REQUIRE(afterSym->lookupProperty(PropertyKey::forSymbol(sym), symInfo));
    CHECK(symInfo.slot == symSlot);

    Shape::forgetLookupMemo();
}
