#include <doctest/doctest.h>

#include <bit>
#include <cstdint>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/typed_array.h"

using namespace bronze;

TEST_CASE("Float32Array basics: zeroed storage, element access, buffer view") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> view(Value::fromObject(Float32ArrayHeader::create(heap, 8)));
    auto* v = view.get().asObject<Float32ArrayHeader>();
    CHECK(v->length == 8);
    CHECK(v->header.flags == 3);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(v->data()[i] == 0.0f);
    }

    v->data()[0] = 1.5f;
    v->data()[7] = -3.75f;
    CHECK(v->data()[0] == 1.5f);
    CHECK(v->data()[7] == -3.75f);

    auto* buf = v->buffer.asObject<ArrayBufferHeader>();
    CHECK(buf->byteLength == 32);
    CHECK(buf->header.flags == 4);
}

TEST_CASE("RawBytes buffers survive collection and are never scanned as Values") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> view(Value::fromObject(Float32ArrayHeader::create(heap, 4)));

    // Plant float bit patterns whose containing 64-bit word looks exactly
    // like an Object-tagged NaN-boxed pointer (upper half 0xFFF1xxxx). If
    // the collector ever scanned this payload as Values it would "forward"
    // these bytes and corrupt them.
    {
        auto* v = view.get().asObject<Float32ArrayHeader>();
        v->data()[0] = std::bit_cast<float>(0x00001234u);
        v->data()[1] = std::bit_cast<float>(0xFFF10000u);
        v->data()[2] = std::bit_cast<float>(0xDEADBEEFu);
        v->data()[3] = std::bit_cast<float>(0xFFF2CAFEu);
    }

    heap.collect();

    auto* v = view.get().asObject<Float32ArrayHeader>();
    CHECK(std::bit_cast<uint32_t>(v->data()[0]) == 0x00001234u);
    CHECK(std::bit_cast<uint32_t>(v->data()[1]) == 0xFFF10000u);
    CHECK(std::bit_cast<uint32_t>(v->data()[2]) == 0xDEADBEEFu);
    CHECK(std::bit_cast<uint32_t>(v->data()[3]) == 0xFFF2CAFEu);
    CHECK(v->length == 4);
    CHECK(v->buffer.asObject<ArrayBufferHeader>()->byteLength == 16);
}
