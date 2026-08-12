// The `arguments` object, below the compiler.
//
// The oracle case pins what ECMA-262 fixes about it — the count, the indices,
// the iteration, the unmapped write. What it deliberately does NOT pin is the
// object's identity, because bronze diverges there on purpose: `arguments` is
// an ordinary array, so `Array.isArray` and `instanceof Array` answer true
// where a spec engine answers false. A divergence that no test names is one
// somebody rediscovers, so it is named here rather than left to the doc.

#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/rt_internal.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

TEST_CASE("the arguments object holds every argument passed, in order") {
    ShadowStackFrame frame;

    const uint64_t argv[3] = {Value::fromDouble(10.0).rawBits(),
                              Value::fromDouble(20.0).rawBits(),
                              Value::fromDouble(30.0).rawBits()};

    Rooted<Value> none{Value(bronze_arguments_object(0, argv))};
    REQUIRE(none.get().isObject());
    CHECK(none.get().asObject<ArrayHeader>()->length == 0);

    Rooted<Value> all{Value(bronze_arguments_object(3, argv))};
    REQUIRE(all.get().isObject());
    ArrayHeader* arr = all.get().asObject<ArrayHeader>();
    REQUIRE(arr->length == 3);
    CHECK(arr->getElem(0).asNumber() == 10.0);
    CHECK(arr->getElem(1).asNumber() == 20.0);
    CHECK(arr->getElem(2).asNumber() == 30.0);
    // Past the end is `undefined`, which is what an index that is not an own
    // property reads as anywhere else.
    CHECK(arr->getElem(3).isUndefined());
}

TEST_CASE("the arguments object IS an array — the recorded divergence") {
    ShadowStackFrame frame;

    const uint64_t argv[1] = {Value::fromDouble(1.0).rawBits()};
    Rooted<Value> args{Value(bronze_arguments_object(1, argv))};
    REQUIRE(args.get().isObject());

    // `flags == 1` is what `Array.isArray`, `JSON.stringify` and the spread
    // fast paths all test. A spec engine's arguments object is an ordinary
    // object with an @@iterator, so all three would see it differently.
    // Changing this is a decision, not a fix: an `arguments` that aliased its
    // parameters would need a mapped object and would cost every call that has
    // one.
    CHECK(args.get().asObject<HeapObjectHeader>()->flags == 1);
}

TEST_CASE("bronze_arg_at answers undefined past the end rather than reading on") {
    // A function that owns an arguments object declares arity 0 so that
    // `FunctionHeader::call` does NOT pad a short call — `f(1)` and
    // `f(1, undefined)` must give `arguments.length` 1 and 2. Its wrapper
    // therefore reads its own parameters through this, and nothing else does.
    const uint64_t argv[2] = {Value::fromDouble(7.0).rawBits(),
                              Value::fromDouble(8.0).rawBits()};

    CHECK(Value(bronze_arg_at(2, argv, 0)).asNumber() == 7.0);
    CHECK(Value(bronze_arg_at(2, argv, 1)).asNumber() == 8.0);
    CHECK(Value(bronze_arg_at(2, argv, 2)).isUndefined());
    // The short call: argc says one, so index 1 is past the end even though
    // the storage behind it happens to hold a value.
    CHECK(Value(bronze_arg_at(1, argv, 1)).isUndefined());
    CHECK(Value(bronze_arg_at(0, argv, 0)).isUndefined());
}
