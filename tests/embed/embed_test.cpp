// Runtime-level tests of the embedding API: everything here goes through the
// same heap, registries and call machinery a real host would, and none of it
// needs the LLVM backend — the "compiled program" side of each seam is played
// by the runtime's own dynamic-call path. The embed-gc-stress run re-executes
// all of it with a collection forced at every allocation, which is where the
// rooting mistakes this module can make actually surface.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
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
    // The two halves a compiled read arrives with: the key string registered
    // under the index lowering assigned it, and the host's value in the
    // registry. A heap STRING value on purpose — it moves at every
    // collection, so this checks the registry is a real root source and not a
    // cache of stale bits.
    embed::registerGlobal("engineName", embed::fromUtf8("bro"));
    bronze_register_key_string(0, "engineName");

    runtime::rtHeap().collect();

    Value v{bronze_global_get(0)};
    CHECK(v.isString());
    CHECK(embed::toUtf8(v) == "bro");

    // Re-registration replaces, and the read sees the replacement — the
    // reason host globals stay out of the builtin cache.
    embed::registerGlobal("engineName", embed::fromDouble(2.0));
    Value replaced{bronze_global_get(0)};
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
