#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <stdexcept>

#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"

using namespace bronze;

namespace {
struct FatalGuard {
    FatalGuard() {
        setFatalHandler([](const char* msg) { throw std::runtime_error(msg); });
    }
    ~FatalGuard() { setFatalHandler(nullptr); }
};
}  // namespace

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
    // This case measures bump-pointer commit growth; per-allocation stress
    // collection would reclaim the (deliberately unrooted) garbage before
    // the bump pointer ever crosses a page boundary.
    heap.set_gc_stress(false);
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

TEST_CASE("semispace copying collection reclaims unrooted memory and relocates rooted objects") {
    Heap heap(1024 * 1024, 64 * 1024);
    // The unrooted string must still be on the heap when collect() runs;
    // stress mode would reclaim it during the later setup allocations.
    heap.set_gc_stress(false);
    ShadowStackFrame frame;

    StringHeader* unrooted_str = StringHeader::createFromUTF8(heap, "unrooted_garbage_string_data");
    CHECK(unrooted_str != nullptr);

    Rooted<Value> rootS2(heap, Value::fromString(StringHeader::createFromUTF8(heap, "rooted_surviving_string")));
    CHECK(rootS2.get().isString());

    auto* raw_obj = heap.allocate(sizeof(Value) * 2, Tag::Object);
    Value* slots = raw_obj->payload<Value>();
    slots[0] = rootS2.get();
    slots[1] = Value::fromDouble(999.888);

    // A heap reference in a Value always points at the object's HEADER.
    Rooted<Value> rootObj(heap, Value::fromObject(raw_obj));
    CHECK(rootObj.get().isObject());

    size_t used_before = heap.used_size();
    heap.collect();
    size_t used_after = heap.used_size();

    CHECK(used_after < used_before);

    CHECK(rootS2.get().isString());
    auto* s2_relocated = rootS2.get().asString<StringHeader>();
    CHECK(s2_relocated != nullptr);
    CHECK(s2_relocated->charCodeAt(0) == 'r');
    CHECK(s2_relocated->length == 23);

    CHECK(rootObj.get().isObject());
    Value* relocated_slots = rootObj.get().asObject<HeapObjectHeader>()->payload<Value>();
    CHECK(relocated_slots[0].isString());
    CHECK(relocated_slots[0] == rootS2.get());
    CHECK(relocated_slots[1].isNumber());
    CHECK(relocated_slots[1].asNumber() == 999.888);
}

TEST_CASE("heap verify passes a clean heap and keeps it live across collections") {
    Heap heap(1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    CHECK(heap.gc_verify());
    ShadowStackFrame frame;
    FatalGuard guard;

    Rooted<Value> str(heap, Value::fromString(StringHeader::createFromUTF8(heap, "verify_me")));

    auto* raw_obj = heap.allocate(sizeof(Value) * 4, Tag::Object);
    Value* slots = raw_obj->payload<Value>();
    slots[0] = str.get();
    slots[1] = Value::fromDouble(2.5);
    slots[2] = Value::fromUndefined();
    slots[3] = Value::fromBool(true);
    Rooted<Value> obj(heap, Value::fromObject(raw_obj));

    // Raw-bytes payloads are exempt from the word check even when their bytes
    // happen to look like Values — the scan never reads them either.
    auto* raw_bytes = heap.allocate(32, Tag::RawBytes);
    std::memset(raw_bytes->payload(), 0xDB, 32);
    slots = obj.get().asObject<HeapObjectHeader>()->payload<Value>();
    slots[2] = Value::fromObject(raw_bytes);

    CHECK_NOTHROW(heap.collect());
    CHECK_NOTHROW(heap.collect());

    Value* relocated = obj.get().asObject<HeapObjectHeader>()->payload<Value>();
    CHECK(relocated[0] == str.get());
    CHECK(relocated[1].asNumber() == 2.5);
    CHECK(relocated[3].asBool() == true);
}

TEST_CASE("heap verify names a scanned word holding a stale semispace pointer") {
    Heap heap(1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    ShadowStackFrame frame;
    FatalGuard guard;

    // A zeroed raw-bytes block: an address inside it is inside the heap but
    // can never be a live object header, which is exactly what recycled
    // residue in an unzeroed scanned word looks like.
    auto* decoy = heap.allocate(64, Tag::RawBytes);
    std::memset(decoy->payload(), 0, 64);

    auto* raw_obj = heap.allocate(sizeof(Value) * 2, Tag::Object);
    Value* slots = raw_obj->payload<Value>();
    slots[0] = Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Object),
                                        reinterpret_cast<uint64_t>(decoy->payload()) + 8);
    slots[1] = Value::fromDouble(1.0);
    Rooted<Value> obj(heap, Value::fromObject(raw_obj));

    CHECK_THROWS_WITH_AS(heap.collect(), doctest::Contains("heap verify"), std::runtime_error);
}

TEST_CASE("heap verify rejects a word carrying an undefined tag") {
    Heap heap(1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    ShadowStackFrame frame;
    FatalGuard guard;

    auto* raw_obj = heap.allocate(sizeof(Value) * 2, Tag::Object);
    Value* slots = raw_obj->payload<Value>();
    slots[0] = Value::fromRawBits((0xFFFCULL << 48) | 0x1234);
    slots[1] = Value::fromDouble(1.0);
    Rooted<Value> obj(heap, Value::fromObject(raw_obj));

    CHECK_THROWS_WITH_AS(heap.collect(),
                         doctest::Contains("tag the value model does not define"),
                         std::runtime_error);
}

TEST_CASE("gc stress mode triggers collection on every allocation") {
    Heap heap(2 * 1024 * 1024, 128 * 1024);
    heap.set_gc_stress(true);
    CHECK(heap.gc_stress() == true);

    ShadowStackFrame frame;
    std::vector<std::unique_ptr<Rooted<Value>>> roots;

    for (int i = 0; i < 30; ++i) {
        std::string text = "stress_string_" + std::to_string(i);
        StringHeader* s = StringHeader::createFromUTF8(heap, text);
        roots.push_back(std::make_unique<Rooted<Value>>(heap, Value::fromString(s)));

        for (int j = 0; j <= i; ++j) {
            std::string expected = "stress_string_" + std::to_string(j);
            Value val = roots[j]->get();
            CHECK(val.isString());
            auto* hdr = val.asString<StringHeader>();
            CHECK(hdr != nullptr);
            CHECK(hdr->length == expected.length());
            CHECK(hdr->charCodeAt(0) == 's');
        }
    }
}

TEST_CASE("heap dynamically scales for deep hierarchy without bad_alloc") {
    Heap heap(1024 * 1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    ShadowStackFrame frame;

    // Build a 21,844 node hierarchy tree (branching=4, depth=7)
    struct Node {
        HeapObjectHeader* obj;
    };
    std::vector<Rooted<Value>*> allRoots;
    allRoots.reserve(21844);

    Rooted<Value> rootNode(heap, Value::fromNull());
    size_t nodeCount = 0;

    std::function<Value(int, int)> build = [&](int branching, int depth) -> Value {
        auto* raw = heap.allocate(sizeof(Value) * 5, Tag::Object);
        raw->flags = HeapKind::Plain;
        Value* payload = raw->payload<Value>();
        payload[0] = Value::fromDouble(static_cast<double>(nodeCount++));
        payload[1] = Value::fromDouble(static_cast<double>(depth));
        payload[2] = Value::fromNull();
        payload[3] = Value::fromNull();
        payload[4] = Value::fromNull();

        if (depth > 1) {
            for (int i = 0; i < branching; ++i) {
                Value child = build(branching, depth - 1);
                payload[2 + (i % 3)] = child;
            }
        }
        return Value::fromObject(raw);
    };

    auto* sceneRaw = heap.allocate(sizeof(Value) * 5, Tag::Object);
    sceneRaw->flags = HeapKind::Plain;
    rootNode.set(Value::fromObject(sceneRaw));
    for (int i = 0; i < 4; ++i) {
        Value branch = build(4, 7);
        sceneRaw->payload<Value>()[i] = branch;
    }
    CHECK(nodeCount == 21844);

    // Collect to verify all surviving objects copy cleanly
    heap.collect();
    CHECK(rootNode.get().isObject());
    auto* liveHdr = rootNode.get().asObject<HeapObjectHeader>();
    CHECK(liveHdr != nullptr);
    CHECK(liveHdr->tag == static_cast<uint16_t>(Tag::Object));
}

