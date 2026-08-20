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
#include <memory>
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
    // The string is built in its own statement, not inline as a sibling
    // argument: argument evaluation order is unspecified, `fromUtf8` allocates,
    // and a compiler that reads `obj.get()` first hands `setElement` the
    // address `obj` had BEFORE the collection that moved it. embed.h's GC
    // contract spells this out; every allocating argument below follows it.
    embed::Persistent third{embed::fromUtf8("third")};
    obj.set(embed::setElement(obj.get(), 3, third.get()));

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

    // Persistent, not a plain Value: this method is used again after calls
    // that allocate, and a raw copy would name the address it had before the
    // first one collected.
    embed::Persistent thenMethod{embed::getProperty(p.get(), "then")};
    CHECK(embed::isFunction(thenMethod.get()));

    embed::call(thenMethod.get(), p.get(), std::vector<embed::Value>{fn1.get()});
    embed::call(thenMethod.get(), p.get(), std::vector<embed::Value>{fn2.get()});

    // Before resolving: no callbacks run.
    CHECK(order.empty());

    // Resolve the promise.
    embed::Persistent hello{embed::fromUtf8("hello")};
    embed::resolvePromise(p.get(), hello.get());

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
    embed::call(thenMethod.get(), p.get(), std::vector<embed::Value>{fn3.get()});
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

    embed::Persistent catchMethod{embed::getProperty(p.get(), "catch")};
    CHECK(embed::isFunction(catchMethod.get()));
    embed::call(catchMethod.get(), p.get(), std::vector<embed::Value>{catchHandler.get()});

    CHECK(caught.empty());

    embed::Persistent reason{embed::fromUtf8("bad error")};
    embed::rejectPromise(p.get(), reason.get());

    CHECK(caught.empty());
    CHECK(embed::microtasksPending());

    embed::drainMicrotasks();
    CHECK(!embed::microtasksPending());
    CHECK(caught == "bad error");

    // First settle wins: a subsequent resolve or reject on already settled promise is ignored.
    embed::Persistent ignored{embed::fromUtf8("ignored")};
    embed::resolvePromise(p.get(), ignored.get());
    embed::Persistent alsoIgnored{embed::fromUtf8("also ignored")};
    embed::rejectPromise(p.get(), alsoIgnored.get());
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

    embed::Persistent thenMethod{embed::getProperty(p.get(), "then")};
    embed::call(thenMethod.get(), p.get(), std::vector<embed::Value>{fn.get()});

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
    CHECK(bronze_tls_block_addr()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS);
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
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
    embed::Persistent two{embed::fromUtf8("two")};
    arr.set(embed::setElement(arr.get(), 1, two.get()));
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
    embed::Persistent third{embed::fromUtf8("third")};
    obj.set(embed::setElement(obj.get(), 3, third.get()));
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

TEST_CASE("globalValue resolves the ladder a compiled read resolves, and reports absence") {
    // A builtin answers first and cannot be shadowed by the host — the same
    // rule bronze_global_get applies, which is the point of sharing its order.
    embed::registerGlobal("Math", embed::fromDouble(13.0));
    embed::GlobalValue math = embed::globalValue("Math");
    REQUIRE(math.found);
    CHECK(embed::isObject(math.value));
    CHECK(embed::toDouble(embed::getProperty(math.value, "E")) > 2.7);

    // A host-registered name answers where no builtin does.
    embed::registerGlobal("engineHandle", embed::fromDouble(7.0));
    embed::GlobalValue host = embed::globalValue("engineHandle");
    REQUIRE(host.found);
    CHECK(host.value.asNumber() == 7.0);

    // An own property of globalThis IS a global (9.1.1.4.1), and the probe
    // sees one the moment it exists — here written through the embed API
    // itself, standing in for a program's `globalThis.assigned = ...`.
    embed::Persistent globalThis{embed::globalValue("globalThis").value};
    embed::setProperty(globalThis.get(), "assignedGlobal", embed::fromDouble(3.0));
    embed::GlobalValue assigned = embed::globalValue("assignedGlobal");
    REQUIRE(assigned.found);
    CHECK(assigned.value.asNumber() == 3.0);

    // Absence is an answer, not a fatal — the whole reason this exists beside
    // bronze_global_get.
    embed::GlobalValue missing = embed::globalValue("noSuchGlobalAnywhere");
    CHECK(!missing.found);
    CHECK(embed::isUndefined(missing.value));
}

TEST_CASE("hasHostGlobal probes the registry and only the registry") {
    CHECK(!embed::hasHostGlobal("neverRegisteredName"));
    // Registry membership is what a manifest diff needs, so a registered
    // undefined still counts — the host DID answer the manifest.
    embed::registerGlobal("registeredUndefined", embed::undefined());
    CHECK(embed::hasHostGlobal("registeredUndefined"));
    // Builtins are not host globals: `JSON` resolves for a program without
    // any host at all, and a loader diffing a manifest must not be told the
    // host provided it. (Not `Math`: the globalValue case above registers
    // that name on purpose, and cases share one process.)
    CHECK(!embed::hasHostGlobal("JSON"));
}

TEST_CASE("construct reaches a builtin constructor the way a compiled `new` does") {
    embed::GlobalValue errorCtor = embed::globalValue("Error");
    REQUIRE(errorCtor.found);
    embed::Persistent ctor{errorCtor.value};

    embed::Persistent message{embed::fromUtf8("made by the host")};
    const embed::Value arg = message.get();
    embed::CallResult made = embed::construct(ctor.get(), {&arg, 1});
    REQUIRE(!made.thrown);
    CHECK(embed::isObject(made.value));
    embed::Persistent err{made.value};
    CHECK(embed::toUtf8(embed::getProperty(err.get(), "message")) == "made by the host");

    // A non-constructor is the TypeError the construct path already raises,
    // reported as a thrown result rather than a fatal.
    embed::CallResult bad = embed::construct(embed::fromDouble(3.0), {});
    CHECK(bad.thrown);
    CHECK(embed::isObject(bad.value));
}

TEST_CASE("globalValue plus construct builds a live Proxy from the host") {
    // The pair this API exists for: dataset, el.style, getComputedStyle and
    // localStorage are all "a Proxy whose traps are host functions", and a
    // host can now build one without a line of compiled JS.
    embed::GlobalValue proxyCtor = embed::globalValue("Proxy");
    REQUIRE(proxyCtor.found);
    embed::Persistent ctor{proxyCtor.value};

    embed::Persistent target{embed::createObject()};
    embed::Persistent getTrap{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value> args) -> embed::Value {
            // get(target, key, receiver): answer "trap:<key>" for any key.
            if (args.size() < 2) return embed::undefined();
            return embed::fromUtf8("trap:" + embed::toUtf8(args[1]));
        },
        3)};
    embed::Persistent traps{embed::createObject()};
    traps.set(embed::setProperty(traps.get(), "get", getTrap.get()));

    embed::Value ctorArgs[2] = {target.get(), traps.get()};
    embed::CallResult made = embed::construct(ctor.get(), ctorArgs);
    REQUIRE(!made.thrown);
    embed::Persistent proxy{made.value};

    // A read through the generic property path runs the host trap.
    CHECK(embed::toUtf8(embed::getProperty(proxy.get(), "fontSize")) == "trap:fontSize");
    CHECK(embed::toUtf8(embed::getProperty(proxy.get(), "color")) == "trap:color");
}

TEST_CASE("setProperty defines properties on a function value") {
    // URL.createObjectURL: a namespace that IS a function, carrying statics.
    embed::Persistent url{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromUtf8("url-instance");
        },
        1)};
    embed::Persistent createObjectURL{embed::makeFunction(
        [](embed::Value, std::span<const embed::Value>) -> embed::Value {
            return embed::fromUtf8("blob:bronze");
        })};

    url.set(embed::setProperty(url.get(), "createObjectURL", createObjectURL.get()));
    url.set(embed::setProperty(url.get(), "version", embed::fromDouble(2.0)));

    runtime::rtHeap().collect();

    // Both land where a class `static` would, and read back through the same
    // path a program's `URL.createObjectURL` read takes.
    CHECK(embed::toDouble(embed::getProperty(url.get(), "version")) == 2.0);
    embed::Persistent method{embed::getProperty(url.get(), "createObjectURL")};
    REQUIRE(embed::isFunction(method.get()));
    embed::CallResult result = embed::call(method.get(), url.get(), {});
    CHECK(!result.thrown);
    CHECK(embed::toUtf8(result.value) == "blob:bronze");

    // The function is still a function: callable, and its statics did not
    // disturb that.
    embed::CallResult called = embed::call(url.get(), embed::undefined(), {});
    CHECK(!called.thrown);
    CHECK(embed::toUtf8(called.value) == "url-instance");
}

namespace {
// The deferred-finalizer probe: written through `data` by a capture-free
// lambda, because HandleDestructor is a plain function pointer.
struct DeferredProbe {
    int freed = 0;
    std::string echoed;
};
}  // namespace

TEST_CASE("a Deferred handle destructor waits for the drain and may make embed calls") {
    DeferredProbe probe;
    embed::makeHandle(
        &probe,
        [](void* p) {
            auto* state = static_cast<DeferredProbe*>(p);
            state->freed++;
            // The whole point of Deferred: this ALLOCATES on the bronze heap,
            // which an InSweep destructor must never do.
            state->echoed = embed::toUtf8(embed::fromUtf8("allocated in a finalizer"));
        },
        embed::Finalize::Deferred);
    // The handle value was dropped on the floor above, so the collection
    // proves it dead — and the destructor still must NOT have run yet.
    embed::collectGarbage();
    CHECK(probe.freed == 0);
    CHECK(embed::finalizersPending());

    embed::drainFinalizers();
    CHECK(probe.freed == 1);
    CHECK(probe.echoed == "allocated in a finalizer");
    CHECK(!embed::finalizersPending());

    // A second collection must not run it again: the sweep dropped the entry
    // when it queued it.
    embed::collectGarbage();
    embed::drainFinalizers();
    CHECK(probe.freed == 1);
}

TEST_CASE("drainMicrotasks runs deferred finalizers, and InSweep still runs in the sweep") {
    DeferredProbe deferred;
    embed::makeHandle(
        &deferred,
        [](void* p) { static_cast<DeferredProbe*>(p)->freed++; },
        embed::Finalize::Deferred);
    DeferredProbe inSweep;
    embed::makeHandle(&inSweep, [](void* p) { static_cast<DeferredProbe*>(p)->freed++; });

    embed::collectGarbage();
    // The default ran inside the collection, exactly as before this enum
    // existed; the deferred one is still waiting on the checkpoint.
    CHECK(inSweep.freed == 1);
    CHECK(deferred.freed == 0);

    // The checkpoint a frame loop already pumps is enough — no new call for
    // the host to learn.
    embed::drainMicrotasks();
    CHECK(deferred.freed == 1);
}

TEST_CASE("setProperty defines named properties on an Array") {
    embed::CallResult parsed = embed::parseJson("[1,2,3]");
    REQUIRE(!parsed.thrown);
    embed::Persistent arr{parsed.value};

    // A named property beside the elements — the write requirePlainObject
    // used to refuse — read back through the same generic path a program's
    // `arr.dirty` read takes.
    arr.set(embed::setProperty(arr.get(), "dirty", embed::fromBool(true)));
    embed::Persistent tag{embed::fromUtf8("host-tagged")};
    arr.set(embed::setProperty(arr.get(), "tag", tag.get()));

    runtime::rtHeap().collect();

    CHECK(embed::toBool(embed::getProperty(arr.get(), "dirty")));
    CHECK(embed::toUtf8(embed::getProperty(arr.get(), "tag")) == "host-tagged");

    // The elements and length are untouched: named properties live beside the
    // element store, not in it.
    CHECK(embed::toDouble(embed::getProperty(arr.get(), "length")) == 3.0);
    CHECK(embed::toDouble(embed::getElement(arr.get(), 2)) == 3.0);
}

TEST_CASE("Object.setPrototypeOf on a handle keeps handleData and the finalizer") {
    int destroyed = 0;
    {
        embed::Persistent handle{embed::makeHandle(
            &destroyed, [](void* p) { ++*static_cast<int*>(p); })};

        // The per-class prototype a wrapper layer wants: one object carrying
        // what every instance shares, instead of a closure per instance.
        embed::Persistent proto{embed::createObject()};
        embed::Persistent kind{embed::fromUtf8("wrapped")};
        proto.set(embed::setProperty(proto.get(), "kind", kind.get()));

        embed::GlobalValue objectNs = embed::globalValue("Object");
        REQUIRE(objectNs.found);
        embed::Persistent objectCtor{objectNs.value};
        embed::Persistent setProto{embed::getProperty(objectCtor.get(), "setPrototypeOf")};
        REQUIRE(embed::isFunction(setProto.get()));
        embed::CallResult swapped =
            embed::call(setProto.get(), embed::undefined(),
                        std::vector<embed::Value>{handle.get(), proto.get()});
        REQUIRE(!swapped.thrown);

        // The swap moved the shape, not the payload: brand and data pointer
        // are internal slots it cannot reach.
        CHECK(embed::handleData(handle.get()) == &destroyed);
        // And it did what it is for: the handle now inherits.
        CHECK(embed::toUtf8(embed::getProperty(handle.get(), "kind")) == "wrapped");

        runtime::rtHeap().collect();
        CHECK(embed::handleData(handle.get()) == &destroyed);
        CHECK(embed::toUtf8(embed::getProperty(handle.get(), "kind")) == "wrapped");
        CHECK(destroyed == 0);
    }
    // Dictionary mode changed nothing about liveness: the destructor fires
    // when the cell dies, exactly once.
    runtime::rtHeap().collect();
    CHECK(destroyed == 1);
    runtime::rtHeap().collect();
    CHECK(destroyed == 1);
}

TEST_CASE("isSymbol answers for symbol primitives and nothing else") {
    embed::GlobalValue symbolGlobal = embed::globalValue("Symbol");
    REQUIRE(symbolGlobal.found);
    embed::Persistent symbolFn{symbolGlobal.value};
    embed::Persistent desc{embed::fromUtf8("host-probe")};
    embed::CallResult made = embed::call(symbolFn.get(), embed::undefined(),
                                         std::vector<embed::Value>{desc.get()});
    REQUIRE(!made.thrown);
    embed::Persistent sym{made.value};

    CHECK(embed::isSymbol(sym.get()));
    CHECK(!embed::isSymbol(embed::undefined()));
    CHECK(!embed::isSymbol(embed::null()));
    CHECK(!embed::isSymbol(embed::fromDouble(3.0)));
    embed::Persistent str{embed::fromUtf8("Symbol(host-probe)")};
    CHECK(!embed::isSymbol(str.get()));
    embed::Persistent obj{embed::createObject()};
    CHECK(!embed::isSymbol(obj.get()));
}

TEST_CASE("a handle born on a prototype is a class instance") {
    int freed = 0;
    {
        // The class: a host constructor, its shared methods on the prototype
        // the READ mints (setProperty refuses `prototype` by name; getProperty
        // answers the real slot-backed object, and it is plain).
        embed::Persistent ctor{embed::makeFunction(
            [](embed::Value, std::span<const embed::Value>) -> embed::Value {
                return embed::undefined();
            })};
        embed::Persistent proto{embed::getProperty(ctor.get(), "prototype")};
        REQUIRE(embed::isObject(proto.get()));
        embed::Persistent method{embed::makeFunction(
            [](embed::Value self, std::span<const embed::Value>) -> embed::Value {
                // The per-class method's whole job: unwrap the receiver.
                int* p = static_cast<int*>(embed::handleData(self));
                return embed::fromDouble(p ? 42.0 : -1.0);
            })};
        proto.set(embed::setProperty(proto.get(), "probe", method.get()));

        // The instance: born on the prototype, not swapped onto it.
        embed::Persistent inst{embed::makeHandle(
            &freed, [](void* p) { ++*static_cast<int*>(p); },
            embed::Finalize::InSweep, proto.get())};

        CHECK(embed::handleData(inst.get()) == &freed);
        embed::Persistent probeFn{embed::getProperty(inst.get(), "probe")};
        REQUIRE(embed::isFunction(probeFn.get()));
        embed::CallResult answered = embed::call(probeFn.get(), inst.get(), {});
        REQUIRE(!answered.thrown);
        CHECK(answered.value.asNumber() == 42.0);

        // Through the same helper a compiled `x instanceof Ctor` calls — a raw
        // bronze_* helper, so the test opens the frame embed:: calls open for
        // themselves.
        embed::Persistent stranger{embed::createObject()};
        {
            ShadowStackFrame frame;
            CHECK(bronze_instanceof(inst.get().rawBits(), ctor.get().rawBits()));
            CHECK(!bronze_instanceof(stranger.get().rawBits(), ctor.get().rawBits()));
        }

        runtime::rtHeap().collect();
        CHECK(embed::handleData(inst.get()) == &freed);
        {
            ShadowStackFrame frame;
            CHECK(bronze_instanceof(inst.get().rawBits(), ctor.get().rawBits()));
        }
        CHECK(freed == 0);
    }
    // Being a class instance changed nothing about the handle's death.
    runtime::rtHeap().collect();
    CHECK(freed == 1);
}

TEST_CASE("construct on a host constructor births instances on its prototype") {
    int freed = 0;
    {
        // Filled after the prototype is read; the ctor body reads it through
        // the shared_ptr so the capture and the decoration can happen in
        // either order.
        auto protoSlot = std::make_shared<embed::Persistent>();
        int* freedPtr = &freed;
        embed::Persistent ctor{embed::makeFunction(
            [protoSlot, freedPtr](embed::Value self,
                                  std::span<const embed::Value> args) -> embed::Value {
                if (!args.empty() && embed::toBool(args[0])) {
                    // A class whose instances ARE handles: return the cell and
                    // bronze_construct's object-return rule delivers it in
                    // place of the ordinary instance.
                    return embed::makeHandle(
                        freedPtr, [](void* p) { ++*static_cast<int*>(p); },
                        embed::Finalize::InSweep, protoSlot->get());
                }
                // Ordinary path: `this` arrived already born on the prototype;
                // the body only decorates it.
                embed::setProperty(self, "decorated", embed::fromBool(true));
                return embed::undefined();
            })};
        embed::Persistent proto{embed::getProperty(ctor.get(), "prototype")};
        embed::Persistent kind{embed::fromUtf8("engine")};
        proto.set(embed::setProperty(proto.get(), "kind", kind.get()));
        protoSlot->set(proto.get());

        // (a) the ordinary instance bronze_construct allocates itself.
        embed::CallResult plain = embed::construct(ctor.get(), {});
        REQUIRE(!plain.thrown);
        embed::Persistent inst{plain.value};
        CHECK(embed::toBool(embed::getProperty(inst.get(), "decorated")));
        CHECK(embed::toUtf8(embed::getProperty(inst.get(), "kind")) == "engine");
        {
            ShadowStackFrame frame;
            CHECK(bronze_instanceof(inst.get().rawBits(), ctor.get().rawBits()));
        }

        // (b) the handle the body substitutes.
        embed::CallResult made =
            embed::construct(ctor.get(), std::vector<embed::Value>{embed::fromBool(true)});
        REQUIRE(!made.thrown);
        embed::Persistent handle{made.value};
        CHECK(embed::handleData(handle.get()) == freedPtr);
        CHECK(embed::toUtf8(embed::getProperty(handle.get(), "kind")) == "engine");
        {
            ShadowStackFrame frame;
            CHECK(bronze_instanceof(handle.get().rawBits(), ctor.get().rawBits()));
        }
        CHECK(freed == 0);
    }
    runtime::rtHeap().collect();
    CHECK(freed == 1);
}

TEST_CASE("the eval global defers to the host's hook, and refuses without one") {
    // `Function`'s sibling seam (embed.h setDynamicEvalHook): `eval` is a
    // provided global whose body performs 19.2.1 step 2 itself and hands
    // source text to the hook — or throws a catchable TypeError when no host
    // installed one, which is the standalone story.
    embed::GlobalValue evalGlobal = embed::globalValue("eval");
    REQUIRE(evalGlobal.found);
    embed::Persistent evalFn{evalGlobal.value};
    CHECK(embed::isFunction(evalFn.get()));

    // Step 2 without a hook: a non-string comes straight back, because no
    // compilation is involved in the first place.
    {
        std::vector<embed::Value> args{embed::fromDouble(5.0)};
        embed::CallResult r = embed::call(evalFn.get(), embed::undefined(), args);
        CHECK(!r.thrown);
        CHECK(r.value.asNumber() == 5.0);
    }

    // Source text without a hook: the refusal, catchable, as a real object.
    {
        std::vector<embed::Value> args{embed::fromUtf8("1 + 1")};
        embed::CallResult r = embed::call(evalFn.get(), embed::undefined(), args);
        CHECK(r.thrown);
        CHECK(r.value.isObject());
    }

    // With a hook: the hook sees the source and its answer is the call's
    // value, uninspected. The argument vector is rebuilt per call — a raw
    // Value held across an allocating call would name a pre-collection
    // address under the gc-stress rerun.
    std::string seen;
    embed::setDynamicEvalHook([&seen](embed::Value source) -> embed::Value {
        seen = embed::toUtf8(source);
        return embed::fromDouble(2.0);
    });
    {
        std::vector<embed::Value> args{embed::fromUtf8("1 + 1")};
        embed::CallResult r = embed::call(evalFn.get(), embed::undefined(), args);
        CHECK(!r.thrown);
        CHECK(r.value.asNumber() == 2.0);
        CHECK(seen == "1 + 1");
    }

    // Cleared, the refusal is back — the hook is not a ratchet.
    embed::setDynamicEvalHook({});
    {
        std::vector<embed::Value> args{embed::fromUtf8("2 + 2")};
        embed::CallResult r = embed::call(evalFn.get(), embed::undefined(), args);
        CHECK(r.thrown);
    }
}

TEST_CASE("externalizeArrayBuffer pins a buffer's bytes and both sides see one store") {
    // A program's Float32Array, then the host's window onto the SAME bytes:
    // the shared-typed-array seam a bridge is built on. The pointer must
    // survive collections — that is the contract's whole point.
    embed::Persistent view{embed::createTypedArray(embed::elements::Float32, 4)};
    embed::setElement(view.get(), 0, embed::fromDouble(1.5));

    embed::ExternalBytes ext = embed::externalizeArrayBuffer(view.get());
    REQUIRE(static_cast<bool>(ext));
    CHECK(ext.byteLength == 16);
    float host0 = 0.0f;
    std::memcpy(&host0, ext.data, sizeof host0);
    CHECK(host0 == 1.5f);

    // The host's write IS the program's read, across a collection.
    const float written = 42.0f;
    std::memcpy(ext.data + sizeof(float), &written, sizeof written);
    embed::collectGarbage();
    CHECK(embed::toDouble(embed::getElement(view.get(), 1)) == 42.0);
    // And the program's write lands in the same block the host still holds.
    embed::setElement(view.get(), 2, embed::fromDouble(7.0));
    float host2 = 0.0f;
    std::memcpy(&host2, ext.data + 2 * sizeof(float), sizeof host2);
    CHECK(host2 == 7.0f);

    // Idempotent: a second call is one more reference on the SAME block.
    embed::ExternalBytes again = embed::externalizeArrayBuffer(view.get());
    REQUIRE(static_cast<bool>(again));
    CHECK(again.data == ext.data);
    CHECK(again.store == ext.store);
    embed::releaseExternalStore(again.store);

    // A view window: externalizing a subrange view answers offset bytes.
    embed::Persistent buffer{embed::typedArrayBuffer(view.get())};
    embed::Persistent sub{
        embed::createTypedArrayView(embed::elements::Float32, buffer.get(), 8, 2)};
    embed::ExternalBytes win = embed::externalizeArrayBuffer(sub.get());
    REQUIRE(static_cast<bool>(win));
    CHECK(win.data == ext.data + 8);
    CHECK(win.byteLength == 8);
    embed::releaseExternalStore(win.store);
    embed::releaseExternalStore(ext.store);
}

namespace {
struct StoreProbe {
    int freed = 0;
    uint8_t* bytes = nullptr;
};
}  // namespace

TEST_CASE("createExternalArrayBuffer reads host bytes in place and shares a live store") {
    static StoreProbe probe;  // static: the deleter runs from the drain, after locals
    probe = StoreProbe{};
    probe.bytes = static_cast<uint8_t*>(std::malloc(16));
    REQUIRE(probe.bytes != nullptr);
    const float seed = 3.5f;
    std::memcpy(probe.bytes, &seed, sizeof seed);

    {
        embed::Persistent buffer{embed::createExternalArrayBuffer(
            probe.bytes, 16,
            [](void*, uint8_t* bytes) {
                probe.freed++;
                std::free(bytes);
            },
            nullptr)};
        REQUIRE(embed::isArrayBuffer(buffer.get()));
        embed::Persistent view{
            embed::createTypedArrayView(embed::elements::Float32, buffer.get(), 0, 4)};
        // The program reads the host's bytes, uncopied...
        CHECK(embed::toDouble(embed::getElement(view.get(), 0)) == 3.5);
        // ...and its write lands in them.
        embed::setElement(view.get(), 1, embed::fromDouble(9.0));
        float host1 = 0.0f;
        std::memcpy(&host1, probe.bytes + sizeof(float), sizeof host1);
        CHECK(host1 == 9.0f);

        // A SECOND buffer over the same bytes shares the live store rather
        // than refusing: its own deleter is redundant and runs immediately.
        static int duplicateDeleterRan = 0;
        duplicateDeleterRan = 0;
        embed::Persistent second{embed::createExternalArrayBuffer(
            probe.bytes, 16, [](void*, uint8_t*) { duplicateDeleterRan++; }, nullptr)};
        REQUIRE(embed::isArrayBuffer(second.get()));
        CHECK(duplicateDeleterRan == 1);
        CHECK(probe.freed == 0);
        embed::Persistent secondView{
            embed::createTypedArrayView(embed::elements::Float32, second.get(), 0, 4)};
        CHECK(embed::toDouble(embed::getElement(secondView.get(), 1)) == 9.0);
    }

    // Both buffers dropped: the store's LAST reference goes at the deferred
    // drain — never mid-collection — and the governing deleter runs once.
    embed::collectGarbage();
    embed::drainFinalizers();
    embed::collectGarbage();
    embed::drainFinalizers();
    CHECK(probe.freed == 1);
}
