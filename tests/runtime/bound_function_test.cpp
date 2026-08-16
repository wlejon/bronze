// Bound functions below the compiler: the argument prepending and receiver
// replacement of 10.4.1.1, the construct-path unwrapping of 10.4.1.2, the
// `name`/`length` pair of 20.2.3.2 — and the GC contract that motivates the
// design: everything a bound function closes over lives in one heap cell the
// payload scan forwards, so a forced collection between binding and calling
// must change nothing (mirrors gc_test.cpp's forcing pattern).

#include <doctest/doctest.h>

#include <initializer_list>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

bool isFunction(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// Targets that report which slot of the call they saw. Raw doubles only, so
// nothing recorded here is a heap pointer a collection could stale.
uint64_t reportThis(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    return thisBits;
}

uint64_t reportArg0(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return argc > 0 ? argv[0] : Value::fromUndefined().rawBits();
}

uint64_t reportArg2(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return argc > 2 ? argv[2] : Value::fromUndefined().rawBits();
}

uint64_t reportArgc(uint64_t, uint64_t, uint32_t argc, const uint64_t*) {
    return Value::fromDouble(static_cast<double>(argc)).rawBits();
}

// What a constructor body saw, recorded as plain numbers for the reason above.
double g_ctorArg0 = 0.0;
uint32_t g_ctorArgc = 0;
uint64_t recordingCtor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    g_ctorArgc = argc;
    g_ctorArg0 = argc > 0 ? Value(argv[0]).asNumber() : -1.0;
    return Value::fromUndefined().rawBits();
}

// `target.bind(thisArg, bound...)` through the same member table the property
// path answers `f.bind` from.
Value bindWith(Rooted<Value>& target, Rooted<Value>& thisArg, std::initializer_list<double> bound) {
    Rooted<Value> bind{rtFunctionMethod("bind")};
    REQUIRE(isFunction(bind.get()));
    Value args[4] = {thisArg.get(), Value::fromUndefined(), Value::fromUndefined(),
                     Value::fromUndefined()};
    uint32_t argc = 1;
    for (double d : bound) args[argc++] = Value::fromDouble(d);
    return bind.get().asObject<FunctionHeader>()->call(target.get(), argc, args);
}

Value callWith(Rooted<Value>& fn, Value thisArg, std::initializer_list<double> extra) {
    Value args[4];
    uint32_t argc = 0;
    for (double d : extra) args[argc++] = Value::fromDouble(d);
    return fn.get().asObject<FunctionHeader>()->call(thisArg, argc, args);
}

}  // namespace

TEST_CASE("[[Call]] prepends the bound arguments and replaces the receiver") {
    ShadowStackFrame frame;

    Rooted<Value> thisArg{Value::fromDouble(42.0)};

    Rooted<Value> t0{rtNativeFunction(reportThis, 0)};
    Rooted<Value> b0{bindWith(t0, thisArg, {})};
    REQUIRE(isFunction(b0.get()));
    // 10.4.1.1 step 5: whatever receiver the call arrives with, the target
    // sees [[BoundThis]].
    CHECK(callWith(b0, Value::fromDouble(99.0), {}).asNumber() == 42.0);

    Rooted<Value> t1{rtNativeFunction(reportArg0, 0)};
    Rooted<Value> b1{bindWith(t1, thisArg, {7.0})};
    // The bound argument comes FIRST: arg 0 is 7 whatever the call passes.
    CHECK(callWith(b1, Value::fromUndefined(), {9.0}).asNumber() == 7.0);

    Rooted<Value> t2{rtNativeFunction(reportArgc, 0)};
    Rooted<Value> b2{bindWith(t2, thisArg, {1.0, 2.0})};
    // Two bound plus one passed is three seen.
    CHECK(callWith(b2, Value::fromUndefined(), {3.0}).asNumber() == 3.0);
}

TEST_CASE("bind of bind nests: each layer prepends its own arguments") {
    ShadowStackFrame frame;

    Rooted<Value> thisA{Value::fromDouble(1.0)};
    Rooted<Value> thisB{Value::fromDouble(2.0)};

    // arg 2 of the innermost call is: outer bound args first? No — the INNER
    // binding's args come first, because the outer trampoline prepends its own
    // and then calls the inner bound function, which prepends again. So the
    // final order is inner, outer, call — which is f.bind(a, x).bind(b, y)(z)
    // seeing (x, y, z), exactly 20.2.3.2 applied twice.
    Rooted<Value> target{rtNativeFunction(reportArg2, 0)};
    Rooted<Value> inner{bindWith(target, thisA, {10.0})};
    Rooted<Value> outer{bindWith(inner, thisB, {20.0})};
    CHECK(callWith(outer, Value::fromUndefined(), {30.0}).asNumber() == 30.0);

    // And the receiver is the INNER binding's: the outer layer's thisB is
    // handed to the inner trampoline, which discards it for thisA.
    Rooted<Value> tThis{rtNativeFunction(reportThis, 0)};
    Rooted<Value> innerT{bindWith(tThis, thisA, {})};
    Rooted<Value> outerT{bindWith(innerT, thisB, {})};
    CHECK(callWith(outerT, Value::fromDouble(3.0), {}).asNumber() == 1.0);
}

TEST_CASE("the binding cell survives forced collections") {
    ShadowStackFrame frame;

    Rooted<Value> bound{Value::fromUndefined()};
    {
        // The target and receiver roots die with this block: after it, the
        // only thing keeping the target function, the bound-this and the
        // bound-arguments array alive is the cell inside `bound` — which is
        // exactly what the EnvHeader arrangement exists to guarantee.
        Rooted<Value> target{rtNativeFunction(reportArg0, 0)};
        Rooted<Value> thisArg{Value::fromDouble(5.0)};
        bound.set(bindWith(target, thisArg, {11.0}));
        REQUIRE(isFunction(bound.get()));
    }

    for (int i = 0; i < 3; ++i) rtHeap().collect();
    CHECK(callWith(bound, Value::fromUndefined(), {}).asNumber() == 11.0);

    for (int i = 0; i < 3; ++i) rtHeap().collect();
    CHECK(callWith(bound, Value::fromUndefined(), {99.0}).asNumber() == 11.0);
}

TEST_CASE("construct unwraps to the target with the bound arguments prepended") {
    ShadowStackFrame frame;

    Rooted<Value> target{rtNativeFunction(recordingCtor, 0)};
    Rooted<Value> thisArg{Value::fromDouble(-7.0)};  // ignored by [[Construct]]
    Rooted<Value> bound{bindWith(target, thisArg, {13.0})};

    g_ctorArgc = 0;
    g_ctorArg0 = 0.0;
    Value extra[1] = {Value::fromDouble(21.0)};
    Rooted<Value> extraRoot{extra[0]};
    Value instance{bronze_construct(bound.get().rawBits(), 1,
                                    reinterpret_cast<const uint64_t*>(extra))};
    CHECK_FALSE(rtExceptionPending());
    // The body ran with (13, 21) — bound first — and the instance is an
    // ordinary object built from the TARGET's prototype path, not something
    // the trampoline's [[Call]] half produced.
    CHECK(g_ctorArgc == 2);
    CHECK(g_ctorArg0 == 13.0);
    CHECK(instance.isObject());
    CHECK(instance.asObject<HeapObjectHeader>()->flags == HeapKind::Plain);
}

TEST_CASE("a bound native carries no invented name or length") {
    ShadowStackFrame frame;

    // A native builtin records neither `name` nor `length`
    // (rt_builtins.h::rtNativeFunction says why), so a bound function over one
    // must inherit the ABSENCE — `name == nullptr` — rather than answer
    // "bound " over a name that was never recorded.
    Rooted<Value> target{rtNativeFunction(reportArg0, 0)};
    Rooted<Value> thisArg{Value::fromUndefined()};
    Rooted<Value> bound{bindWith(target, thisArg, {})};
    CHECK(bound.get().asObject<FunctionHeader>()->name == nullptr);
}
