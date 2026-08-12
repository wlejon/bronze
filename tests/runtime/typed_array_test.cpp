#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/typed_array.h"

using namespace bronze;

TEST_CASE("Float32Array basics: zeroed storage, element access, buffer view") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> view(Value::fromObject(TypedArrayHeader::create(heap, ElementKind::Float32, 8)));
    auto* v = view.get().asObject<TypedArrayHeader>();
    CHECK(v->length == 8);
    CHECK(v->header.flags == 3);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(v->get(i) == 0.0);
    }

    v->set(0, 1.5);
    v->set(7, -3.75);
    CHECK(v->get(0) == 1.5);
    CHECK(v->get(7) == -3.75);

    auto* buf = v->buffer.asObject<ArrayBufferHeader>();
    CHECK(buf->byteLength == 32);
    CHECK(buf->header.flags == 4);
}

TEST_CASE("RawBytes buffers survive collection and are never scanned as Values") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> view(Value::fromObject(TypedArrayHeader::create(heap, ElementKind::Float32, 4)));

    // Plant float bit patterns whose containing 64-bit word looks exactly
    // like an Object-tagged NaN-boxed pointer (upper half 0xFFF1xxxx). If
    // the collector ever scanned this payload as Values it would "forward"
    // these bytes and corrupt them.
    {
        auto* v = view.get().asObject<TypedArrayHeader>();
        const uint32_t planted[4] = {0x00001234u, 0xFFF10000u, 0xDEADBEEFu, 0xFFF2CAFEu};
        std::memcpy(v->bytes(), planted, sizeof planted);
    }

    heap.collect();

    auto* v = view.get().asObject<TypedArrayHeader>();
    uint32_t read[4] = {};
    std::memcpy(read, v->bytes(), sizeof read);
    CHECK(read[0] == 0x00001234u);
    CHECK(read[1] == 0xFFF10000u);
    CHECK(read[2] == 0xDEADBEEFu);
    CHECK(read[3] == 0xFFF2CAFEu);
    CHECK(v->length == 4);
    CHECK(v->buffer.asObject<ArrayBufferHeader>()->byteLength == 16);
}

TEST_CASE("the element-kind table is the one pinned by ECMA-262 table 71") {
    struct Expect {
        ElementKind kind;
        const char* name;
        uint32_t bytes;
    };
    const Expect expected[] = {
        {ElementKind::Int8, "Int8Array", 1},
        {ElementKind::Uint8, "Uint8Array", 1},
        {ElementKind::Uint8Clamped, "Uint8ClampedArray", 1},
        {ElementKind::Int16, "Int16Array", 2},
        {ElementKind::Uint16, "Uint16Array", 2},
        {ElementKind::Int32, "Int32Array", 4},
        {ElementKind::Uint32, "Uint32Array", 4},
        {ElementKind::Float32, "Float32Array", 4},
        {ElementKind::Float64, "Float64Array", 8},
    };
    for (const Expect& e : expected) {
        CHECK(std::string(elementKindInfo(e.kind).name) == e.name);
        CHECK(elementKindInfo(e.kind).bytesPerElement == e.bytes);
    }
}

// 7.1.6..7.1.11. Every value below is the spec's answer, computed from the
// algorithm and not from an engine: truncate towards zero, then modulo 2^N,
// then re-sign for the signed kinds; clamp-and-round-half-to-even for
// Uint8Clamped.
TEST_CASE("store conversions at the boundaries") {
    CHECK(convertForStore(ElementKind::Int8, -1) == -1);
    CHECK(convertForStore(ElementKind::Int8, 128) == -128);
    CHECK(convertForStore(ElementKind::Int8, 255) == -1);
    CHECK(convertForStore(ElementKind::Int8, 256) == 0);
    CHECK(convertForStore(ElementKind::Int8, 0.5) == 0);
    CHECK(convertForStore(ElementKind::Int8, -0.5) == 0);
    CHECK(!std::signbit(convertForStore(ElementKind::Int8, -0.5)));
    CHECK(convertForStore(ElementKind::Int8, std::nan("")) == 0);
    CHECK(convertForStore(ElementKind::Int8, 1e40) == 0);

    CHECK(convertForStore(ElementKind::Uint8, -1) == 255);
    CHECK(convertForStore(ElementKind::Uint8, 256) == 0);
    CHECK(convertForStore(ElementKind::Uint8, 257.9) == 1);

    // ToUint8Clamp saturates and rounds half to EVEN — 0.5 down, 1.5 up,
    // 2.5 down. That is the one place JS does not round half away from zero.
    CHECK(convertForStore(ElementKind::Uint8Clamped, -1) == 0);
    CHECK(convertForStore(ElementKind::Uint8Clamped, 256) == 255);
    CHECK(convertForStore(ElementKind::Uint8Clamped, std::nan("")) == 0);
    CHECK(convertForStore(ElementKind::Uint8Clamped, 0.5) == 0);
    CHECK(convertForStore(ElementKind::Uint8Clamped, 1.5) == 2);
    CHECK(convertForStore(ElementKind::Uint8Clamped, 2.5) == 2);
    CHECK(convertForStore(ElementKind::Uint8Clamped, 2.6) == 3);

    CHECK(convertForStore(ElementKind::Int16, 32768) == -32768);
    CHECK(convertForStore(ElementKind::Uint16, -1) == 65535);
    CHECK(convertForStore(ElementKind::Int32, 2147483648.0) == -2147483648.0);
    CHECK(convertForStore(ElementKind::Uint32, -1) == 4294967295.0);
    CHECK(convertForStore(ElementKind::Uint32, 4294967296.0) == 0);

    // The float kinds narrow rather than wrap; 0.1 is the classic witness.
    CHECK(convertForStore(ElementKind::Float32, 0.1) == 0.10000000149011612);
    CHECK(convertForStore(ElementKind::Float64, 0.1) == 0.1);
    CHECK(std::isnan(convertForStore(ElementKind::Float32, std::nan(""))));
    CHECK(std::isinf(convertForStore(ElementKind::Float32, 1e40)));
}

TEST_CASE("two views over one buffer see each other's bytes") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> buffer(Value::fromObject(ArrayBufferHeader::create(heap, 4)));
    Rooted<Value> f32(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Float32, buffer, 0, 1)));
    Rooted<Value> u32(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Uint32, buffer, 0, 1)));

    f32.get().asObject<TypedArrayHeader>()->set(0, 1.0f);
    // The IEEE-754 single-precision encoding of 1.0.
    CHECK(u32.get().asObject<TypedArrayHeader>()->get(0) == 0x3F800000u);

    // The same in reverse, and across a collection that moves the buffer the
    // two of them share: a view holds an OFFSET, never a data pointer, so
    // both are still looking at the same bytes afterwards.
    heap.collect();
    u32.get().asObject<TypedArrayHeader>()->set(0, 0xC0000000u);
    CHECK(f32.get().asObject<TypedArrayHeader>()->get(0) == -2.0);
    CHECK(f32.get().asObject<TypedArrayHeader>()->buffer.rawBits() ==
          u32.get().asObject<TypedArrayHeader>()->buffer.rawBits());
}

TEST_CASE("a view at a byte offset addresses only its own window") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> buffer(Value::fromObject(ArrayBufferHeader::create(heap, 8)));
    Rooted<Value> low(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Uint8, buffer, 0, 4)));
    Rooted<Value> high(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Uint8, buffer, 4, 4)));

    for (uint32_t i = 0; i < 4; ++i) low.get().asObject<TypedArrayHeader>()->set(i, i + 1);
    for (uint32_t i = 0; i < 4; ++i) high.get().asObject<TypedArrayHeader>()->set(i, 100 + i);

    heap.collect();

    Rooted<Value> whole(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Uint8, buffer, 0, 8)));
    auto* w = whole.get().asObject<TypedArrayHeader>();
    CHECK(w->get(0) == 1);
    CHECK(w->get(3) == 4);
    CHECK(w->get(4) == 100);
    CHECK(w->get(7) == 103);
    CHECK(high.get().asObject<TypedArrayHeader>()->byteOffset == 4);
    CHECK(high.get().asObject<TypedArrayHeader>()->byteLength() == 4);
}

TEST_CASE("a zero-length buffer is a real object with a real header") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> buffer(Value::fromObject(ArrayBufferHeader::create(heap, 0)));
    CHECK(buffer.get().asObject<ArrayBufferHeader>()->byteLength == 0);
    Rooted<Value> view(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Float64, buffer, 0, 0)));
    CHECK(view.get().asObject<TypedArrayHeader>()->length == 0);
    heap.collect();
    CHECK(view.get().asObject<TypedArrayHeader>()->buffer.asObject<ArrayBufferHeader>()
              ->byteLength == 0);
}
