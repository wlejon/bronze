#include <doctest/doctest.h>

#include <stdexcept>

#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"

using namespace bronze;

TEST_CASE("ArrayHeader element access and bounds") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ArrayHeader*> arr(ArrayHeader::create(heap, 4));
    REQUIRE(arr.get() != nullptr);
    CHECK(arr.get()->header.flags == HeapKind::Array);
    CHECK(arr.get()->length == 0);
    CHECK(arr.get()->capacity == 4);

    Rooted<Value> v0(Value::fromDouble(10.0));
    Rooted<Value> v1(Value::fromDouble(20.0));

    arr.get()->setElem(heap, 0, v0);
    CHECK(arr.get()->length == 1);

    arr.get()->setElem(heap, 1, v1);
    CHECK(arr.get()->length == 2);

    CHECK(arr.get()->getElem(0).asNumber() == 10.0);
    CHECK(arr.get()->getElem(1).asNumber() == 20.0);
    CHECK(arr.get()->getElem(2).isUndefined());

    // Sparse write grows the array and fills intermediate slots with holes
    Rooted<Value> v_sparse(Value::fromDouble(99.0));
    arr.get()->setElem(heap, 10, v_sparse);
    CHECK(arr.get()->length == 11);
    CHECK(arr.get()->getElem(10).asNumber() == 99.0);
    CHECK(arr.get()->hasElem(10));
    CHECK(arr.get()->getElem(5).isUndefined());
    CHECK(!arr.get()->hasElem(5));
}

#include "abi/bronze_abi.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze::runtime;

inline uint64_t customJoinNumber(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(42.0).rawBits();
}

inline uint64_t customJoinBool(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(true).rawBits();
}

TEST_CASE("Array.prototype.join safely converts non-string join result without type confusion") {
    ShadowStackFrame frame;

    // Create an inner array [1]
    Rooted<Value> inner{Value(bronze_create_array(1))};
    Rooted<Value> one{Value::fromDouble(1.0)};
    inner.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, one);

    // Provide a custom join method returning a number
    Rooted<Value> joinKey{rtMakeString("join")};
    Rooted<Value> fnNum{rtNativeFunction(customJoinNumber, 0, "join", 0)};
    bronze_elem_set(inner.get().rawBits(), joinKey.get().rawBits(), fnNum.get().rawBits(), false);

    // Create outer array [inner, "end"]
    Rooted<Value> outer{Value(bronze_create_array(2))};
    outer.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, inner);
    Rooted<Value> endStr{rtMakeString("end")};
    outer.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, endStr);

    uint64_t resBits = arrayJoin(0, outer.get().rawBits(), 0, nullptr);
    CHECK_FALSE(rtExceptionPending());
    Rooted<Value> res{Value(resBits)};
    REQUIRE(res.get().isString());
    CHECK(rtUtf8Chars(res.get().asString<StringHeader>()) == "42,end");

    // Provide a custom join method returning a boolean
    Rooted<Value> fnBool{rtNativeFunction(customJoinBool, 0, "join", 0)};
    bronze_elem_set(inner.get().rawBits(), joinKey.get().rawBits(), fnBool.get().rawBits(), false);

    resBits = arrayJoin(0, outer.get().rawBits(), 0, nullptr);
    CHECK_FALSE(rtExceptionPending());
    res = Value(resBits);
    REQUIRE(res.get().isString());
    CHECK(rtUtf8Chars(res.get().asString<StringHeader>()) == "true,end");
}

