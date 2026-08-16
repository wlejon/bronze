#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"

using namespace bronze;

static uint64_t dummyAdd(uint64_t envBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    (void)envBits;
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

    Rooted<FunctionHeader*> fn(FunctionHeader::create(heap, dummyAdd, Value::fromUndefined(), 2));
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

// ---- the two own data properties, and the count that is not one of them ----
//
// `arity` and `length` are different numbers about the same parameter list, and
// the whole reason `length` is a second field. `arity` is a fact about the
// CALLING CONVENTION — how many slots a short call is padded to — while
// `length` is 10.2.10's own property, which 15.1.5 ExpectedArgumentCount stops
// counting at the first parameter with a default or the rest one. Collapsing
// them would make `function f(a, b = 1)` report 2, which is not what the
// language says and not what a call needs.

TEST_CASE("a function object's name and length are separate from its call arity") {
    Heap heap;
    ShadowStackFrame frame;

    // Straight from `create`: neither property has been set, which is the
    // state a native builtin stays in. A null `name` is "never recorded" and
    // not the empty string an anonymous function really has.
    Rooted<FunctionHeader*> raw(FunctionHeader::create(heap, dummyAdd, Value::fromUndefined(), 2));
    CHECK(raw.get()->arity == 2);
    CHECK(raw.get()->name == nullptr);
    CHECK(raw.get()->length == 0);

    // `BRONZE_ABI_FN_NAME_NONE` is how generated code says "no name", and it
    // leaves BOTH absent — including a length that was passed alongside it,
    // because the two are created together or not at all.
    runtime::rtSetFunctionNameAndLength(raw.get(), BRONZE_ABI_FN_NAME_NONE, 7);
    CHECK(raw.get()->name == nullptr);
    CHECK(raw.get()->length == 0);

    // A real name key. The key registry is what makes the string immortal and
    // non-moving, which is why the header may hold a raw pointer to it.
    const uint32_t nameKey = 4001;
    bronze_register_key_string(nameKey, "adder");
    Rooted<FunctionHeader*> named(FunctionHeader::create(heap, dummyAdd, Value::fromUndefined(), 2));
    runtime::rtSetFunctionNameAndLength(named.get(), nameKey, 1);
    REQUIRE(named.get()->name != nullptr);
    CHECK(runtime::rtUtf8Chars(named.get()->name) == "adder");
    // The padding arity and the `length` property disagree, which is the point.
    CHECK(named.get()->arity == 2);
    CHECK(named.get()->length == 1);
    // And the name is the registry's own header, not a copy — a copy would move
    // under the collector and leave the field dangling.
    CHECK(named.get()->name == runtime::rtKeyHeader(nameKey));
}
