#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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

TEST_CASE("a shared buffer is the same storage with a different brand") {
    Heap heap;
    ShadowStackFrame frame;

    // 25.2's SharedArrayBuffer is `kFlagShared` on the ordinary buffer header,
    // so the brand has to be readable off the header and must NOT be readable
    // off a plain one — that predicate is what splits the two member surfaces.
    Rooted<Value> fixed(Value::fromObject(ArrayBufferHeader::createShared(heap, 8, 8)));
    auto* f = fixed.get().asObject<ArrayBufferHeader>();
    CHECK(f->header.flags == ArrayBufferHeader::kFlags);
    CHECK(f->isShared());
    CHECK_FALSE(f->isResizable());  // not growable: max == length
    CHECK_FALSE(f->isDetached());
    CHECK(f->byteLength == 8);
    CHECK(f->maxByteLength == 8);
    for (uint32_t i = 0; i < 8; ++i) {
        CHECK(f->data()[i] == 0);
    }

    Rooted<Value> plain(Value::fromObject(ArrayBufferHeader::create(heap, 8)));
    CHECK_FALSE(plain.get().asObject<ArrayBufferHeader>()->isShared());

    // A GROWABLE one reserves its maximum up front, for the reason a resizable
    // ArrayBuffer does: a view holds a byte offset into this block, and a
    // moving collector cannot reallocate it under the view.
    Rooted<Value> growable(Value::fromObject(ArrayBufferHeader::createShared(heap, 4, 12)));
    auto* g = growable.get().asObject<ArrayBufferHeader>();
    CHECK(g->isShared());
    CHECK(g->isResizable());
    CHECK(g->byteLength == 4);
    CHECK(g->maxByteLength == 12);
    for (uint32_t i = 0; i < 12; ++i) {
        CHECK(g->data()[i] == 0);
    }

    // And the brand survives a collection, which a flag word in the header only
    // does if the copy phase copies the whole header rather than rebuilding it.
    g->data()[11] = 0x5A;
    heap.collect();
    auto* moved = growable.get().asObject<ArrayBufferHeader>();
    CHECK(moved->isShared());
    CHECK(moved->isResizable());
    CHECK(moved->maxByteLength == 12);
    CHECK(moved->data()[11] == 0x5A);
    CHECK(fixed.get().asObject<ArrayBufferHeader>()->isShared());
    CHECK_FALSE(plain.get().asObject<ArrayBufferHeader>()->isShared());
}

TEST_CASE("binary16 rounds to nearest even and is computed from the double") {
    // IEEE 754 binary16 has an 11-bit significand, so the interesting cases are
    // the ties — and the reason the conversion takes a `double` rather than
    // narrowing through a `float` first is that a float rounds ONCE before the
    // half rounds again, which turns a value above a tie into the tie itself.
    CHECK(doubleToFloat16Bits(1.0) == 0x3C00);
    CHECK(doubleToFloat16Bits(-1.0) == 0xBC00);
    CHECK(doubleToFloat16Bits(0.0) == 0x0000);
    CHECK(doubleToFloat16Bits(-0.0) == 0x8000);
    CHECK(doubleToFloat16Bits(65504.0) == 0x7BFF);  // the largest finite half
    CHECK(doubleToFloat16Bits(65520.0) == 0x7C00);  // the tie above it: to even, = inf
    CHECK(doubleToFloat16Bits(65519.0) == 0x7BFF);  // below the tie
    CHECK(doubleToFloat16Bits(std::numeric_limits<double>::infinity()) == 0x7C00);

    // NaN canonicalises to one quiet pattern, as the other float kinds do.
    CHECK(doubleToFloat16Bits(std::numeric_limits<double>::quiet_NaN()) == 0x7E00);
    CHECK(std::isnan(float16BitsToDouble(0x7E00)));

    // Subnormals, and the tie at half the smallest one going to +0.
    CHECK(float16BitsToDouble(0x0001) == std::ldexp(1.0, -24));
    CHECK(doubleToFloat16Bits(std::ldexp(1.0, -24)) == 0x0001);
    CHECK(doubleToFloat16Bits(std::ldexp(1.0, -25)) == 0x0000);
    CHECK(doubleToFloat16Bits(std::ldexp(1.5, -25)) == 0x0001);

    // The double-rounding witness. 2049 + 2^-30 is above the 2048/2050 tie, so
    // one correct rounding gives 2050 (0x6801). Narrowed through binary32 it
    // becomes exactly 2049 — the tie — and would round to 2048 (0x6800).
    const double witness = 2049.0 + std::ldexp(1.0, -30);
    CHECK(static_cast<double>(static_cast<float>(witness)) == 2049.0);
    CHECK(doubleToFloat16Bits(witness) == 0x6801);
    CHECK(float16BitsToDouble(doubleToFloat16Bits(witness)) == 2050.0);
    // The two ties either side, which go in OPPOSITE directions because
    // ties-to-even is about the significand and not about magnitude: the quantum
    // here is 2, so 2049 ties 2048 (significand 1024, even) against 2050 (1025,
    // odd) and lands on 2048, while 2051 ties 2050 against 2052 (1026, even)
    // and lands on 2052.
    CHECK(doubleToFloat16Bits(2049.0) == 0x6800);
    CHECK(doubleToFloat16Bits(2051.0) == 0x6802);

    // A Float16Array element agrees with the free functions by construction.
    Heap heap;
    ShadowStackFrame frame;
    Rooted<Value> view(Value::fromObject(TypedArrayHeader::create(heap, ElementKind::Float16, 4)));
    auto* v = view.get().asObject<TypedArrayHeader>();
    CHECK(v->bytesPerElement() == 2);
    v->set(0, witness);
    CHECK(v->get(0) == 2050.0);
    v->set(1, 0.1);
    CHECK(v->get(1) == float16BitsToDouble(doubleToFloat16Bits(0.1)));
    v->set(2, -0.0);
    CHECK(v->get(2) == 0.0);
    CHECK(std::signbit(v->get(2)));
}

// DOCUMENTED, NOT ENDORSED. 10.4.5's length-TRACKING views are unimplemented,
// and what follows pins the answers bronze gives instead — so the gap is a fact
// recorded in the suite rather than a surprise in someone's program.
//
// It is deliberately NOT an oracle case. An oracle case pins what a conforming
// engine prints, and every number below is the wrong one; promoting it would
// make a ratchet out of a bug.
//
// 23.2.5.1 step 6 makes `new Uint8Array(resizableBuffer)` — no length argument —
// a view whose [[ArrayLength]] is AUTO, and 10.4.5.11 TypedArrayLength then
// recomputes it from the buffer's current byteLength on every access. bronze
// stores an element count in the view header once, at construction, and every
// reader trusts it: the runtime helpers, and the inlined
// `BRONZE_ABI_TA_LENGTH_OFFSET` load that llvm_elem.cpp emits and marks
// invariant. So after a `resize` the view is stale in both directions — blind to
// bytes that appeared, and still claiming bytes that went away.
//
// At the JS level today, with `rab = new ArrayBuffer(4, {maxByteLength: 16})`:
//
//     const track = new Uint8Array(rab);   // 10.4.5: length-tracking
//     rab.resize(8);
//     track.length     // 4, should be 8
//     track[7] = 99;   // dropped; `track[7]` reads back undefined
//     rab.resize(2);
//     track.length     // 4, should be 2
//
// and a FIXED-length view over the same buffer is wrong the other way: 10.4.5.10
// IsTypedArrayOutOfBounds makes `new Uint8Array(rab, 0, 2)` out-of-bounds once
// the buffer shrinks below it, so `length` becomes 0 and every element is
// undefined, where bronze keeps answering 2 and reading the bytes.
//
// The cost of fixing it is structural rather than a matter of a bit: the header
// has a spare `reserved` word to carry the tracking flag with no ABI offset
// moving, but `length` stops being the answer, so every read of it becomes a
// branch — including the inlined invariant load, which is the one place bronze's
// typed-array element access is as fast as it is.
TEST_CASE("a view over a resizable buffer does NOT track it (10.4.5, unimplemented)") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> buffer(Value::fromObject(ArrayBufferHeader::createResizable(heap, 4, 16)));
    CHECK(buffer.get().asObject<ArrayBufferHeader>()->isResizable());

    // The view a length-tracking one would be: constructed over the whole
    // buffer, as 23.2.5.1 does when no length is given.
    Rooted<Value> view(Value::fromObject(
        TypedArrayHeader::createOverBuffer(heap, ElementKind::Uint8, buffer, 0, 4)));
    CHECK(view.get().asObject<TypedArrayHeader>()->length == 4);

    // Growing the buffer is exactly this assignment in 25.1.5.5 — the bytes
    // above the old length are already zeroed, since a resizable buffer reserves
    // its maximum up front.
    buffer.get().asObject<ArrayBufferHeader>()->byteLength = 8;
    CHECK(view.get().asObject<TypedArrayHeader>()->length == 4);  // 10.4.5: should be 8
    CHECK(view.get().asObject<TypedArrayHeader>()->byteLength() == 4);

    // And shrinking leaves the view addressing bytes the buffer no longer has.
    // Nothing here is unsafe — a resizable buffer keeps its maximum allocated,
    // so the read is in bounds of the ALLOCATION — but the answer is not the
    // spec's, which is `length` 0 for a view whose window no longer fits.
    buffer.get().asObject<ArrayBufferHeader>()->byteLength = 2;
    CHECK(view.get().asObject<TypedArrayHeader>()->length == 4);  // 10.4.5: should be 2
    CHECK(view.get().asObject<TypedArrayHeader>()->get(3) == 0.0);

    // The stale length survives a collection, which is the part that makes this a
    // stored fact and not a cached one: the copy phase moves the word along with
    // the header, and there is no place a recomputation could happen.
    heap.collect();
    CHECK(view.get().asObject<TypedArrayHeader>()->length == 4);
    CHECK(buffer.get().asObject<ArrayBufferHeader>()->byteLength == 2);
}
