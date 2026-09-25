#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// A hand-built run of `count` Values: a ValueBlock, not an ObjectHeader, so
// the collector traces every word after the header.
HeapObjectHeader* valueBlock(Heap& heap, size_t count) {
    auto* block = heap.allocate(sizeof(Value) * count, Tag::Object);
    block->flags = HeapKind::ValueBlock;
    return block;
}

HeapValue* valuesOf(Value v) { return v.asObject<HeapObjectHeader>()->payload<HeapValue>(); }

}  // namespace

TEST_CASE("heap allocation lands in the low 47-bit address range") {
    Heap heap;
    CHECK(heap.reserved_size() > 0);
    CHECK(heap.used_size() == 0);

    auto* obj1 = valueBlock(heap, 2);
    CHECK(obj1->tag == static_cast<uint16_t>(Tag::Object));
    CHECK(obj1->size == 24);
    CHECK(reinterpret_cast<uintptr_t>(obj1) < (1ULL << 47));
    CHECK(heap.contains(obj1));
    CHECK(heap.is_movable(obj1));

    auto* obj2 = heap.allocate(32, Tag::String);
    CHECK(obj2->tag == static_cast<uint16_t>(Tag::String));
    CHECK(reinterpret_cast<uintptr_t>(obj2) < (1ULL << 47));

    CHECK(heap.used_size() > 0);
}

TEST_CASE("post-collection hooks run inside every collection") {
    Heap heap;
    heap.set_gc_stress(false);
    int runs = 0;
    bool sawFull = false;
    heap.add_post_collection_hook([&] {
        ++runs;
        sawFull = heap.collecting_full();
    });
    heap.collect_minor();
    CHECK(runs == 1);
    CHECK_FALSE(sawFull);
    heap.collect();
    CHECK(runs == 2);
    CHECK(sawFull);
    CHECK(heap.collection_count() == 2);
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

TEST_CASE("rooted handle slot modification") {
    Heap heap;
    ShadowStackFrame frame;

    Value initial_val = Value::fromObject(valueBlock(heap, 2));
    Rooted<Value> root(heap, initial_val);
    CHECK(root.get() == initial_val);
    CHECK(frame.count() == 1);
    CHECK(frame.roots()[0] == root.slot_ptr());

    root.set(Value::fromDouble(100.5));
    CHECK(*frame.roots()[0] == Value::fromDouble(100.5));

    root = Value::fromBool(true);
    CHECK(*frame.roots()[0] == Value::fromBool(true));
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

    std::vector<TestMetadata*> items;
    items.push_back(first);
    for (uint32_t i = 0; i < 500; ++i) {
        items.push_back(arena.create<TestMetadata>(i, static_cast<double>(i) * 1.5, "test"));
    }

    CHECK(arena.chunk_count() > 1);
    CHECK(items[0] == first);
    CHECK(first->weight == 75.5);
    CHECK(std::string(first->name) == "alpha");
}

TEST_CASE("a minor collection moves rooted young objects and a full one keeps them") {
    Heap heap;
    heap.set_gc_stress(false);
    ShadowStackFrame frame;

    (void)StringHeader::createFromUTF8(heap, "unrooted_garbage_string_data");
    Rooted<Value> str(heap, Value::fromString(StringHeader::createFromUTF8(heap, "rooted_surviving_string")));
    Rooted<Value> obj(heap, Value::fromObject(valueBlock(heap, 2)));
    valuesOf(obj.get())[0] = str.get();
    valuesOf(obj.get())[1] = Value::fromDouble(999.888);

    const Value before = obj.get();
    const uint64_t epoch = heap.relocation_epoch();
    heap.collect_minor();
    CHECK(obj.get() != before);
    CHECK(heap.relocation_epoch() != epoch);

    heap.collect();
    CHECK_FALSE(heap.is_movable(obj.get().asObject()));
    auto* s = str.get().asString<StringHeader>();
    CHECK(s->charCodeAt(0) == 'r');
    CHECK(s->length == 23);
    CHECK(valuesOf(obj.get())[0] == str.get());
    CHECK(valuesOf(obj.get())[1].asNumber() == 999.888);
}

TEST_CASE("an old object's store of a young value keeps it alive through a minor collection") {
    Heap heap;
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);  // the missing-barrier check
    ShadowStackFrame frame;

    Rooted<Value> old(heap, Value::fromObject(valueBlock(heap, 4)));
    for (int i = 0; i < 4; ++i) valuesOf(old.get())[i] = Value::fromUndefined();
    heap.collect();
    REQUIRE_FALSE(heap.is_movable(old.get().asObject()));

    // One store through the barrier, and one bulk copy.
    valuesOf(old.get())[0] = Value::fromString(StringHeader::createFromUTF8(heap, "young_one"));
    Value young[2];
    young[0] = Value::fromString(StringHeader::createFromUTF8(heap, "young_two"));
    young[1] = Value::fromDouble(7.0);
    gcCopyValues(old.get().asObject(), valuesOf(old.get()) + 1, young, 2);

    heap.collect_minor();
    heap.collect_minor();
    CHECK(valuesOf(old.get())[0].asString<StringHeader>()->length == 9);
    CHECK(valuesOf(old.get())[1].asString<StringHeader>()->charCodeAt(6) == 't');
    CHECK(valuesOf(old.get())[2].asNumber() == 7.0);
}

TEST_CASE("heap verify passes a clean heap and keeps it live across collections") {
    Heap heap;
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    CHECK(heap.gc_verify());
    ShadowStackFrame frame;

    Rooted<Value> str(heap, Value::fromString(StringHeader::createFromUTF8(heap, "verify_me")));
    Rooted<Value> obj(heap, Value::fromObject(valueBlock(heap, 4)));
    HeapValue* slots = valuesOf(obj.get());
    slots[0] = str.get();
    slots[1] = Value::fromDouble(2.5);
    slots[2] = Value::fromUndefined();
    slots[3] = Value::fromBool(true);

    // A raw-bytes payload is never read as Values, even when its bytes look
    // like them.
    auto* raw_bytes = heap.allocate(32, Tag::RawBytes);
    std::memset(raw_bytes->payload(), 0xFF, 32);
    valuesOf(obj.get())[2] = Value::fromObject(raw_bytes);

    heap.collect_minor();
    heap.collect();
    heap.collect();

    const HeapValue* live = valuesOf(obj.get());
    CHECK(live[0] == str.get());
    CHECK(live[1].asNumber() == 2.5);
    CHECK(live[3].asBool() == true);
    auto* bytes = live[2].asObject<HeapObjectHeader>()->payload<uint8_t>();
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[31] == 0xFF);
}

TEST_CASE("gc stress mode collects at every allocation") {
    Heap heap;
    heap.set_gc_stress(true);
    CHECK(heap.gc_stress() == true);

    ShadowStackFrame frame;
    std::vector<std::unique_ptr<Rooted<Value>>> roots;

    const uint64_t before = heap.collection_count();
    for (int i = 0; i < 30; ++i) {
        std::string text = "stress_string_" + std::to_string(i);
        StringHeader* s = StringHeader::createFromUTF8(heap, text);
        roots.push_back(std::make_unique<Rooted<Value>>(heap, Value::fromString(s)));

        for (int j = 0; j <= i; ++j) {
            std::string expected = "stress_string_" + std::to_string(j);
            auto* hdr = roots[j]->get().asString<StringHeader>();
            CHECK(hdr->length == expected.length());
            CHECK(hdr->charCodeAt(0) == 's');
        }
    }
    CHECK(heap.collection_count() - before >= 30);
}

TEST_CASE("a deep hierarchy survives promotion") {
    Heap heap;
    heap.set_gc_stress(false);
    ShadowStackFrame frame;

    size_t nodeCount = 0;
    std::function<Value(int, int)> build = [&](int branching, int depth) -> Value {
        Rooted<Value> node(heap, Value::fromObject(valueBlock(heap, 5)));
        HeapValue* payload = valuesOf(node.get());
        payload[0] = Value::fromDouble(static_cast<double>(nodeCount++));
        payload[1] = Value::fromDouble(static_cast<double>(depth));
        for (int i = 2; i < 5; ++i) payload[i] = Value::fromNull();
        if (depth > 1) {
            for (int i = 0; i < branching; ++i) {
                Value child = build(branching, depth - 1);
                valuesOf(node.get())[2 + (i % 3)] = child;
            }
        }
        return node.get();
    };

    Rooted<Value> rootNode(heap, Value::fromObject(valueBlock(heap, 5)));
    for (int i = 0; i < 5; ++i) valuesOf(rootNode.get())[i] = Value::fromNull();
    for (int i = 0; i < 4; ++i) {
        Value branch = build(4, 7);
        valuesOf(rootNode.get())[i] = branch;
    }
    CHECK(nodeCount == 21844);

    heap.collect();
    auto* liveHdr = rootNode.get().asObject<HeapObjectHeader>();
    CHECK(liveHdr->tag == static_cast<uint16_t>(Tag::Object));
    const Value firstChild = valuesOf(rootNode.get())[0];
    CHECK(valuesOf(firstChild)[1].asNumber() == 7.0);
}

TEST_CASE("user prototype shapes are reclaimed on GC when prototypes die") {
    Heap& heap = rtHeap();
    NonMovingArena& arena = rtArena();
    ShadowStackFrame frame;
    const bool wasStress = heap.gc_stress();

    const size_t initialUserProtoCount = rtUserPrototypeShapeCount();

    // 1. Create temporary prototype objects and root shapes.
    heap.set_gc_stress(false);
    for (int i = 0; i < 50; ++i) {
        Rooted<Value> proto{Value(bronze_create_object())};
        Shape* shape = rtRootShapeForPrototype(proto.get());
        CHECK(shape != nullptr);
    }
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 50);

    // 2. Run GC. All 50 temporary prototypes are unreferenced and should be reclaimed.
    heap.collect();
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount);

    // 3. A prototype kept alive by a Rooted survives GC, and its shape is preserved.
    Rooted<Value> liveProto{Value(bronze_create_object())};
    Shape* liveShape = rtRootShapeForPrototype(liveProto.get());
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 1);

    heap.collect();
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 1);
    CHECK(rtRootShapeForPrototype(liveProto.get()) == liveShape);

    // 4. A prototype kept alive via an object instance created with it survives GC.
    Rooted<Value> liveInstance;
    {
        Rooted<Value> protoObj{Value(bronze_create_object())};
        Shape* pShape = rtRootShapeForPrototype(protoObj.get());
        liveInstance.set(Value::fromObject(ObjectHeader::create(heap, arena, pShape)));
    }
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 2);

    heap.collect();
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 2);
    auto* instObj = liveInstance.get().asObject<ObjectHeader>();
    CHECK(instObj->shape != nullptr);
    CHECK(instObj->shape->prototypeValue().isObject());

    // 5. Minor collections keep every prototype, however unreachable.
    liveProto.set(Value::fromUndefined());
    heap.collect_minor();
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount + 2);

    // 6. Clear roots and collect again; everything should be swept.
    liveInstance.set(Value::fromUndefined());
    heap.collect();
    CHECK(rtUserPrototypeShapeCount() == initialUserProtoCount);
    heap.set_gc_stress(wasStress);
}
