#include <doctest/doctest.h>

#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"

using namespace bronze;

static uint64_t dummyAdd(uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    (void)thisBits;
    Value a0 = (argc > 0) ? Value(argvBits[0]) : Value::fromUndefined();
    Value a1 = (argc > 1) ? Value(argvBits[1]) : Value::fromUndefined();
    double a = a0.isNumber() ? a0.asNumber() : 0.0;
    double b = a1.isNumber() ? a1.asNumber() : 0.0;
    return Value::fromDouble(a + b).rawBits();
}

TEST_CASE("FunctionHeader dynamic call execution and arity adaptation") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<FunctionHeader*> fn(FunctionHeader::create(heap, dummyAdd, nullptr, 2));
    REQUIRE(fn.get() != nullptr);

    Value args[2] = {Value::fromDouble(10.0), Value::fromDouble(20.0)};
    Value res = fn.get()->call(Value::fromUndefined(), 2, args);
    CHECK(res.isNumber());
    CHECK(res.asNumber() == 30.0);

    // Call with missing arguments (arity adaptation)
    Value singleArg[1] = {Value::fromDouble(15.0)};
    Value resAdapt = fn.get()->call(Value::fromUndefined(), 1, singleArg);
    CHECK(resAdapt.isNumber());
    CHECK(resAdapt.asNumber() == 15.0);
}
