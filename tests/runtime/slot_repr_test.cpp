// Stage R1: per-slot representation (src/runtime/slot_repr.h).
//
// The claim under test is narrow and total: a slot a shape calls a double holds
// a double, whatever the program does to it. So every case below is a way of
// contradicting that claim — a string store, a redefinition, a delete, a
// dictionary conversion, a collection — and what it checks is that the claim
// either survived or was withdrawn, never that it was quietly broken.

#include <doctest/doctest.h>

#include <string>

#include "runtime/dictionary.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/slot_repr.h"
#include "runtime/string.h"

using namespace bronze;

namespace {

// The eligible-name registry and the seam are process-wide, and doctest runs
// every case in one process — so a case that registered `x` and left would give
// an unrelated case's `x` a double slot. Every case opens with one of these.
struct ReprScope {
    ReprScope() {
        runtime::slotReprResetForTesting();
        runtime::slotReprSetEnabledForTesting(true);
        runtime::slotReprSetObservesUnpinnedForTesting(false);
    }
    ~ReprScope() {
        runtime::slotReprResetForTesting();
        runtime::slotReprSetEnabledForTesting(true);
        runtime::slotReprSetObservesUnpinnedForTesting(false);
    }
};

// Registers `name` as a pinned-number field, the way
// `bronze_register_slot_repr` does at module init. The generalization rebuild
// allocates from the process arena, so the shapes these cases build have to
// come from there too — a chain half in a local arena and half in the process
// one would be two lifetimes for one layout.
void pin(const char* name) {
    Heap& heap = runtime::rtHeap();
    StringHeader* interned = StringHeader::internToArena(
        runtime::rtArena(), StringHeader::createFromUTF8(heap, name));
    runtime::slotReprRegisterName(interned);
}

Rooted<Value> keyOf(Heap& heap, const char* name) {
    return Rooted<Value>{Value::fromString(StringHeader::createFromUTF8(heap, name))};
}

// A fresh root shape in the process arena, so a case's transition tree is its
// own and `Shape::withSlotBoxed` rebuilds into the same arena it walked.
Shape* freshRoot() { return Shape::createRoot(runtime::rtArena()); }

ObjectHeader* setProp(Heap& heap, Rooted<Value>& self, const char* name, Value v) {
    Rooted<Value> key = keyOf(heap, name);
    Rooted<Value> val{v};
    auto* obj = self.get().asObject<ObjectHeader>();
    ObjectHeader* live = obj->setProp(heap, runtime::rtArena(), key, val);
    self.set(Value::fromObject(live));
    return live;
}

Value getProp(Heap& heap, Rooted<Value>& self, const char* name) {
    Rooted<Value> key = keyOf(heap, name);
    return self.get().asObject<ObjectHeader>()->getProp(heap, key);
}

}  // namespace

TEST_CASE("a pinned field is born a double slot and an unpinned one is not") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    setProp(heap, obj, "y", Value::fromDouble(2.5));

    Shape* shape = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(shape->slotIsDouble(0));
    CHECK_FALSE(shape->slotIsDouble(1));
    CHECK(shape->double_slots == 1u);
    CHECK(shape->parent->repr == SlotRepr::Double);
    CHECK(shape->repr == SlotRepr::Boxed);

    // The stored bits are the number's, and every reader still sees a Number:
    // that bit-compatibility is what lets the storage land with the codegen
    // unchanged (slot_repr.h).
    CHECK(getProp(heap, obj, "x").isNumber());
    CHECK(getProp(heap, obj, "x").asNumber() == 1.5);
    CHECK(obj.get().asObject<ObjectHeader>()->rawSlot(0).rawBits() ==
          Value::fromDouble(1.5).rawBits());
}

TEST_CASE("an eligible name whose first store is not a number stays boxed") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromString(StringHeader::createFromUTF8(heap, "not a number")));
    CHECK_FALSE(obj.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));
}

TEST_CASE("an Int32-tagged number is a number for a double slot") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("n");

    const Value boxedI32 =
        Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32), static_cast<uint32_t>(-7));
    CHECK_FALSE(boxedI32.isNumber());  // the tag is above the number range
    CHECK(slotReprAcceptsValue(boxedI32));

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "n", boxedI32);
    CHECK(obj.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));
    // Stored as the double it denotes, not as the Int32 box: the whole point of
    // the representation is that the eight bytes read back as an f64.
    CHECK(obj.get().asObject<ObjectHeader>()->rawSlot(0).asNumber() == -7.0);
    CHECK(getProp(heap, obj, "n").asNumber() == -7.0);
}

TEST_CASE("a non-number store generalizes one object and leaves its shape-mates alone") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    Shape* root = freshRoot();

    Rooted<Value> a{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(), root))};
    Rooted<Value> b{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(), root))};
    setProp(heap, a, "x", Value::fromDouble(1.25));
    setProp(heap, a, "y", Value::fromDouble(2.0));
    setProp(heap, b, "x", Value::fromDouble(3.75));
    setProp(heap, b, "y", Value::fromDouble(4.0));

    Shape* shared = a.get().asObject<ObjectHeader>()->shape;
    REQUIRE(shared == b.get().asObject<ObjectHeader>()->shape);
    REQUIRE(shared->slotIsDouble(0));

    const uint64_t before = runtime::slotReprCounters().generalizations;
    setProp(heap, a, "x", Value::fromString(StringHeader::createFromUTF8(heap, "hello")));
    CHECK(runtime::slotReprCounters().generalizations == before + 1);

    // `a` moved to a shape whose slot 0 is boxed, and kept every other fact
    // about its layout: same names, same slots, same prototype root.
    Shape* moved = a.get().asObject<ObjectHeader>()->shape;
    CHECK(moved != shared);
    CHECK_FALSE(moved->slotIsDouble(0));
    CHECK(moved->double_slots == 0u);
    CHECK(moved->root == shared->root);
    CHECK(moved->slot_index == shared->slot_index);
    CHECK(getProp(heap, a, "x").isString());
    CHECK(getProp(heap, a, "y").asNumber() == 2.0);

    // `b` is untouched: shape nodes are immutable, so the demotion is a move
    // and not an edit.
    CHECK(b.get().asObject<ObjectHeader>()->shape == shared);
    CHECK(b.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));
    CHECK(getProp(heap, b, "x").asNumber() == 3.75);

    // A number goes back into the generalized slot as a plain boxed store —
    // the object does NOT return to the double shape.
    setProp(heap, a, "x", Value::fromDouble(9.5));
    CHECK(a.get().asObject<ObjectHeader>()->shape == moved);
    CHECK(getProp(heap, a, "x").asNumber() == 9.5);

    // And the demotion is STICKY: the next object to install `x` on this root
    // takes the boxed edge, so a field that turns over splits the tree once.
    Rooted<Value> c{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(), root))};
    setProp(heap, c, "x", Value::fromDouble(6.5));
    CHECK_FALSE(c.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));
}

TEST_CASE("generalizing one double slot keeps the object's other double slots") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    pin("y");
    pin("z");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.0));
    setProp(heap, obj, "y", Value::fromDouble(2.0));
    setProp(heap, obj, "z", Value::fromDouble(3.0));
    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->double_slots == 0b111u);

    setProp(heap, obj, "y", Value::fromNull());
    Shape* moved = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(moved->double_slots == 0b101u);
    CHECK(getProp(heap, obj, "x").asNumber() == 1.0);
    CHECK(getProp(heap, obj, "y").isNull());
    CHECK(getProp(heap, obj, "z").asNumber() == 3.0);
}

TEST_CASE("a double slot past the inline four lives in the overflow block") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    for (const char* n : {"a", "b", "c", "d", "e", "f"}) pin(n);

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    const char* names[] = {"a", "b", "c", "d", "e", "f"};
    for (uint32_t i = 0; i < 6; ++i) {
        setProp(heap, obj, names[i], Value::fromDouble(static_cast<double>(i) + 0.5));
    }
    Shape* shape = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(shape->double_slots == 0b111111u);
    CHECK(shape->slotIsDouble(5));
    CHECK(obj.get().asObject<ObjectHeader>()->overflowCapacity() >= 2);
    for (uint32_t i = 0; i < 6; ++i) {
        CHECK(getProp(heap, obj, names[i]).asNumber() == static_cast<double>(i) + 0.5);
    }

    // Generalizing an OVERFLOW slot moves the object and leaves the inline
    // double slots alone.
    setProp(heap, obj, "f", Value::fromBool(true));
    CHECK(obj.get().asObject<ObjectHeader>()->shape->double_slots == 0b011111u);
    CHECK(getProp(heap, obj, "f").isBool());
    CHECK(getProp(heap, obj, "e").asNumber() == 4.5);
}

TEST_CASE("a collection over a mix of double and boxed slots keeps both") {
    ReprScope scope;
    // A heap of this case's own, so the collection below is over its objects
    // and the verifier's structural passes have a space they can parse.
    Heap heap(4 * 1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    ShadowStackFrame frame;
    for (const char* n : {"x", "y", "z", "w", "q"}) pin(n);

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    // Slots 0..2 are doubles, slot 3 is a heap STRING the collector must
    // forward, slot 4 is a double in the overflow block, slot 5 an object.
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    setProp(heap, obj, "y", Value::fromDouble(2.5));
    setProp(heap, obj, "z", Value::fromDouble(3.5));
    setProp(heap, obj, "s", Value::fromString(StringHeader::createFromUTF8(heap, "survivor")));
    setProp(heap, obj, "q", Value::fromDouble(4.5));
    Rooted<Value> inner{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                               freshRoot()))};
    setProp(heap, inner, "w", Value::fromDouble(5.5));
    setProp(heap, obj, "child", inner.get());

    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->double_slots == 0b10111u);

    // Garbage, so the collection actually copies rather than no-ops.
    for (int i = 0; i < 64; ++i) {
        (void)StringHeader::createFromUTF8(heap, "garbage garbage garbage garbage");
    }
    heap.collect();
    heap.collect();

    CHECK(getProp(heap, obj, "x").asNumber() == 1.5);
    CHECK(getProp(heap, obj, "y").asNumber() == 2.5);
    CHECK(getProp(heap, obj, "z").asNumber() == 3.5);
    CHECK(getProp(heap, obj, "q").asNumber() == 4.5);
    CHECK(getProp(heap, obj, "s").isString());
    Value child = getProp(heap, obj, "child");
    REQUIRE(child.isObject());
    Rooted<Value> childRoot{child};
    CHECK(getProp(heap, childRoot, "w").asNumber() == 5.5);
    // The forwarded child is the object `inner` still names: a double slot that
    // had been TRACED would have been "forwarded" as if its bits were a
    // pointer, and this is where that shows.
    CHECK(child.rawBits() == inner.get().rawBits());
}

TEST_CASE("a double whose bits look like a pointer survives collection intact") {
    ReprScope scope;
    Heap heap(4 * 1024 * 1024, 64 * 1024);
    heap.set_gc_stress(false);
    heap.set_gc_verify(true);
    ShadowStackFrame frame;
    pin("x");

    // A double whose bit pattern is a small integer — exactly the shape of a
    // heap address, and the reason a double slot must not be traced as a Value
    // once stage R2 stops canonicalizing.
    const double aliasing = std::bit_cast<double>(uint64_t{0x0000'1234'5678'9AB0});
    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(aliasing));
    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));

    heap.collect();
    CHECK(obj.get().asObject<ObjectHeader>()->rawSlot(0).rawBits() ==
          uint64_t{0x0000'1234'5678'9AB0});
}

TEST_CASE("dictionary mode drops every representation") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    pin("y");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    setProp(heap, obj, "y", Value::fromDouble(2.5));
    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->double_slots == 0b11u);

    ObjectHeader::toDictionary(runtime::rtArena(), obj);
    Shape* dict = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(dict->isDictionary());
    CHECK(dict->double_slots == 0u);
    CHECK(getProp(heap, obj, "x").asNumber() == 1.5);
    CHECK(getProp(heap, obj, "y").asNumber() == 2.5);

    // A dictionary slot takes anything, with no generalization to perform: the
    // representation went with the shape.
    setProp(heap, obj, "x", Value::fromString(StringHeader::createFromUTF8(heap, "s")));
    CHECK(getProp(heap, obj, "x").isString());
    CHECK(obj.get().asObject<ObjectHeader>()->shape->double_slots == 0u);
}

TEST_CASE("delete takes a double-slot object to a dictionary") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    pin("y");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    setProp(heap, obj, "y", Value::fromDouble(2.5));

    Rooted<Value> keyX = keyOf(heap, "x");
    StringHeader* interned =
        StringHeader::internToArena(runtime::rtArena(), keyX.get().asString<StringHeader>());
    CHECK(obj.get().asObject<ObjectHeader>()->deleteProperty(runtime::rtArena(),
                                                             PropertyKey::forString(interned)));
    Shape* after = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(after->isDictionary());
    CHECK(after->double_slots == 0u);
    CHECK(getProp(heap, obj, "x").isUndefined());
    CHECK(getProp(heap, obj, "y").asNumber() == 2.5);
}

TEST_CASE("defineOwn over a double slot generalizes it like any other store") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    REQUIRE(obj.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));

    Rooted<Value> key = keyOf(heap, "x");
    Rooted<Value> val{Value::fromBool(false)};
    ObjectHeader* live = obj.get().asObject<ObjectHeader>()->setProp(
        heap, runtime::rtArena(), key, val, /*ic=*/nullptr, /*enumerable=*/true,
        /*defineOwn=*/true);
    obj.set(Value::fromObject(live));
    CHECK_FALSE(live->shape->slotIsDouble(0));
    CHECK(getProp(heap, obj, "x").isBool());
}

TEST_CASE("an accessor never takes a double slot") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    pin("g");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    Rooted<Value> key = keyOf(heap, "g");
    Rooted<Value> getter{Value::fromUndefined()};
    Rooted<Value> setter{Value::fromUndefined()};
    ObjectHeader::defineAccessor(heap, runtime::rtArena(), obj, key, getter, setter,
                                 /*enumerable=*/true);
    Shape* shape = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(shape->accessor);
    CHECK(shape->repr == SlotRepr::Boxed);
    // The accessor pair takes slots 1 and 2; only the pinned data slot is a
    // double.
    CHECK(shape->double_slots == 1u);
}

TEST_CASE("the seam turns the whole mechanism off") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");
    runtime::slotReprSetEnabledForTesting(false);

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "x", Value::fromDouble(1.5));
    Shape* shape = obj.get().asObject<ObjectHeader>()->shape;
    CHECK(shape->double_slots == 0u);
    CHECK(shape->repr == SlotRepr::Boxed);
    CHECK_FALSE(shape->hasDoubleSlots());
    CHECK(getProp(heap, obj, "x").asNumber() == 1.5);

    // And nothing to take back: a non-number store is an ordinary slot write.
    const uint64_t before = runtime::slotReprCounters().generalizations;
    setProp(heap, obj, "x", Value::fromNull());
    CHECK(runtime::slotReprCounters().generalizations == before);
    CHECK(obj.get().asObject<ObjectHeader>()->shape == shape);
}

TEST_CASE("the observed-unpinned policy makes every key eligible") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    runtime::slotReprSetObservesUnpinnedForTesting(true);

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    setProp(heap, obj, "nothing_pinned_this", Value::fromDouble(1.5));
    CHECK(obj.get().asObject<ObjectHeader>()->shape->slotIsDouble(0));
}

TEST_CASE("a set-site cache is never filled with a double slot") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    pin("x");

    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                             freshRoot()))};
    Rooted<Value> keyX = keyOf(heap, "x");
    Rooted<Value> keyY = keyOf(heap, "y");
    Rooted<Value> v{Value::fromDouble(1.5)};
    InlineCache icX;
    InlineCache icY;

    ObjectHeader* live = obj.get().asObject<ObjectHeader>()->setProp(heap, runtime::rtArena(),
                                                                     keyX, v, &icX);
    obj.set(Value::fromObject(live));
    live = obj.get().asObject<ObjectHeader>()->setProp(heap, runtime::rtArena(), keyY, v, &icY);
    obj.set(Value::fromObject(live));

    // Generated code's inline store paths consume a set-site entry and store
    // the value's bits unasked, so an entry naming a double slot would be a
    // licence to put a pointer in one.
    CHECK(icX.cached_shape == nullptr);
    CHECK(icY.cached_shape != nullptr);
    CHECK(icY.cached_slot == 1);

    // The write still lands, through the helper.
    CHECK(getProp(heap, obj, "x").asNumber() == 1.5);
    Rooted<Value> v2{Value::fromDouble(7.5)};
    live = obj.get().asObject<ObjectHeader>()->setProp(heap, runtime::rtArena(), keyX, v2, &icX);
    obj.set(Value::fromObject(live));
    CHECK(getProp(heap, obj, "x").asNumber() == 7.5);
    CHECK(icX.cached_shape == nullptr);
}
