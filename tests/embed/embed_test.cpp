// Runtime-level tests of the embedding API: everything here goes through the
// same heap, registries and call machinery a real host would, and none of it
// needs the LLVM backend — the "compiled program" side of each seam is played
// by the runtime's own dynamic-call path. The embed-gc-stress run re-executes
// all of it with a collection forced at every allocation, which is where the
// rooting mistakes this module can make actually surface.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("a native function is callable through the runtime's dynamic call path") {
    int hits = 0;
    embed::Persistent fn{embed::makeFunction(
        [&hits](embed::Value thisValue, std::span<const embed::Value> args) -> embed::Value {
            (void)thisValue;
            ++hits;
            double sum = 0.0;
            for (const embed::Value& a : args) sum += embed::toDouble(a);
            return embed::fromDouble(sum);
        },
        /*arity=*/2)};

    CHECK(embed::isFunction(fn.get()));

    std::vector<embed::Value> args{embed::fromDouble(2.0), embed::fromDouble(40.0)};
    embed::CallResult result = embed::call(fn.get(), embed::undefined(), args);
    CHECK(!result.thrown);
    CHECK(result.value.isNumber());
    CHECK(result.value.asNumber() == 42.0);
    CHECK(hits == 1);
}

TEST_CASE("a host callback throws into JS through the exception cell") {
    embed::Persistent thrower{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::throwTypeError("host says no");
        })};

    embed::CallResult result = embed::call(thrower.get(), embed::undefined(), {});
    CHECK(result.thrown);
    // What a JS `catch` would see: a TypeError instance, a real object.
    CHECK(result.value.isObject());

    // The boundary cleared the cell: an unrelated call afterwards is clean,
    // rather than appearing to throw its predecessor's exception.
    embed::Persistent ok{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromDouble(1.0);
        })};
    embed::CallResult second = embed::call(ok.get(), embed::undefined(), {});
    CHECK(!second.thrown);
    CHECK(second.value.asNumber() == 1.0);
}

TEST_CASE("calling a non-function reports the TypeError as a thrown result") {
    embed::CallResult result = embed::call(embed::fromDouble(3.0), embed::undefined(), {});
    CHECK(result.thrown);
    CHECK(result.value.isObject());
}

TEST_CASE("a registered host global answers bronze_global_get") {
    // A root frame, because this case calls a GENERATED-CODE helper directly
    // and generated code never arrives without one. Rooted<> registers into
    // ShadowStackFrame::current(), and with no frame open it registers
    // nothing at all — so the builtin namespaces that the first
    // bronze_global_get in a process builds (rtResolveBuiltinGlobal's ladder
    // constructs them lazily) would be collected while still half-built. It
    // is invisible until this case runs BEFORE anything else has built that
    // ladder, and under BRONZE_GC_STRESS, where the collection comes at every
    // allocation: an order-dependent crash rather than a constant one. Every
    // embed:: entry point opens its own frame for this reason; a test that
    // reaches past them owes the same.
    ShadowStackFrame frame;

    // The two halves a compiled read arrives with: the key string registered
    // under the index lowering assigned it, and the host's value in the
    // registry. A heap STRING value on purpose — it moves at every
    // collection, so this checks the registry is a real root source and not a
    // cache of stale bits.
    embed::registerGlobal("engineName", embed::fromUtf8("bro"));
    const uint32_t key = bronze_register_key_string("engineName");

    runtime::rtHeap().collect();

    // No cache cell: the runtime's own callers have no module to hold one, and
    // a host global is never cached anyway.
    Value v{bronze_global_get(key, nullptr)};
    CHECK(v.isString());
    CHECK(embed::toUtf8(v) == "bro");

    // Re-registration replaces, and the read sees the replacement — the
    // reason host globals stay out of the builtin cache.
    embed::registerGlobal("engineName", embed::fromDouble(2.0));
    Value replaced{bronze_global_get(key, nullptr)};
    CHECK(replaced.isNumber());
    CHECK(replaced.asNumber() == 2.0);
}

TEST_CASE("Persistent keeps a heap value alive and current across a collection") {
    embed::Persistent p{embed::fromUtf8("persistent_payload")};
    const uint64_t before = p.get().rawBits();

    runtime::rtHeap().collect();

    Value after = p.get();
    CHECK(after.isString());
    // A semispace survivor is always COPIED to the other space, so the handle
    // proving it tracked the move is the address having changed.
    CHECK(after.rawBits() != before);
    CHECK(embed::toUtf8(after) == "persistent_payload");
}

TEST_CASE("Persistent copies are independent roots over the same value") {
    embed::Persistent a{embed::fromUtf8("shared")};
    embed::Persistent b = a;
    CHECK(b.get().rawBits() == a.get().rawBits());

    runtime::rtHeap().collect();
    CHECK(embed::toUtf8(a.get()) == "shared");
    CHECK(embed::toUtf8(b.get()) == "shared");
    // Still ONE value: both slots forwarded to the same relocated string.
    CHECK(b.get().rawBits() == a.get().rawBits());

    b.set(embed::fromDouble(5.0));
    CHECK(embed::toUtf8(a.get()) == "shared");
    CHECK(b.get().asNumber() == 5.0);
}

TEST_CASE("bits round-trip through a Persistent") {
    embed::Persistent p{embed::fromUtf8("bits")};
    const uint64_t bits = embed::toBits(p.get());
    CHECK(embed::fromBits(bits).rawBits() == bits);
    CHECK(embed::toUtf8(embed::fromBits(embed::toBits(p.get()))) == "bits");
}

TEST_CASE("a native handle's finalizer runs when the cell dies and not while rooted") {
    int destroyed = 0;
    {
        embed::Persistent keep{embed::makeHandle(
            &destroyed, [](void* p) { ++*static_cast<int*>(p); })};
        CHECK(embed::handleData(keep.get()) == &destroyed);

        runtime::rtHeap().collect();
        // Rooted: the sweep re-pointed the registry entry at the survivor and
        // ran nothing.
        CHECK(destroyed == 0);
        CHECK(embed::handleData(keep.get()) == &destroyed);
    }

    // The root is gone; the next collection proves the cell dead and the
    // sweep runs the destructor — exactly once, however many collections
    // follow.
    runtime::rtHeap().collect();
    CHECK(destroyed == 1);
    runtime::rtHeap().collect();
    CHECK(destroyed == 1);
}

TEST_CASE("handleData refuses values that are not handles") {
    CHECK(embed::handleData(embed::undefined()) == nullptr);
    CHECK(embed::handleData(embed::fromDouble(1.0)) == nullptr);
    embed::Persistent obj{embed::createObject()};
    CHECK(embed::handleData(obj.get()) == nullptr);
}

TEST_CASE("host-built objects take properties, elements and accessors") {
    embed::Persistent obj{embed::createObject()};
    obj.set(embed::setProperty(obj.get(), "answer", embed::fromDouble(42.0)));
    obj.set(embed::setElement(obj.get(), 3, embed::fromUtf8("third")));

    // Read back through the runtime's own property path.
    {
        ShadowStackFrame frame;
        Rooted<Value> key{runtime::rtMakeString("answer")};
        Value got = obj.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key);
        CHECK(got.isNumber());
        CHECK(got.asNumber() == 42.0);
    }
    {
        ShadowStackFrame frame;
        Rooted<Value> key{runtime::rtMakeString("3")};
        Value got = obj.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key);
        CHECK(got.isString());
        CHECK(embed::toUtf8(got) == "third");
    }

    // An accessor whose getter is a host function: reading the property runs
    // host code.
    embed::Persistent getter{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromDouble(7.0);
        })};
    obj.set(embed::defineAccessor(obj.get(), "computed", getter.get(), embed::undefined()));
    {
        ShadowStackFrame frame;
        Rooted<Value> key{runtime::rtMakeString("computed")};
        Value got = obj.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key);
        CHECK(got.isNumber());
        CHECK(got.asNumber() == 7.0);
    }
}

TEST_CASE("promise create -> resolve -> then-callback-order") {
    embed::Persistent p{embed::createPromise()};
    CHECK(embed::isPromise(p.get()));

    std::vector<std::string> order;

    embed::Persistent fn1{embed::makeFunction(
        [&order](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            order.push_back("then1:" + (args.empty() ? "" : embed::toUtf8(args[0])));
            return embed::undefined();
        },
        1)};

    embed::Persistent fn2{embed::makeFunction(
        [&order](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            order.push_back("then2:" + (args.empty() ? "" : embed::toUtf8(args[0])));
            return embed::undefined();
        },
        1)};

    Value thenMethod = embed::getProperty(p.get(), "then");
    CHECK(embed::isFunction(thenMethod));

    embed::call(thenMethod, p.get(), std::vector<embed::Value>{fn1.get()});
    embed::call(thenMethod, p.get(), std::vector<embed::Value>{fn2.get()});

    // Before resolving: no callbacks run.
    CHECK(order.empty());

    // Resolve the promise.
    embed::resolvePromise(p.get(), embed::fromUtf8("hello"));

    // Before draining microtasks: no callbacks run yet.
    CHECK(order.empty());
    CHECK(embed::microtasksPending());

    // Drain microtasks.
    embed::drainMicrotasks();
    CHECK(!embed::microtasksPending());

    // Callbacks ran in exact registration order.
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "then1:hello");
    CHECK(order[1] == "then2:hello");

    // Adding a then callback to an already settled promise also schedules reaction through the microtask queue.
    embed::Persistent fn3{embed::makeFunction(
        [&order](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            order.push_back("then3:" + (args.empty() ? "" : embed::toUtf8(args[0])));
            return embed::undefined();
        },
        1)};
    embed::call(thenMethod, p.get(), std::vector<embed::Value>{fn3.get()});
    CHECK(order.size() == 2);
    CHECK(embed::microtasksPending());

    embed::drainMicrotasks();
    REQUIRE(order.size() == 3);
    CHECK(order[2] == "then3:hello");
}

TEST_CASE("promise reject -> catch") {
    embed::Persistent p{embed::createPromise()};
    CHECK(embed::isPromise(p.get()));

    std::string caught;
    embed::Persistent catchHandler{embed::makeFunction(
        [&caught](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            caught = args.empty() ? "" : embed::toUtf8(args[0]);
            return embed::undefined();
        },
        1)};

    Value catchMethod = embed::getProperty(p.get(), "catch");
    CHECK(embed::isFunction(catchMethod));
    embed::call(catchMethod, p.get(), std::vector<embed::Value>{catchHandler.get()});

    CHECK(caught.empty());

    embed::rejectPromise(p.get(), embed::fromUtf8("bad error"));

    CHECK(caught.empty());
    CHECK(embed::microtasksPending());

    embed::drainMicrotasks();
    CHECK(!embed::microtasksPending());
    CHECK(caught == "bad error");

    // First settle wins: a subsequent resolve or reject on already settled promise is ignored.
    embed::resolvePromise(p.get(), embed::fromUtf8("ignored"));
    embed::rejectPromise(p.get(), embed::fromUtf8("also ignored"));
    embed::drainMicrotasks();
    CHECK(caught == "bad error");
}

TEST_CASE("promise settlement after GC pressure") {
    embed::Persistent p{embed::createPromise()};
    embed::Persistent payload{embed::fromUtf8("survived_gc_data")};

    std::string received;
    embed::Persistent fn{embed::makeFunction(
        [&received](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            received = args.empty() ? "" : embed::toUtf8(args[0]);
            return embed::undefined();
        },
        1)};

    Value thenMethod = embed::getProperty(p.get(), "then");
    embed::call(thenMethod, p.get(), std::vector<embed::Value>{fn.get()});

    const uint64_t promiseBitsBefore = p.get().rawBits();
    const uint64_t payloadBitsBefore = payload.get().rawBits();

    // Force GC collection before settling.
    runtime::rtHeap().collect();

    // Moving semispace collector relocates objects.
    CHECK(p.get().rawBits() != promiseBitsBefore);
    CHECK(payload.get().rawBits() != payloadBitsBefore);
    CHECK(embed::isPromise(p.get()));

    // Force another collection.
    runtime::rtHeap().collect();

    // Settle with relocated values.
    embed::resolvePromise(p.get(), payload.get());

    // Force another collection while reaction is in the microtask queue.
    runtime::rtHeap().collect();

    embed::drainMicrotasks();
    CHECK(received == "survived_gc_data");
}

TEST_CASE("createArrayBuffer and info helpers") {
    embed::Persistent buf1{embed::createArrayBuffer(16)};
    CHECK(embed::isArrayBuffer(buf1.get()));
    CHECK(!embed::isTypedArray(buf1.get()));

    embed::ArrayBufferInfo info1 = embed::arrayBufferInfo(buf1.get());
    CHECK(info1.data != nullptr);
    CHECK(info1.byteLength == 16);

    uint8_t rawBytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    embed::Persistent buf2{embed::createArrayBuffer(rawBytes)};
    CHECK(embed::isArrayBuffer(buf2.get()));

    embed::ArrayBufferInfo info2 = embed::arrayBufferInfo(buf2.get());
    CHECK(info2.byteLength == 8);
    CHECK(std::memcmp(info2.data, rawBytes, 8) == 0);

    // GC keeps ArrayBuffer alive and payload intact.
    runtime::rtHeap().collect();
    embed::ArrayBufferInfo info2After = embed::arrayBufferInfo(buf2.get());
    CHECK(info2After.byteLength == 8);
    CHECK(std::memcmp(info2After.data, rawBytes, 8) == 0);
}

TEST_CASE("parseJson parses valid JSON and rejects invalid syntax") {
    embed::CallResult ok = embed::parseJson("{\"name\": \"bronze\", \"count\": 42, \"flag\": true}");
    CHECK(!ok.thrown);
    CHECK(embed::isObject(ok.value));

    embed::Persistent obj{ok.value};
    embed::Value nameVal = embed::getProperty(obj.get(), "name");
    CHECK(embed::toUtf8(nameVal) == "bronze");
    embed::Value countVal = embed::getProperty(obj.get(), "count");
    CHECK(embed::toDouble(countVal) == 42.0);
    embed::Value flagVal = embed::getProperty(obj.get(), "flag");
    CHECK(embed::toBool(flagVal) == true);

    // Invalid JSON returns thrown=true with an Error object.
    embed::CallResult bad = embed::parseJson("{invalid json}");
    CHECK(bad.thrown);
    CHECK(embed::isObject(bad.value));
}


TEST_CASE("createTypedArray builds views indistinguishable from the program's own") {
    struct Case {
        ElementKind kind;
        uint32_t bytesPerElement;
    };
    // The five a host binding actually produces — a texture's bytes, a clamped
    // image plane, indices, positions, doubles — and the widths they promise.
    const Case cases[] = {
        {embed::elements::Uint8, 1},   {embed::elements::Uint8Clamped, 1},
        {embed::elements::Int32, 4},   {embed::elements::Float32, 4},
        {embed::elements::Float64, 8},
    };

    for (const Case& c : cases) {
        embed::Persistent view{embed::createTypedArray(c.kind, 4)};
        CHECK(embed::isTypedArray(view.get()));
        embed::TypedArrayInfo info = embed::typedArrayInfo(view.get());
        REQUIRE(static_cast<bool>(info));
        CHECK(info.elementCount == 4);
        CHECK(info.bytesPerElement == c.bytesPerElement);
        CHECK(info.byteLength == 4 * c.bytesPerElement);
        CHECK(info.elementKind == c.kind);
        // 23.2.5.1 allocates a zero-filled buffer, and a host handing the
        // program a half-initialized view would be handing it whatever the
        // allocator last wrote there.
        for (uint32_t i = 0; i < info.byteLength; ++i) CHECK(info.data[i] == 0);
        // And the program's own view of it: `length` is a property, not just a
        // header field, which is what makes this a real typed array rather
        // than something that only looks like one from C++.
        CHECK(embed::toDouble(embed::getProperty(view.get(), "length")) == 4.0);
    }
}

TEST_CASE("createTypedArray refuses a length the heap cannot hold") {
    // The constructor's RangeError, through the host path: a length whose byte
    // size overflows 32 bits if it is multiplied carelessly. The refusal is
    // what must happen; a heap that dies mid-copy is what must not.
    embed::Value refused = embed::createTypedArray(embed::elements::Float64, 0xFFFFFFFFu);
    CHECK(embed::isUndefined(refused));
    CHECK(bronze_exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS);
    bronze_exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
}

TEST_CASE("fillTypedArray copies host bytes in and refuses what does not fit") {
    embed::Persistent view{embed::createTypedArray(embed::elements::Float32, 3)};

    const float source[3] = {1.5f, -2.25f, 4.0f};
    uint8_t raw[sizeof(source)];
    std::memcpy(raw, source, sizeof(source));
    CHECK(embed::fillTypedArray(view.get(), raw));

    // Read back as the PROGRAM sees it — element by element through the
    // generic element path, not by reinterpreting the same bytes again.
    CHECK(embed::toDouble(embed::getElement(view.get(), 0)) == 1.5);
    CHECK(embed::toDouble(embed::getElement(view.get(), 1)) == -2.25);
    CHECK(embed::toDouble(embed::getElement(view.get(), 2)) == 4.0);

    // Too many bytes writes NOTHING: a partial fill would leave the program
    // holding half a texture with nothing to say so.
    const uint8_t tooMany[sizeof(source) + 1] = {};
    CHECK(!embed::fillTypedArray(view.get(), tooMany));
    CHECK(embed::toDouble(embed::getElement(view.get(), 0)) == 1.5);

    // A receiver that is not a view is refused rather than reinterpreted.
    embed::Persistent notAView{embed::createObject()};
    CHECK(!embed::fillTypedArray(notAView.get(), raw));
    CHECK(!embed::fillTypedArray(embed::fromDouble(1.0), raw));
}

TEST_CASE("setElement writes typed-array elements with the kind's conversion") {
    embed::Persistent bytes{embed::createTypedArray(embed::elements::Uint8, 2)};
    bytes.set(embed::setElement(bytes.get(), 0, embed::fromDouble(7.0)));
    // 7.1.6 ToUint8 wraps, and the host path must not invent its own answer.
    bytes.set(embed::setElement(bytes.get(), 1, embed::fromDouble(300.0)));
    CHECK(embed::toDouble(embed::getElement(bytes.get(), 0)) == 7.0);
    CHECK(embed::toDouble(embed::getElement(bytes.get(), 1)) == 44.0);

    // The clamped kind is a different conversion on the same path.
    embed::Persistent clamped{embed::createTypedArray(embed::elements::Uint8Clamped, 1)};
    clamped.set(embed::setElement(clamped.get(), 0, embed::fromDouble(300.0)));
    CHECK(embed::toDouble(embed::getElement(clamped.get(), 0)) == 255.0);

    // Out of range is DROPPED, which is what a typed array does — not an
    // exception, and not a property named "9" appearing on the view.
    bytes.set(embed::setElement(bytes.get(), 9, embed::fromDouble(1.0)));
    CHECK(embed::isUndefined(embed::getElement(bytes.get(), 9)));
    CHECK(embed::toDouble(embed::getProperty(bytes.get(), "length")) == 2.0);
}

TEST_CASE("setElement fills an Array, length and all") {
    // A real Array, built the way a host has one to hand: parsed rather than
    // constructed, so nothing here reaches past the embed API for a fixture.
    embed::CallResult parsed = embed::parseJson("[10,20,30]");
    REQUIRE(!parsed.thrown);
    embed::Persistent arr{parsed.value};

    // Overwrite in range...
    arr.set(embed::setElement(arr.get(), 1, embed::fromUtf8("two")));
    CHECK(embed::toUtf8(embed::getElement(arr.get(), 1)) == "two");
    CHECK(embed::toDouble(embed::getProperty(arr.get(), "length")) == 3.0);

    // ...and past the end, which GROWS it. This is the gap the generic element
    // path closed: setElement used to reach requirePlainObject's fatal here,
    // so a host could READ an array element and could not write one.
    arr.set(embed::setElement(arr.get(), 3, embed::fromDouble(40.0)));
    CHECK(embed::toDouble(embed::getProperty(arr.get(), "length")) == 4.0);
    CHECK(embed::toDouble(embed::getElement(arr.get(), 3)) == 40.0);

    // The plain-object half is unchanged: an integer key is the canonical
    // numeric string, and the write is a definition rather than an assignment.
    embed::Persistent obj{embed::createObject()};
    obj.set(embed::setElement(obj.get(), 3, embed::fromUtf8("third")));
    CHECK(embed::toUtf8(embed::getElement(obj.get(), 3)) == "third");
}

TEST_CASE("a host-built typed array survives collections and round-trips through a call") {
    embed::Persistent view{embed::createTypedArray(embed::elements::Int32, 4)};
    for (uint32_t i = 0; i < 4; ++i) {
        view.set(embed::setElement(view.get(), i, embed::fromDouble((i + 1) * 3.0)));
    }

    const uint64_t bitsBefore = view.get().rawBits();
    runtime::rtHeap().collect();
    runtime::rtHeap().collect();
    // A moving collector relocated it and the Persistent answers the new
    // address: the identity survived, the pointer did not.
    CHECK(view.get().rawBits() != bitsBefore);
    CHECK(embed::isTypedArray(view.get()));

    embed::TypedArrayInfo info = embed::typedArrayInfo(view.get());
    REQUIRE(static_cast<bool>(info));
    CHECK(info.elementCount == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        int32_t element = 0;
        std::memcpy(&element, info.data + i * sizeof(int32_t), sizeof(int32_t));
        CHECK(element == static_cast<int32_t>((i + 1) * 3));
    }

    // And read back the way the PROGRAM would: through the dynamic-call path,
    // which is this file's stand-in for compiled code. The callback sums the
    // view it is handed, so a wrong address answers a wrong number rather than
    // crashing — the failure a rooting bug actually has.
    embed::Persistent sum{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            if (args.empty()) return embed::fromDouble(-1.0);
            double total = 0.0;
            const uint32_t count = embed::typedArrayInfo(args[0]).elementCount;
            for (uint32_t i = 0; i < count; ++i) {
                total += embed::toDouble(embed::getElement(args[0], i));
            }
            return embed::fromDouble(total);
        },
        1)};
    const embed::Value arg = view.get();
    embed::CallResult result = embed::call(sum.get(), embed::undefined(), {&arg, 1});
    CHECK(!result.thrown);
    CHECK(result.value.asNumber() == 30.0);
}
