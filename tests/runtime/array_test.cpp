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

    // Out-of-bounds write is a hard error; observe it via a throwing
    // fatal handler (the default handler aborts, which doctest can't see)
    setFatalHandler([](const char* msg) { throw std::runtime_error(msg); });
    Rooted<Value> v_oob(Value::fromDouble(99.0));
    CHECK_THROWS_AS(arr.get()->setElem(heap, 10, v_oob), std::runtime_error);
    setFatalHandler(nullptr);
}
