// Stable layouts (shape.h `watched`, runtime/shape_leave.h): which moves away
// from a watched shape bump the layout epoch, which do not, and how the watched
// bit spreads down the transition tree. docs/aot-specialization.md, "S1".

#include <doctest/doctest.h>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/slot_repr.h"
#include "runtime/string.h"

using namespace bronze;

namespace {

struct ReprScope {
    ReprScope() { runtime::slotReprResetForTesting(); }
    ~ReprScope() { runtime::slotReprResetForTesting(); }
};

void setProp(Heap& heap, Rooted<Value>& self, const char* name, Value v) {
    Rooted<Value> key{Value::fromString(StringHeader::createFromUTF8(heap, name))};
    Rooted<Value> val{v};
    ObjectHeader* live = self.get().asObject<ObjectHeader>()->setProp(heap, runtime::rtArena(), key, val);
    self.set(Value::fromObject(live));
}

PropertyKey keyNamed(Heap& heap, const char* name) {
    return PropertyKey::forString(
        StringHeader::internToArena(runtime::rtArena(), StringHeader::createFromUTF8(heap, name)));
}

Shape* shapeOf(Rooted<Value>& v) { return v.get().asObject<ObjectHeader>()->shape; }

// A fresh object at {a, b} on a root of its own.
Rooted<Value> objectAB(Heap& heap, Shape* root) {
    Rooted<Value> o{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(), root))};
    setProp(heap, o, "a", Value::fromDouble(1.0));
    setProp(heap, o, "b", Value::fromDouble(2.0));
    return o;
}

}  // namespace

TEST_CASE("watching a shape marks its subtree and every child created under it later") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    Shape* root = Shape::createRoot(runtime::rtArena());
    Rooted<Value> o = objectAB(heap, root);
    Shape* ab = shapeOf(o);
    Shape* a = ab->parent;
    setProp(heap, o, "c", Value::fromDouble(3.0));
    Shape* abc = shapeOf(o);

    Shape::watch(a);
    CHECK(a->watched);
    CHECK(ab->watched);
    CHECK(abc->watched);
    CHECK_FALSE(root->watched);

    // A new edge below a watched node is born watched; one beside it is not.
    Rooted<Value> p = objectAB(heap, root);
    setProp(heap, p, "d", Value::fromDouble(4.0));
    CHECK(shapeOf(p)->parent == ab);
    CHECK(shapeOf(p)->watched);
    Rooted<Value> q{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(), root))};
    setProp(heap, q, "z", Value::fromDouble(0.0));
    CHECK_FALSE(shapeOf(q)->watched);
}

TEST_CASE("an append from a watched shape keeps the layout epoch, a delete moves it") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    Rooted<Value> o = objectAB(heap, Shape::createRoot(runtime::rtArena()));
    Shape::watch(shapeOf(o));

    const uint64_t e0 = protoMutationEpoch();
    setProp(heap, o, "c", Value::fromDouble(3.0));
    CHECK(protoMutationEpoch() == e0);
    CHECK(shapeOf(o)->watched);

    CHECK(o.get().asObject<ObjectHeader>()->deleteProperty(runtime::rtArena(), keyNamed(heap, "a")));
    CHECK(shapeOf(o)->isDictionary());
    CHECK(protoMutationEpoch() != e0);
}

TEST_CASE("breaking an unwatched layout leaves the epoch alone") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    Rooted<Value> o = objectAB(heap, Shape::createRoot(runtime::rtArena()));
    REQUIRE_FALSE(shapeOf(o)->watched);

    const uint64_t e0 = protoMutationEpoch();
    CHECK(o.get().asObject<ObjectHeader>()->deleteProperty(runtime::rtArena(), keyNamed(heap, "a")));
    Rooted<Value> f = objectAB(heap, Shape::createRoot(runtime::rtArena()));
    runtime::rtFreezeObject(f.get());
    CHECK(shapeOf(f)->isDictionary());
    CHECK(protoMutationEpoch() == e0);
}

TEST_CASE("freezing an object at a watched shape moves the epoch") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    Rooted<Value> o = objectAB(heap, Shape::createRoot(runtime::rtArena()));
    Shape::watch(shapeOf(o));
    const uint64_t e0 = protoMutationEpoch();
    runtime::rtFreezeObject(o.get());
    CHECK(shapeOf(o)->isDictionary());
    CHECK(protoMutationEpoch() != e0);
}

TEST_CASE("generalizing a double slot of a watched shape moves the epoch") {
    ReprScope scope;
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    runtime::slotReprRegisterName(
        StringHeader::internToArena(runtime::rtArena(), StringHeader::createFromUTF8(heap, "x")));
    Rooted<Value> o{Value::fromObject(ObjectHeader::create(heap, runtime::rtArena(),
                                                           Shape::createRoot(runtime::rtArena())))};
    setProp(heap, o, "x", Value::fromDouble(1.25));
    Shape* doubled = shapeOf(o);
    REQUIRE(doubled->slotIsDouble(0));
    Shape::watch(doubled);

    const uint64_t e0 = protoMutationEpoch();
    setProp(heap, o, "x", Value::fromDouble(2.5));  // a number: the slot holds
    CHECK(protoMutationEpoch() == e0);
    setProp(heap, o, "x", Value::fromString(StringHeader::createFromUTF8(heap, "s")));
    CHECK(shapeOf(o) != doubled);
    CHECK(protoMutationEpoch() != e0);
}

TEST_CASE("an object at a watched shape becoming a prototype moves the epoch") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    Rooted<Value> o = objectAB(heap, Shape::createRoot(runtime::rtArena()));
    Shape* ab = shapeOf(o);
    Shape::watch(ab);
    const uint64_t e0 = protoMutationEpoch();
    Shape::markObjectAsPrototype(runtime::rtArena(), o.get().asObject<ObjectHeader>());
    CHECK(shapeOf(o) != ab);
    CHECK(protoMutationEpoch() != e0);
}
