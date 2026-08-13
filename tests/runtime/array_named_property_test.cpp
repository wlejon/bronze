// An array's own properties that are not elements — `length` and the named
// ones — asked directly of rt_prop_array.cpp.
//
// The oracle cases pin what a PROGRAM sees, which is the contract. What they
// cannot see is the storage decision underneath: a named property lives in the
// side object `ArrayHeader::properties` already held for a match array's
// `index` and an `arguments` object's `callee`, and an array that never takes
// one still has no side object at all. That is what keeps the element path
// unchanged, and it is exactly the kind of thing a later change could undo with
// every oracle case still passing — a new field on ArrayHeader, or a named
// write that eagerly builds the box, would cost every array in the program and
// nothing here would notice unless it is pinned.
//
// The other half is ArraySetLength's storage effects (10.4.2.4): what shrinking
// leaves behind. `length` moving is observable from JS; the dropped slots
// holding HOLES rather than their old values is not, until the array grows
// again — which is precisely why it belongs in a unit test.

#include <doctest/doctest.h>

#include <string>

#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

Value newArray(uint32_t count) {
    Rooted<Value> arr{Value(bronze_create_array(0))};
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i) + 1.0)};
        arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, v);
    }
    return arr.get();
}

SetRefusal setNamed(Rooted<Value>& arr, const char* name, Value value) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> val{value};
    return rtArrayNamedSet(arr, key, val);
}

bool ownNamed(Value arr, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    PropertyInfo info;
    return rtArrayOwnNamed(arr, key.get().asString<StringHeader>(), info);
}

std::string namedKeys(Value arr) {
    std::string out;
    for (StringHeader* k : rtArrayOwnNamedKeys(arr)) {
        if (!out.empty()) out += ",";
        out += rtAsciiChars(k);
    }
    return out;
}

}  // namespace

TEST_CASE("an array pays nothing for named properties until it has one") {
    ShadowStackFrame frame;

    Rooted<Value> arr{newArray(3)};
    // No side object, so the element write path's integrity check is one load
    // and a not-taken branch (integrity.h), and nothing walks a shape.
    CHECK(arr.get().asObject<ArrayHeader>()->properties.isUndefined());
    CHECK_FALSE(ownNamed(arr.get(), "foo"));
    CHECK(namedKeys(arr.get()).empty());

    CHECK(setNamed(arr, "foo", Value::fromDouble(7.0)) == SetRefusal::None);
    CHECK(arr.get().asObject<ArrayHeader>()->properties.isObject());
    CHECK(ownNamed(arr.get(), "foo"));
    CHECK(arr.get().asObject<ArrayHeader>()->length == 3);
}

TEST_CASE("named keys come back in insertion order, and a delete takes a position with it") {
    ShadowStackFrame frame;

    Rooted<Value> arr{newArray(1)};
    setNamed(arr, "a", Value::fromDouble(1.0));
    setNamed(arr, "b", Value::fromDouble(2.0));
    setNamed(arr, "c", Value::fromDouble(3.0));
    CHECK(namedKeys(arr.get()) == "a,b,c");

    Rooted<Value> key{rtMakeString("b")};
    CHECK(rtArrayNamedDelete(arr.get(), key.get().asString<StringHeader>()));
    CHECK_FALSE(ownNamed(arr.get(), "b"));
    CHECK(namedKeys(arr.get()) == "a,c");

    // Re-added, so it is a NEW insertion and goes to the end — the dictionary
    // the delete left behind keeps the order explicitly.
    setNamed(arr, "b", Value::fromDouble(9.0));
    CHECK(namedKeys(arr.get()) == "a,c,b");

    // Deleting a name that was never there is the state delete wants, and an
    // array with no side object at all answers the same way.
    Rooted<Value> other{newArray(1)};
    Rooted<Value> missing{rtMakeString("nope")};
    CHECK(rtArrayNamedDelete(other.get(), missing.get().asString<StringHeader>()));
    CHECK(other.get().asObject<ArrayHeader>()->properties.isUndefined());
}

TEST_CASE("a non-extensible array refuses a new named property and keeps the old ones") {
    ShadowStackFrame frame;

    Rooted<Value> arr{newArray(2)};
    setNamed(arr, "kept", Value::fromDouble(1.0));
    const uint64_t argv[1] = {arr.get().rawBits()};
    rtObjectPreventExtensions(0, 0, 1, argv);

    CHECK(setNamed(arr, "fresh", Value::fromDouble(2.0)) == SetRefusal::NotExtensible);
    CHECK_FALSE(ownNamed(arr.get(), "fresh"));
    CHECK(setNamed(arr, "kept", Value::fromDouble(3.0)) == SetRefusal::None);
}

TEST_CASE("ArraySetLength leaves holes behind rather than the values it dropped") {
    ShadowStackFrame frame;

    Rooted<Value> arr{newArray(4)};
    CHECK(rtArraySetLength(arr, Value::fromDouble(2.0)) == SetRefusal::None);
    CHECK(arr.get().asObject<ArrayHeader>()->length == 2);

    // Grown back over the SAME element block — the capacity never shrank — so
    // the slots would still hold 3 and 4 if the truncation had only moved
    // `length`. They are holes, which is what makes the regrown indices absent
    // rather than present with stale values.
    CHECK(rtArraySetLength(arr, Value::fromDouble(4.0)) == SetRefusal::None);
    CHECK(arr.get().asObject<ArrayHeader>()->length == 4);
    CHECK_FALSE(arr.get().asObject<ArrayHeader>()->hasElem(2));
    CHECK_FALSE(arr.get().asObject<ArrayHeader>()->hasElem(3));
    CHECK(arr.get().asObject<ArrayHeader>()->getElem(2).isUndefined());
    CHECK(arr.get().asObject<ArrayHeader>()->hasElem(0));
}

TEST_CASE("ArraySetLength answers the integrity levels") {
    ShadowStackFrame frame;

    SUBCASE("frozen: a change is refused, the same value is not") {
        Rooted<Value> arr{newArray(3)};
        const uint64_t argv[1] = {arr.get().rawBits()};
        rtObjectFreeze(0, 0, 1, argv);
        CHECK(rtArraySetLength(arr, Value::fromDouble(0.0)) == SetRefusal::NotWritable);
        CHECK(arr.get().asObject<ArrayHeader>()->length == 3);
        CHECK(rtArraySetLength(arr, Value::fromDouble(3.0)) == SetRefusal::None);
    }

    SUBCASE("sealed: growing works, shrinking stops at the highest live element") {
        Rooted<Value> arr{newArray(3)};
        const uint64_t argv[1] = {arr.get().rawBits()};
        rtObjectSeal(0, 0, 1, argv);
        CHECK(rtArraySetLength(arr, Value::fromDouble(5.0)) == SetRefusal::None);
        CHECK(arr.get().asObject<ArrayHeader>()->length == 5);
        // The two slots the growth added are holes and therefore deletable, so
        // the shrink gets that far and no further: index 2 is a live element a
        // sealed array may not remove.
        CHECK(rtArraySetLength(arr, Value::fromDouble(0.0)) == SetRefusal::NotWritable);
        CHECK(arr.get().asObject<ArrayHeader>()->length == 3);
    }
}
