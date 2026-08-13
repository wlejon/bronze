// Array.prototype.sort at the C++ level: STABILITY, which no oracle case can
// pin cheaply (it needs equal keys whose original order is observable), the
// undefined/hole ordering of SortIndexedProperties, and the throwing
// comparator's contract — the array is untouched, because the write-back had
// not begun. Under BRONZE_GC_STRESS=1 this file is also the rooting proof for
// the sorted-list buffer: the allocating comparator below forces a collection
// inside every comparison, and the list survives only if it is a heap array
// held through a root rather than a C++ buffer of stale Values.

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

bool isFunction(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// Compares by FLOOR, so 2.1 and 2.2 are "equal" and only a stable sort keeps
// them in input order — the fractional digits are the observer.
uint64_t floorComparator(uint64_t, uint64_t, uint32_t, const uint64_t* argv) {
    const double a = std::floor(Value(argv[0]).asNumber());
    const double b = std::floor(Value(argv[1]).asNumber());
    return Value::fromDouble(a - b).rawBits();
}

// The same comparison with an allocation in the middle, which under gc-stress
// is a collection in the middle: every object in the sort moves between every
// pair of comparisons.
uint64_t allocatingComparator(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> churn{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "churn"))};
    (void)churn;
    const double a = std::floor(args[0].asNumber());
    const double b = std::floor(args[1].asNumber());
    return Value::fromDouble(a - b).rawBits();
}

uint64_t throwingComparator(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return rtThrowTypeError("comparator refuses").rawBits();
}

Value arrayOf(std::initializer_list<double> values) {
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t at = 0;
    for (double d : values) {
        Rooted<Value> v{Value::fromDouble(d)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, v);
    }
    return out.get();
}

// `arr.sort(comparator)` the way a program reaches it.
Value sortWith(Rooted<Value>& arr, Value comparator) {
    Rooted<Value> cmp{comparator};
    Rooted<Value> sort{rtArrayMethod("sort")};
    REQUIRE(isFunction(sort.get()));
    Value args[1] = {cmp.get()};
    return sort.get().asObject<FunctionHeader>()->call(arr.get(), 1, args);
}

double elem(Rooted<Value>& arr, uint32_t i) {
    return arr.get().asObject<ArrayHeader>()->getElem(i).asNumber();
}

}  // namespace

TEST_CASE("sort is stable: equal keys keep their input order") {
    ShadowStackFrame frame;

    // Floor-equal pairs interleaved; the fractions record the input order.
    Rooted<Value> a{arrayOf({2.1, 1.1, 2.2, 1.2, 2.3, 1.3})};
    Rooted<Value> result{sortWith(a, rtNativeFunction(floorComparator, 2))};
    CHECK_FALSE(rtExceptionPending());
    // 23.1.3.30 sorts IN PLACE and answers the same array.
    CHECK(result.get().rawBits() == a.get().rawBits());

    const double expected[6] = {1.1, 1.2, 1.3, 2.1, 2.2, 2.3};
    for (uint32_t i = 0; i < 6; ++i) CHECK(elem(a, i) == expected[i]);
}

TEST_CASE("the sorted-list buffer stays rooted across an allocating comparator") {
    ShadowStackFrame frame;

    Rooted<Value> a{arrayOf({5.1, 3.1, 5.2, 1.1, 3.2, 1.2, 5.3, 1.3})};
    sortWith(a, rtNativeFunction(allocatingComparator, 2));
    CHECK_FALSE(rtExceptionPending());

    const double expected[8] = {1.1, 1.2, 1.3, 3.1, 3.2, 5.1, 5.2, 5.3};
    for (uint32_t i = 0; i < 8; ++i) CHECK(elem(a, i) == expected[i]);
}

TEST_CASE("default comparator is ToString order, undefined last, holes after that") {
    ShadowStackFrame frame;

    // Lexicographic, not numeric: "10" < "9".
    Rooted<Value> a{arrayOf({10.0, 9.0, 1.0})};
    sortWith(a, Value::fromUndefined());
    CHECK_FALSE(rtExceptionPending());
    CHECK(elem(a, 0) == 1.0);
    CHECK(elem(a, 1) == 10.0);
    CHECK(elem(a, 2) == 9.0);

    // [3, <hole>, undefined, 1] sorts to [1, 3, undefined, <hole>]: the
    // present elements first, the undefineds after them (never handed to a
    // comparator), and the holes DELETED to the tail.
    Rooted<Value> b{arrayOf({3.0, 0.0, 0.0, 1.0})};
    {
        Rooted<Value> u{Value::fromUndefined()};
        b.get().asObject<ArrayHeader>()->setElem(rtHeap(), 2, u);
        b.get().asObject<ArrayHeader>()->deleteElem(1);
    }
    sortWith(b, Value::fromUndefined());
    CHECK_FALSE(rtExceptionPending());
    CHECK(elem(b, 0) == 1.0);
    CHECK(elem(b, 1) == 3.0);
    CHECK(b.get().asObject<ArrayHeader>()->getElem(2).isUndefined());
    CHECK(b.get().asObject<ArrayHeader>()->hasElem(2));   // a stored undefined
    CHECK_FALSE(b.get().asObject<ArrayHeader>()->hasElem(3));  // a hole
    CHECK(b.get().asObject<ArrayHeader>()->length == 4);  // length is untouched
}

TEST_CASE("a throwing comparator leaves the array exactly as it was") {
    ShadowStackFrame frame;

    Rooted<Value> a{arrayOf({3.0, 1.0, 2.0})};
    sortWith(a, rtNativeFunction(throwingComparator, 2));
    CHECK(rtExceptionPending());
    rtClearException();

    // The write-back never began: reads happened, the list sort aborted, and
    // the receiver still holds its original bytes.
    CHECK(elem(a, 0) == 3.0);
    CHECK(elem(a, 1) == 1.0);
    CHECK(elem(a, 2) == 2.0);
}
