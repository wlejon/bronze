#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <thread>

#include "../stack_scrub.h"
#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/external_store.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/native_handle.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/typed_array.h"

using namespace bronze;
using namespace bronze::runtime;

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

TEST_CASE("large ArrayBuffer and Float32Array (> 256 MiB) allocate externally and reclaim on GC") {
    std::thread t([] {
        Heap heap;
        LocalShapes shapes;
        ShadowStackFrame frame;

        // Buffers at or below kMaxInlineByteLength (1 MiB) stay inline on the GC heap.
        {
            Rooted<Value> smallBuf(
                Value::fromObject(ArrayBufferHeader::create(heap, shapes.shape, kMaxInlineByteLength)));
            CHECK_FALSE(smallBuf.get().asObject<ArrayBufferHeader>()->isExternal());
        }
        CHECK(rtExternalStoreCount() == 0);

        // A 10000 x 10000 Float32Array (400 MB > 256 MiB) allocates its backing store
        // externally via calloc, keeping the moving GC heap footprint tiny.
        constexpr uint32_t kSide = 10000;
        constexpr uint32_t kElements = kSide * kSide;
        constexpr uint32_t kBytes = kElements * sizeof(float);
        static_assert(kBytes == 400'000'000u);
        static_assert(kBytes > (1u << 28));
        static_assert(kBytes <= kMaxByteLength);

        const size_t usedBefore = heap.used_size();
        auto viewRoot = bronze::test::heapRooted<Rooted<Value>>();
        bronze::test::inOwnFrame([&] {
            viewRoot->set(Value::fromObject(
                TypedArrayHeader::create(heap, shapes.shape, shapes.shape, ElementKind::Float32, kElements)));
            auto* ta = viewRoot->get().asObject<TypedArrayHeader>();
            auto* buf = ta->buffer.asObject<ArrayBufferHeader>();
            CHECK(ta->length == kElements);
            CHECK(ta->byteLength() == kBytes);
            CHECK(buf->byteLength == kBytes);
            CHECK(buf->isExternal());
            CHECK(rtExternalStoreCount() == 1);
            // At most a few headers more: the 400 MB of external bytes
            // request a collection, which the view's allocation runs, so the
            // heap may also come out smaller.
            CHECK(heap.used_size() < usedBefore + 4096);

            uint8_t* origPtr = buf->data();
            CHECK(ta->get(0) == 0.0);
            CHECK(ta->get(kElements / 2) == 0.0);
            CHECK(ta->get(kElements - 1) == 0.0);

            ta->set(0, 12.5);
            ta->set(kElements / 2, -42.25);
            ta->set(kElements - 1, 9001.75);

            // Survives GC without copying or moving the 400 MB backing store.
            heap.collect();
            ta = viewRoot->get().asObject<TypedArrayHeader>();
            buf = ta->buffer.asObject<ArrayBufferHeader>();
            CHECK(buf->data() == origPtr);
            CHECK(ta->get(0) == 12.5);
            CHECK(ta->get(kElements / 2) == -42.25);
            CHECK(ta->get(kElements - 1) == 9001.75);

            // rtExternalizeArrayBuffer reuses the existing ExternalStore without copying.
            ExternalWindow win = rtExternalizeArrayBuffer(viewRoot->get());
            CHECK(win.bytes == origPtr);
            CHECK(win.byteLength == kBytes);
            rtReleaseExternalStore(win.store);
        });

        viewRoot.reset();
        bronze::test::scrubStack();
        heap.collect();
        rtDrainFinalizers();
        heap.collect();
        rtDrainFinalizers();
        CHECK(rtExternalStoreCount() == 0);
    });
    t.join();
}

// A thread that makes big buffers and drops them, allocating next to nothing
// else on its heap (a Worker decoding photos): the external bytes alone bring
// its collections round, and its stores stay few. Two such threads at once,
// because the trigger is each thread's own: with one count for the process,
// each thread's new bytes never passed what was live, mostly the other's
// uncollected garbage, and neither collected (an image viewer's two decode
// workers grew 30 MB a photo).
TEST_CASE("dropped external buffers bring their own thread's collections round") {
    constexpr uint32_t kBytes = 8u << 20;
    constexpr int kBuffers = 96;  // 768 MB a thread, if none were freed
    auto run = [](size_t& most) {
        Heap heap;
        LocalShapes shapes;
        ShadowStackFrame frame;
        for (int i = 0; i < kBuffers; ++i) {
            bronze::test::inOwnFrame([&] {
                auto* buf = ArrayBufferHeader::create(heap, shapes.shape, kBytes);
                CHECK(buf->isExternal());
            });
            bronze::test::scrubStack();
            // A host's checkpoint between turns runs the deferred finalizers.
            rtDrainFinalizers();
            most = std::max(most, rtExternalStoreCount());
        }
    };
    size_t mostA = 0, mostB = 0;
    std::thread a([&] { run(mostA); });
    std::thread b([&] { run(mostB); });
    a.join();
    b.join();
    // The trigger is 64 MB past what was live: about eight of these, with a
    // few more for the stack's conservative pins.
    CHECK(mostA <= 24);
    CHECK(mostB <= 24);
}

TEST_CASE("allocating above kMaxByteLength throws RangeError") {
    ShadowStackFrame frame;
    Rooted<Value> ctor{rtTypedArrayConstructor("Uint8Array")};
    Rooted<Value> f64Ctor{rtTypedArrayConstructor("Float64Array")};

    Value caught = Value::fromUndefined();
    const bool threwBytes = rtTryCatch(
        [&] {
            RootedBlock args(1);
            args.set(0, Value::fromDouble(static_cast<double>(kMaxByteLength) + 1.0));
            (void)bronze_construct(ctor.get().rawBits(), 1, args.data());
        },
        caught);
    CHECK(threwBytes);

    const bool threwElements = rtTryCatch(
        [&] {
            RootedBlock args(1);
            args.set(0, Value::fromDouble(static_cast<double>(kMaxByteLength / 8 + 1)));
            (void)bronze_construct(f64Ctor.get().rawBits(), 1, args.data());
        },
        caught);
    CHECK(threwElements);
}
