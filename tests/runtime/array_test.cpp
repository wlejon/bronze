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
