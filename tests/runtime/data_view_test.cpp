// `DataView` below the compiler: the header's layout, the byte order, and the
// two things an oracle case cannot see.
//
// The oracle cases pin what ECMA-262 fixes about the object. What they cannot
// reach is the shape of the header — and that is where a DataView is one bad
// field away from a collector bug, because `{byteOffset, byteLength}` share the
// one 8-byte word the payload scan reads as a `Value`. The tests here hold that
// constraint as arithmetic rather than as a comment: a byteLength above
// 0xFFF1_0000 really would present a valid pointer tag, and the only thing
// standing between that and a length overwritten with an address is
// `kMaxByteLength`.
//
// The other half is the byte assembly. Every expectation below is a big-endian
// one by default, which on this machine is the order the host does NOT use — so
// a rewrite that reached for a native load would fail these and not merely be
// unportable.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// The `Name: message` of the last throw the two call helpers below caught, or
// "" when the last call returned. Text rather than the value, so nothing has
// to keep it rooted.
std::string g_thrownText;

// Forgets a caught throw whatever a CHECK did, so one failing expectation
// cannot make every later test in the binary look like it threw.
struct ClearCell {
    ~ClearCell() { g_thrownText.clear(); }
};

// Runs a call the way a program's `try` would: a throw out of it is recorded
// in g_thrownText and the call answers undefined.
template <typename Body>
Value catching(Body&& body) {
    g_thrownText.clear();
    Value result = Value::fromUndefined();
    Value thrown;
    if (rtTryCatch([&] { result = body(); }, thrown)) {
        g_thrownText = "?";
        rtErrorText(thrown, g_thrownText);
    }
    return result;
}

Value newBuffer(uint32_t byteLength) { return rtNewArrayBuffer(byteLength); }

// A named read through the same `bronze_prop_get` a compiled program issues:
// `DataView.prototype` is a real object since the prototype-object work, so
// the accessor a test calls is found by the ordinary chain walk and not by
// any table this file could reach around. The receiver is rooted before the
// key is registered because registering can allocate.
Value readMember(Value receiver, const char* name) {
    Rooted<Value> root{receiver};
    const uint32_t key = bronze_register_key_string(name);
    return Value(bronze_prop_get(root.get().rawBits(), key, nullptr));
}

// An argument vector held across an ALLOCATION, and put back afterwards.
//
// Both helpers below look up their callee before they call it, and both
// lookups intern a function object on first use — an allocation, and under
// BRONZE_GC_STRESS every allocation moves the world. A `std::vector<Value>`
// is ordinary C++ memory the collector has no view of, so a buffer passed in
// one is a pre-collection address by the time the callee reads it, and the
// symptom is `new DataView(buffer)` reporting that its first argument is not
// an ArrayBuffer. `RootedBlock` is what the runtime uses for the same hazard
// in `bronze_construct`. The accessor call copies back and calls with nothing
// allocating in between; the construction passes the block itself, because
// `bronze_construct` allocates the instance BEFORE its body reads a thing.
// A RootedBlock pushes pointers into its own storage, so it is built where it
// is used and never moved; these lines are spelled out at both sites for
// that reason rather than hidden behind a factory that would have to return
// one.
void fillBlock(RootedBlock& block, const std::vector<Value>& args) {
    for (uint32_t i = 0; i < args.size(); ++i) block.set(i, args[i]);
}

void refreshThroughRoots(std::vector<Value>& args, const RootedBlock& block) {
    for (uint32_t i = 0; i < args.size(); ++i) args[i] = Value(block.data()[i]);
}

// `new DataView(...)` through the constructor the provided-global path hands
// out and the [[Construct]] entry a compiled program reaches, so NewTarget is
// the constructor itself (25.3.2.1 step 1 makes a plain call a TypeError).
// The arguments go in from the ROOTED block: `bronze_construct` allocates the
// instance before the body reads them (rt_object.cpp names the contract), and
// a compiled caller's frame is rooted the same way.
Value newDataView(std::vector<Value> args) {
    RootedBlock block(static_cast<uint32_t>(args.size()));
    fillBlock(block, args);
    Rooted<Value> ctor{rtDataViewConstructor("DataView")};
    return catching([&] {
        return Value(bronze_construct(ctor.get().rawBits(), static_cast<uint32_t>(args.size()), block.data()));
    });
}

// `view.name(...)`, reached by the property path rather than by a C++ pointer,
// so the prototype object that answers a property read is the one under test.
Value callAccessor(Rooted<Value>& view, const char* name, std::vector<Value> args) {
    RootedBlock block(static_cast<uint32_t>(args.size()));
    fillBlock(block, args);
    Rooted<Value> fn{readMember(view.get(), name)};
    REQUIRE(fn.get().isObject());
    refreshThroughRoots(args, block);
    return catching([&] {
        return fn.get().asObject<FunctionHeader>()->call(view.get(), static_cast<uint32_t>(args.size()),
                                                         args.data());
    });
}

Value num(double d) { return Value::fromDouble(d); }

// The `Name: message` of the last call's throw, or "" when it returned; taken,
// so the next question starts clean.
std::string pendingText() {
    std::string out = std::move(g_thrownText);
    g_thrownText.clear();
    return out;
}

// The class alone, which is what the specification fixes; the message is
// bronze's wording and is deliberately not pinned here.
std::string pendingClass() {
    const std::string text = pendingText();
    const size_t colon = text.find(':');
    return colon == std::string::npos ? text : text.substr(0, colon);
}

uint8_t byteAt(Rooted<Value>& view, uint32_t index) {
    return view.get().asObject<DataViewHeader>()->buffer.asObject<ArrayBufferHeader>()->data()[index];
}

}  // namespace

TEST_CASE("a DataView header is an object prefix, a buffer Value and ONE window word") {
    // The ordinary-object prefix (header, shape, the inline slots a named
    // property lands in), then the buffer, then the pair. A further payload
    // word would be scanned as a Value too, and would hold whatever the
    // allocator last left there.
    constexpr size_t prefix = sizeof(ObjectHeader) + sizeof(Value) * ObjectHeader::kInlineSlots;
    CHECK(offsetof(DataViewHeader, object) == 0);
    CHECK(offsetof(DataViewHeader, inlineSlots) == sizeof(ObjectHeader));
    CHECK(offsetof(DataViewHeader, buffer) == prefix);
    CHECK(offsetof(DataViewHeader, byteOffset) == prefix + 8);
    CHECK(offsetof(DataViewHeader, byteLength) == prefix + 12);
    CHECK(sizeof(DataViewHeader) == prefix + 16);
    CHECK(DataViewHeader::kFlags == HeapKind::DataView);
    // A kind of its own, and not a number borrowed from a neighbour — which is
    // the collision the HeapKind registry exists to make impossible.
    CHECK(DataViewHeader::kFlags != TypedArrayHeader::kFlags);
    CHECK(DataViewHeader::kFlags != ArrayBufferHeader::kFlags);
}

TEST_CASE("kMaxByteLength is what keeps the shared window word a non-pointer") {
    // The collector reads the payload as an array of Values and tests each
    // one's tag. On a little-endian host `byteLength` is the top half of the
    // second word, so the tag it presents is `byteLength >> 16`.
    DataViewHeader probe{};
    probe.byteOffset = kMaxByteLength;
    probe.byteLength = kMaxByteLength;
    uint64_t word = 0;
    std::memcpy(&word, &probe.byteOffset, sizeof word);
    CHECK_FALSE(Value(word).isPointer());

    // And the hazard is real rather than hypothetical: one byteLength above
    // this and the word IS a pointer, so the collector would relocate the
    // window's length into an address.
    probe.byteLength = 0xFFF10000u;
    std::memcpy(&word, &probe.byteOffset, sizeof word);
    CHECK(Value(word).isPointer());
    CHECK(kMaxByteLength < 0xFFF10000u);
}

TEST_CASE("a DataView's buffer is forwarded, and its window survives collection") {
    Heap heap;
    ShadowStackFrame frame;
    // A local heap with no runtime behind it: the shape is a bare root, since
    // the collection and not the chain is what this case is about.
    NonMovingArena arena;
    Shape* shape = Shape::createRoot(arena, Value::fromNull());

    Rooted<Value> buf(Value::fromObject(ArrayBufferHeader::create(heap, shape, 32)));
    Rooted<Value> view(Value::fromObject(DataViewHeader::create(heap, shape, buf, 8, 16)));
    const uintptr_t before = reinterpret_cast<uintptr_t>(view.get().asObject<DataViewHeader>());
    view.get().asObject<DataViewHeader>()->bytes()[0] = 0xAB;
    view.get().asObject<DataViewHeader>()->bytes()[15] = 0xCD;

    // Something unreachable, so the collection has work to do and the survivors
    // land at different addresses.
    ArrayBufferHeader::create(heap, shape, 1024);
    heap.collect();

    auto* v = view.get().asObject<DataViewHeader>();
    CHECK(reinterpret_cast<uintptr_t>(v) != before);
    CHECK(v->byteOffset == 8);
    CHECK(v->byteLength == 16);
    // The address is RECOMPUTED from the buffer Value, which is the whole GC
    // rule: a cached `uint8_t*` would name dead from-space here.
    CHECK(v->bytes()[0] == 0xAB);
    CHECK(v->bytes()[15] == 0xCD);
    CHECK(v->buffer.asObject<ArrayBufferHeader>()->byteLength == 32);
    CHECK(v->buffer.asObject<ArrayBufferHeader>()->data() + 8 == v->bytes());
}

TEST_CASE("the default byte order is big-endian, whatever the host's is") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};
    Rooted<Value> view{newDataView({buf.get()})};
    REQUIRE(view.get().isObject());
    CHECK(view.get().asObject<HeapObjectHeader>()->flags == DataViewHeader::kFlags);

    // 0x0102 written with no order named puts the most significant byte first,
    // which is the opposite of what this machine would do with a native store.
    callAccessor(view, "setUint16", {num(0), num(258)});
    CHECK(byteAt(view, 0) == 0x01);
    CHECK(byteAt(view, 1) == 0x02);
    CHECK(callAccessor(view, "getUint16", {num(0)}).asNumber() == 258);
    CHECK(callAccessor(view, "getUint16", {num(0), Value::fromBool(true)}).asNumber() == 513);

    callAccessor(view, "setUint16", {num(0), num(258), Value::fromBool(true)});
    CHECK(byteAt(view, 0) == 0x02);
    CHECK(byteAt(view, 1) == 0x01);

    // 32 and 64 bits, so the loop is exercised past the two-byte case where a
    // reversal and a swap are the same operation.
    callAccessor(view, "setUint32", {num(0), num(16909060)});
    CHECK(byteAt(view, 0) == 0x01);
    CHECK(byteAt(view, 1) == 0x02);
    CHECK(byteAt(view, 2) == 0x03);
    CHECK(byteAt(view, 3) == 0x04);
    CHECK(callAccessor(view, "getUint32", {num(0), Value::fromBool(true)}).asNumber() == 67305985);

    callAccessor(view, "setFloat64", {num(0), num(1.5)});
    CHECK(byteAt(view, 0) == 0x3F);
    CHECK(byteAt(view, 1) == 0xF8);
    for (uint32_t i = 2; i < 8; ++i) CHECK(byteAt(view, i) == 0x00);
    CHECK(callAccessor(view, "getFloat64", {num(0)}).asNumber() == 1.5);
}

TEST_CASE("a one-byte accessor has no order to have, and the flag is ignored") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(2)};
    Rooted<Value> view{newDataView({buf.get()})};
    REQUIRE(view.get().isObject());
    callAccessor(view, "setInt8", {num(0), num(-1), Value::fromBool(true)});
    CHECK(byteAt(view, 0) == 0xFF);
    CHECK(callAccessor(view, "getInt8", {num(0)}).asNumber() == -1);
    CHECK(callAccessor(view, "getInt8", {num(0), Value::fromBool(true)}).asNumber() == -1);
    CHECK(callAccessor(view, "getUint8", {num(0)}).asNumber() == 255);
}

TEST_CASE("the constructor's ladder: TypeError for the kind, RangeError for the value") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};

    // 25.3.2.1 step 2 — the wrong KIND of first argument.
    newDataView({Value::fromDouble(8)});
    CHECK(pendingClass() == "TypeError");
    newDataView({});
    CHECK(pendingClass() == "TypeError");
    {
        // A typed array has [[ViewedArrayBuffer]] and not [[ArrayBufferData]],
        // so it is on this rung and its `.buffer` is not.
        Rooted<Value> ta{rtNewTypedArray(ElementKind::Uint8, 8)};
        newDataView({ta.get()});
        CHECK(pendingClass() == "TypeError");
        Rooted<Value> ok{newDataView({ta.get().asObject<TypedArrayHeader>()->buffer})};
        CHECK(pendingText().empty());
        CHECK(ok.get().asObject<DataViewHeader>()->byteLength == 8);
    }

    // Steps 3, 6 and 9 — a NUMBER out of range, every one a RangeError.
    newDataView({buf.get(), num(-1)});
    CHECK(pendingClass() == "RangeError");
    newDataView({buf.get(), num(9)});
    CHECK(pendingClass() == "RangeError");
    newDataView({buf.get(), num(0), num(9)});
    CHECK(pendingClass() == "RangeError");
    newDataView({buf.get(), num(4), num(5)});
    CHECK(pendingClass() == "RangeError");
    newDataView({buf.get(), num(0), num(-1)});
    CHECK(pendingClass() == "RangeError");

    // ToIndex TRUNCATES, so a fraction is legal; -0 is +0 and so is not a
    // negative offset at all.
    {
        Rooted<Value> a{newDataView({buf.get(), num(1.9)})};
        CHECK(pendingText().empty());
        CHECK(a.get().asObject<DataViewHeader>()->byteOffset == 1);
        CHECK(a.get().asObject<DataViewHeader>()->byteLength == 7);
        Rooted<Value> b{newDataView({buf.get(), num(-0.0)})};
        CHECK(pendingText().empty());
        CHECK(b.get().asObject<DataViewHeader>()->byteOffset == 0);
        // An offset AT the end is legal and makes an empty window.
        Rooted<Value> c{newDataView({buf.get(), num(8)})};
        CHECK(pendingText().empty());
        CHECK(c.get().asObject<DataViewHeader>()->byteLength == 0);
    }
}

TEST_CASE("an access past the window is a RangeError, and a wrong receiver a TypeError") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};
    Rooted<Value> view{newDataView({buf.get(), num(4), num(2)})};

    // The bound is the WINDOW's byteLength and not the buffer's, and it depends
    // on the width the accessor names.
    CHECK(callAccessor(view, "getUint16", {num(0)}).asNumber() == 0);
    CHECK(pendingText().empty());
    callAccessor(view, "getUint16", {num(1)});
    CHECK(pendingClass() == "RangeError");
    callAccessor(view, "getUint32", {num(0)});
    CHECK(pendingClass() == "RangeError");
    callAccessor(view, "getUint8", {num(-1)});
    CHECK(pendingClass() == "RangeError");
    callAccessor(view, "setUint8", {num(2), num(1)});
    CHECK(pendingClass() == "RangeError");

    // 25.3.1.1 step 1, RequireInternalSlot([[DataView]]).
    {
        Rooted<Value> fn{readMember(view.get(), "getUint8")};
        Value args[1] = {num(0)};
        catching([&] { return fn.get().asObject<FunctionHeader>()->call(buf.get(), 1, args); });
        CHECK(pendingClass() == "TypeError");
        catching([&] {
            return fn.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 1, args);
        });
        CHECK(pendingClass() == "TypeError");
    }
}

TEST_CASE("a DataView and a typed array over one buffer are one store") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};
    Rooted<Value> view{newDataView({buf.get()})};
    Rooted<Value> ta{rtNewTypedArrayOverBuffer(ElementKind::Uint8, buf, 0, 8, /*tracking=*/false)};

    callAccessor(view, "setUint32", {num(0), num(16909060)});
    auto* elems = ta.get().asObject<TypedArrayHeader>();
    CHECK(elems->get(0) == 1);
    CHECK(elems->get(1) == 2);
    CHECK(elems->get(2) == 3);
    CHECK(elems->get(3) == 4);

    ta.get().asObject<TypedArrayHeader>()->set(4, 255);
    ta.get().asObject<TypedArrayHeader>()->set(5, 254);
    CHECK(callAccessor(view, "getInt16", {num(4)}).asNumber() == -2);
    CHECK(callAccessor(view, "getUint16", {num(4)}).asNumber() == 65534);
    CHECK(view.get().asObject<DataViewHeader>()->buffer.rawBits() == buf.get().rawBits());
}

// 25.3.4.5, .6, .19 and .20 — the only DataView accessors whose value is a
// BigInt, and the only way eight raw bytes become a 64-bit integer without
// passing through a double that cannot hold one.
TEST_CASE("the 64-bit accessors round-trip a value no double holds") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(16)};
    Rooted<Value> view{newDataView({buf.get()})};

    const auto big = [](const char* digits) {
        BigNum v;
        REQUIRE(rtStringToBigInt(digits, v));
        return rtMakeBigInt(v);
    };
    const auto text = [](Value v) { return rtBigIntToString(v, 10); };

    // 2^63 - 1 and -2^63: the two values the signed window ends at, and
    // neither is representable as a double.
    callAccessor(view, "setBigInt64", {num(0), big("9223372036854775807")});
    CHECK(text(callAccessor(view, "getBigInt64", {num(0)})) == "9223372036854775807");
    CHECK(text(callAccessor(view, "getBigUint64", {num(0)})) == "9223372036854775807");

    callAccessor(view, "setBigInt64", {num(0), big("-9223372036854775808")});
    CHECK(text(callAccessor(view, "getBigInt64", {num(0)})) == "-9223372036854775808");
    CHECK(text(callAccessor(view, "getBigUint64", {num(0)})) == "9223372036854775808");

    // The same eight bytes read the other way round: -1 is all ones.
    callAccessor(view, "setBigUint64", {num(8), big("18446744073709551615")});
    CHECK(text(callAccessor(view, "getBigInt64", {num(8)})) == "-1");
    CHECK(text(callAccessor(view, "getBigUint64", {num(8)})) == "18446744073709551615");

    // 25.3.1.5 NumericToRawBytes is modulo 2^64 for the BigInt rows, so a
    // value too wide WRAPS rather than throwing — setInt32's rule, widened.
    callAccessor(view, "setBigInt64", {num(0), big("18446744073709551621")});
    CHECK(text(callAccessor(view, "getBigUint64", {num(0)})) == "5");

    // Byte order, checked against the individual bytes rather than against
    // the accessor that wrote them.
    callAccessor(view, "setBigUint64", {num(0), big("72623859790382856")});
    CHECK(callAccessor(view, "getUint8", {num(0)}).asNumber() == 1);
    CHECK(callAccessor(view, "getUint8", {num(7)}).asNumber() == 8);
    callAccessor(view, "setBigUint64", {num(0), big("72623859790382856"), Value::fromBool(true)});
    CHECK(callAccessor(view, "getUint8", {num(0)}).asNumber() == 8);
    CHECK(callAccessor(view, "getUint8", {num(7)}).asNumber() == 1);

    // Step 4 is ToBigInt, and 7.1.13 has no Number row: there is no implicit
    // widening of 1 to 1n, and the refusal is a catchable TypeError.
    callAccessor(view, "setBigInt64", {num(0), num(1)});
    CHECK(pendingClass() == "TypeError");
}

TEST_CASE("the twenty accessors are twenty function objects on one prototype") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};
    Rooted<Value> view{newDataView({buf.get()})};
    Rooted<Value> other{newDataView({buf.get(), num(4)})};

    // One function object per accessor, held by `DataView.prototype` and so
    // shared by every DataView; a different one per width and per direction.
    const uint64_t get16 = readMember(view.get(), "getUint16").rawBits();
    CHECK(get16 == readMember(view.get(), "getUint16").rawBits());
    CHECK(get16 == readMember(other.get(), "getUint16").rawBits());
    CHECK(get16 != readMember(view.get(), "getUint32").rawBits());
    CHECK(get16 != readMember(view.get(), "getInt16").rawBits());
    CHECK(get16 != readMember(view.get(), "setUint16").rawBits());

    // A name 25.3.4 does not define really is absent.
    CHECK(readMember(view.get(), "getInt24").isUndefined());
    // The four 64-bit accessors are ordinary members of the same prototype, so
    // they are shared per name exactly as the sixteen number accessors are.
    const uint64_t getBig = readMember(view.get(), "getBigInt64").rawBits();
    CHECK(getBig == readMember(other.get(), "getBigInt64").rawBits());
    CHECK(getBig != readMember(view.get(), "getBigUint64").rawBits());
    CHECK(getBig != readMember(view.get(), "setBigInt64").rawBits());
    CHECK_FALSE(readMember(view.get(), "setBigUint64").isUndefined());
}

TEST_CASE("the slot accessors report the WINDOW, not the buffer") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(16)};
    Rooted<Value> view{newDataView({buf.get(), num(4), num(8)})};

    CHECK(readMember(view.get(), "byteLength").asNumber() == 8);
    CHECK(readMember(view.get(), "byteOffset").asNumber() == 4);
    CHECK(readMember(view.get(), "buffer").rawBits() == buf.get().rawBits());
    CHECK(readMember(view.get(), "constructor").rawBits() ==
          rtDataViewConstructor("DataView").rawBits());
    CHECK(std::string(rtDataViewConstructorName(rtDataViewConstructor("DataView"))) == "DataView");
    CHECK(rtDataViewConstructorName(buf.get()) == nullptr);
    CHECK(rtDataViewConstructor("DataViewish").isUndefined());

    // Index 0 of the window is byte 4 of the buffer.
    callAccessor(view, "setUint8", {num(0), num(9)});
    CHECK(byteAt(view, 4) == 9);
    CHECK(byteAt(view, 0) == 0);
}

TEST_CASE("NumericToRawBytes takes the modulo, and pins one NaN encoding") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> buf{newBuffer(8)};
    Rooted<Value> view{newDataView({buf.get()})};

    // Table 70's conversion is ToInt8 .. ToUint32: a truncation and a modulo,
    // never a clamp — there is no clamped entry in the table, and no accessor
    // that could name one.
    callAccessor(view, "setInt8", {num(0), num(300)});
    CHECK(byteAt(view, 0) == 44);
    callAccessor(view, "setUint8", {num(0), num(-1.7)});
    CHECK(byteAt(view, 0) == 255);
    callAccessor(view, "setInt32", {num(0), num(-2)});
    CHECK(callAccessor(view, "getUint32", {num(0)}).asNumber() == 4294967294.0);
    callAccessor(view, "setUint32", {num(0), num(-1)});
    CHECK(callAccessor(view, "getUint32", {num(0)}).asNumber() == 4294967295.0);
    CHECK(callAccessor(view, "getInt32", {num(0)}).asNumber() == -1.0);

    // 25.3.1.5 allows any NaN pattern; bronze writes exactly one, because a
    // compiler's choice of payload is not a deterministic output.
    callAccessor(view, "setFloat64", {num(0), num(std::nan(""))});
    CHECK(byteAt(view, 0) == 0x7F);
    CHECK(byteAt(view, 1) == 0xF8);
    for (uint32_t i = 2; i < 8; ++i) CHECK(byteAt(view, i) == 0x00);
    callAccessor(view, "setFloat32", {num(0), num(std::nan(""))});
    CHECK(byteAt(view, 0) == 0x7F);
    CHECK(byteAt(view, 1) == 0xC0);
    CHECK(byteAt(view, 2) == 0x00);
    CHECK(byteAt(view, 3) == 0x00);
}
