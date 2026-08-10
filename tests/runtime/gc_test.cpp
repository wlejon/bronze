#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/gc.h"
#include "runtime/heap.h"

using namespace bronze;

TEST_CASE("heap virtual allocation and low memory address reservation") {
    Heap heap(1024 * 1024 * 1024, 64 * 1024);
    CHECK(heap.base_address() != 0);
    CHECK(heap.base_address() < (1ULL << 47));
    CHECK(heap.reserved_size() == 1024 * 1024 * 1024);
    CHECK(heap.committed_size() >= 64 * 1024);
    CHECK(heap.used_size() == 0);

    auto* obj1 = heap.allocate(16, Tag::Object);
    CHECK(obj1 != nullptr);
    CHECK(obj1->tag == static_cast<uint16_t>(Tag::Object));
    CHECK(obj1->size >= 24);
    CHECK(reinterpret_cast<uintptr_t>(obj1) < (1ULL << 47));

    auto* obj2 = heap.allocate(32, Tag::String);
    CHECK(obj2 != nullptr);
    CHECK(obj2->tag == static_cast<uint16_t>(Tag::String));
    CHECK(reinterpret_cast<uintptr_t>(obj2) < (1ULL << 47));

    CHECK(heap.used_size() > 0);
}

TEST_CASE("bump allocator auto commits pages and triggers collection hook") {
    Heap heap(1024 * 1024, 64 * 1024);
    bool collection_triggered = false;
    heap.set_collection_hook([&](Heap& h) {
        (void)h;
        collection_triggered = true;
    });

    size_t initial_committed = heap.committed_size();

    for (int i = 0; i < 2000; ++i) {
        heap.allocate(64, Tag::Object);
    }

    CHECK(heap.committed_size() > initial_committed);

    heap.collect();
    CHECK(collection_triggered);
}

TEST_CASE("shadow stack frame push pop and top frame nesting") {
    CHECK(ShadowStackFrame::current() == nullptr);

    {
        ShadowStackFrame frame1;
        CHECK(ShadowStackFrame::current() == &frame1);
        CHECK(frame1.prev() == nullptr);
        CHECK(frame1.count() == 0);

        Value v1 = Value::fromDouble(42.0);
        frame1.push(&v1);
        CHECK(frame1.count() == 1);
        CHECK(frame1.roots()[0] == &v1);
        CHECK(*frame1.roots()[0] == Value::fromDouble(42.0));

        {
            ShadowStackFrame frame2;
            CHECK(ShadowStackFrame::current() == &frame2);
            CHECK(frame2.prev() == &frame1);
            CHECK(frame2.count() == 0);

            Value v2 = Value::fromBool(true);
            frame2.push(&v2);
            CHECK(frame2.count() == 1);
            CHECK(frame2.roots()[0] == &v2);

            frame2.pop(&v2);
            CHECK(frame2.count() == 0);
        }

        CHECK(ShadowStackFrame::current() == &frame1);
        frame1.pop(&v1);
        CHECK(frame1.count() == 0);
    }

    CHECK(ShadowStackFrame::current() == nullptr);
}

TEST_CASE("rooted handle scoping and automatic registration") {
    Heap heap;
    ShadowStackFrame frame;

    CHECK(frame.count() == 0);

    {
        Rooted<Value> r1(heap, Value::fromDouble(3.14159));
        CHECK(frame.count() == 1);
        CHECK(r1.get() == Value::fromDouble(3.14159));
        CHECK(*r1 == Value::fromDouble(3.14159));

        {
            Rooted<Value> r2(heap, Value::fromBool(false));
            CHECK(frame.count() == 2);
            CHECK(r2.get().asBool() == false);
        }

        CHECK(frame.count() == 1);
    }

    CHECK(frame.count() == 0);
}

TEST_CASE("rooted handle slot modification and garbage collector mutation simulation") {
    Heap heap;
    ShadowStackFrame frame;

    auto* raw_obj = heap.allocate(16, Tag::Object);
    Value initial_val = Value::fromObject(raw_obj->payload());

    Rooted<Value> root(heap, initial_val);
    CHECK(root.get() == initial_val);
    CHECK(frame.count() == 1);
    CHECK(frame.roots()[0] == root.slot_ptr());

    Value new_val = Value::fromDouble(100.5);
    root.set(new_val);
    CHECK(root.get() == new_val);
    CHECK(*frame.roots()[0] == new_val);

    root = Value::fromBool(true);
    CHECK(root.get() == Value::fromBool(true));
    CHECK(*frame.roots()[0] == Value::fromBool(true));

    auto* new_raw_obj = heap.allocate(32, Tag::Object);
    Value relocated_val = Value::fromObject(new_raw_obj->payload());
    *root.slot_ptr() = relocated_val;

    CHECK(root.get() == relocated_val);
    CHECK(root.get().asObject() == new_raw_obj->payload());
}

TEST_CASE("non moving arena allocation and pointer stability") {
    NonMovingArena arena(4 * 1024);

    struct TestMetadata {
        uint32_t id;
        double weight;
        char name[16];

        TestMetadata(uint32_t i, double w, const char* n) : id(i), weight(w) {
            std::size_t len = 0;
            while (n[len] != '\0' && len < sizeof(name) - 1) {
                name[len] = n[len];
                ++len;
            }
            name[len] = '\0';
        }
    };

    TestMetadata* first = arena.create<TestMetadata>(101, 75.5, "alpha");
    CHECK(first != nullptr);
    CHECK(first->id == 101);
    CHECK(first->weight == 75.5);
    CHECK(std::string(first->name) == "alpha");

    std::vector<TestMetadata*> items;
    items.push_back(first);

    for (uint32_t i = 0; i < 500; ++i) {
        TestMetadata* item = arena.create<TestMetadata>(i, static_cast<double>(i) * 1.5, "test");
        items.push_back(item);
    }

    CHECK(arena.chunk_count() > 1);

    CHECK(items[0] == first);
    CHECK(first->id == 101);
    CHECK(first->weight == 75.5);
    CHECK(std::string(first->name) == "alpha");
}
