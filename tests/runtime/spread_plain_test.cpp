// Object spread and rest between plain objects (runtime/spread_plain.cpp): the
// copy off the source's shape chain, the remembered target layout, and the
// sources it has to hand back to the general walk.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/spread_plain.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

void put(Rooted<Value>& obj, const char* name, Value v) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> val{v};
    bronze_elem_set(obj.get().rawBits(), key.get().rawBits(), val.get().rawBits(), true);
}

Value get(Rooted<Value>& obj, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return Value(bronze_elem_get(obj.get().rawBits(), key.get().rawBits()));
}

uint64_t keyCount(Rooted<Value>& obj) {
    return obj.get().asObject<ObjectHeader>()->shape->ownKeysInInsertionOrder(true).size();
}

const Shape* shapeOf(Rooted<Value>& obj) {
    return obj.get().asObject<ObjectHeader>()->shape;
}

uint64_t noGetter(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(0.0).rawBits();
}

}  // namespace

TEST_CASE("a plain spread copies off the shape chain and the second copy takes the first one's layout") {
    ShadowStackFrame frame;
    rtForgetSpreadShapeHints();

    Rooted<Value> src{Value(bronze_create_object())};
    put(src, "color", Value::fromDouble(16777215.0));
    put(src, "alpha", Value::fromDouble(0.5));
    Rooted<Value> label{rtMakeString("white")};
    put(src, "texture", label.get());
    // More keys than the inline slots, so the copy sizes the slot block.
    for (int i = 0; i < 24; ++i) put(src, ("k" + std::to_string(i)).c_str(), Value::fromDouble(i));

    Rooted<Value> first{Value(bronze_create_object())};
    REQUIRE(rtSpreadPlain(first, src, nullptr));
    Rooted<Value> second{Value(bronze_create_object())};
    REQUIRE(rtSpreadPlain(second, src, nullptr));

    CHECK(shapeOf(first) == shapeOf(second));
    CHECK(keyCount(second) == 27);
    CHECK(get(second, "color").asNumber() == 16777215.0);
    CHECK(get(second, "alpha").asNumber() == 0.5);
    CHECK(get(second, "texture").rawBits() == label.get().rawBits());
    CHECK(get(second, "k23").asNumber() == 23.0);

    // The remembered layout does not decide what a slot may hold: the same
    // source shape with a value of another kind in a slot that held a number.
    Rooted<Value> other{Value(bronze_create_object())};
    put(other, "x", Value::fromDouble(1.5));
    Rooted<Value> a{Value(bronze_create_object())};
    REQUIRE(rtSpreadPlain(a, other, nullptr));
    put(other, "x", label.get());
    Rooted<Value> b{Value(bronze_create_object())};
    REQUIRE(rtSpreadPlain(b, other, nullptr));
    CHECK(get(a, "x").asNumber() == 1.5);
    CHECK(get(b, "x").rawBits() == label.get().rawBits());
}

TEST_CASE("a plain spread onto a target with keys overwrites in place and appends the rest") {
    ShadowStackFrame frame;
    rtForgetSpreadShapeHints();

    Rooted<Value> src{Value(bronze_create_object())};
    put(src, "alpha", Value::fromDouble(2.0));
    put(src, "width", Value::fromDouble(3.0));
    for (int round = 0; round < 3; ++round) {
        Rooted<Value> target{Value(bronze_create_object())};
        put(target, "color", Value::fromDouble(1.0));
        put(target, "alpha", Value::fromDouble(0.5));
        REQUIRE(rtSpreadPlain(target, src, nullptr));
        CHECK(keyCount(target) == 3);
        CHECK(get(target, "color").asNumber() == 1.0);
        CHECK(get(target, "alpha").asNumber() == 2.0);
        CHECK(get(target, "width").asNumber() == 3.0);
    }
}

TEST_CASE("a rest copy leaves out the excluded keys, whichever ones they are") {
    ShadowStackFrame frame;
    rtForgetSpreadShapeHints();

    Rooted<Value> src{Value(bronze_create_object())};
    put(src, "a", Value::fromDouble(1.0));
    put(src, "b", Value::fromDouble(2.0));
    put(src, "c", Value::fromDouble(3.0));

    // Two exclusion lists of one length: the layouts must not be confused.
    for (const char* dropped : {"a", "c", "a", "c"}) {
        Rooted<Value> excluded{Value(bronze_create_array(1))};
        Rooted<Value> name{rtMakeString(dropped)};
        bronze_array_append(excluded.get().rawBits(), name.get().rawBits());
        Rooted<Value> out{Value(bronze_create_object())};
        REQUIRE(rtSpreadPlain(out, src, &excluded));
        CHECK(keyCount(out) == 2);
        CHECK(get(out, dropped).isUndefined());
        CHECK(get(out, "b").asNumber() == 2.0);
    }
}

TEST_CASE("sources the shape chain does not describe are handed back untouched") {
    ShadowStackFrame frame;
    rtForgetSpreadShapeHints();

    SUBCASE("a dictionary-mode source") {
        Rooted<Value> src{Value(bronze_create_object())};
        put(src, "a", Value::fromDouble(1.0));
        put(src, "b", Value::fromDouble(2.0));
        Rooted<Value> key{rtMakeString("a")};
        bronze_elem_delete(src.get().rawBits(), key.get().rawBits(), true);
        Rooted<Value> out{Value(bronze_create_object())};
        CHECK_FALSE(rtSpreadPlain(out, src, nullptr));
        CHECK(keyCount(out) == 0);
    }
    SUBCASE("a source with an accessor") {
        Rooted<Value> src{Value(bronze_create_object())};
        put(src, "a", Value::fromDouble(1.0));
        Rooted<Value> key{rtMakeString("g")};
        Rooted<Value> getter{rtNativeFunction(noGetter, 0)};
        Rooted<Value> setter{Value::fromUndefined()};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), src, key, getter, setter, true);
        Rooted<Value> out{Value(bronze_create_object())};
        CHECK_FALSE(rtSpreadPlain(out, src, nullptr));
        CHECK(keyCount(out) == 0);
    }
    SUBCASE("a source with an index key") {
        Rooted<Value> src{Value(bronze_create_object())};
        put(src, "b", Value::fromDouble(1.0));
        put(src, "7", Value::fromDouble(2.0));
        Rooted<Value> out{Value(bronze_create_object())};
        CHECK_FALSE(rtSpreadPlain(out, src, nullptr));
        CHECK(keyCount(out) == 0);
    }
    SUBCASE("an array source and an array target") {
        Rooted<Value> arr{Value(bronze_create_array(0))};
        Rooted<Value> obj{Value(bronze_create_object())};
        CHECK_FALSE(rtSpreadPlain(obj, arr, nullptr));
        CHECK_FALSE(rtSpreadPlain(arr, obj, nullptr));
    }
}
