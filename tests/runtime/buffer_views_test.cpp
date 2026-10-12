#include <doctest/doctest.h>

#include <cstdint>
#include <thread>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/typed_array.h"

using namespace bronze;

namespace {

struct LocalShapes {
    NonMovingArena arena;
    Shape* shape = Shape::createRoot(arena, Value::fromNull());
};

bool tracked(Value view) {
    const auto* buf = view.asObject<TypedArrayHeader>()->buffer.asObject<ArrayBufferHeader>();
    return (buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) != 0;
}

uint32_t lengthOf(Rooted<Value>& view) { return view.get().asObject<TypedArrayHeader>()->length; }

// What `transfer` and a host's transfer list do to the buffer behind a view.
void detach(Heap& heap, Rooted<Value>& view) {
    Rooted<Value> buf{view.get().asObject<TypedArrayHeader>()->buffer};
    buf.get().asObject<ArrayBufferHeader>()->setDetached();
    closeOrReopenViews(heap, buf);
}

// Short-lived objects enough to fill the young generation a few times over.
void churn(Heap& heap, LocalShapes& shapes) {
    for (int i = 0; i < 20000; ++i) {
        TypedArrayHeader::create(heap, shapes.shape, shapes.shape, ElementKind::Uint8, 16);
    }
}

}  // namespace

// The view table and the thread's tracking switch are per thread, so the case
// runs on a thread of its own: it starts with no buffer tracked, as a program
// does, whatever the cases before it on the test thread left behind.
TEST_CASE("a thread that detaches a buffer tracks the views of every buffer it makes afterwards") {
    std::thread t([] {
        Heap heap;
        LocalShapes shapes;
        ShadowStackFrame frame;

        // Before any detach nothing is tracked: making views costs no row.
        Rooted<Value> early(Value::fromObject(
            TypedArrayHeader::create(heap, shapes.shape, shapes.shape, ElementKind::Uint8, 8)));
        Rooted<Value> first(Value::fromObject(
            TypedArrayHeader::create(heap, shapes.shape, shapes.shape, ElementKind::Uint8, 4)));
        CHECK_FALSE(tracked(early.get()));
        CHECK_FALSE(tracked(first.get()));

        // The first detach finds the views by walking the heap, and turns tracking on.
        detach(heap, first);
        CHECK(lengthOf(first) == 0);

        // A buffer made now is tracked, with every view over it: the one it was
        // made with, a second at an offset, and a dead one.
        Rooted<Value> later(Value::fromObject(
            TypedArrayHeader::create(heap, shapes.shape, shapes.shape, ElementKind::Float32, 8)));
        CHECK(tracked(later.get()));
        Rooted<Value> laterBuf{later.get().asObject<TypedArrayHeader>()->buffer};
        Rooted<Value> window(Value::fromObject(TypedArrayHeader::createOverBuffer(
            heap, shapes.shape, ElementKind::Uint16, laterBuf, 8, 4)));
        TypedArrayHeader::createOverBuffer(heap, shapes.shape, ElementKind::Uint8, laterBuf, 0, 32);
        CHECK(lengthOf(later) == 8);
        CHECK(lengthOf(window) == 4);

        // Collections move the views and the buffer and reclaim the dead view;
        // the table follows them.
        for (int i = 0; i < 3; ++i) {
            churn(heap, shapes);
            heap.collect();
        }
        CHECK(lengthOf(later) == 8);
        detach(heap, later);
        CHECK(lengthOf(later) == 0);
        CHECK(lengthOf(window) == 0);
        CHECK(later.get().asObject<TypedArrayHeader>()->isOutOfBounds());

        // A buffer made before the switch is still untracked, and its detach
        // still finds its view.
        CHECK_FALSE(tracked(early.get()));
        detach(heap, early);
        CHECK(lengthOf(early) == 0);

        // An external buffer (a host's bytes) made now is tracked too.
        Rooted<Value> ext{Value::fromObject(ArrayBufferHeader::createExternal(heap, shapes.shape, 16))};
        CHECK((ext.get().asObject<ArrayBufferHeader>()->bufferFlags &
               ArrayBufferHeader::kFlagViewsTracked) != 0);
    });
    t.join();
}
