// `DirectCallee` (runtime/call_out.h): the binding a runtime operation makes
// ONCE for a callee it will call per element.
//
// What no oracle case can reach from JS, and what this file is therefore for:
//   * the refusals. `bind` answers false for a callable PROXY, and JS cannot
//     observe that through `Array.prototype.sort` because bronze's step-1
//     validation rejects a proxy comparator before the binding is attempted.
//     The refusal still has to be right, because the day that validation is
//     widened the binding is what stands between a proxy and a call straight
//     to a `code` pointer the proxy does not own.
//   * the GC contract. A binding is a FACT and not a pointer: `call` re-reads
//     `code` and `env_record` through the caller's root every time. Proving
//     that needs a collection between the `bind` and the `call`, at a moment
//     of the test's choosing — which from JS is a wish and here is one line.
//
// The ordinary paths are pinned by tests/oracle/cases/array_sort_callout.js;
// this file does not repeat them.

#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/call_out.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"
#include "../stack_scrub.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// Answers a + b, so a successful call is visible in the result and a call that
// went to the wrong code pointer is not merely "no crash".
uint64_t addTwo(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    const double a = argc > 0 ? Value(argv[0]).asNumber() : 0.0;
    const double b = argc > 1 ? Value(argv[1]).asNumber() : 0.0;
    return Value::fromDouble(a + b).rawBits();
}

// A MOVABLE function object, unlike `rtNativeFunction`'s by-code-pointer
// singleton: `bronze_create_function` allocates on the collected heap, which is
// what makes the relocation test below a real one.
Value movableFunction(uint32_t arity) {
    return Value(bronze_create_function(addTwo, arity, /*length=*/arity,
                                        BRONZE_ABI_FN_NAME_NONE,
                                        BRONZE_ABI_FN_FLAGS_ORDINARY,
                                        Value::fromUndefined().rawBits()));
}

uint64_t callWith(const DirectCallee& direct, Rooted<Value>& callee, double a, double b) {
    const uint64_t argv[2] = {Value::fromDouble(a).rawBits(), Value::fromDouble(b).rawBits()};
    return direct.call(callee, Value::fromUndefined().rawBits(), 2, argv);
}

}  // namespace

TEST_CASE("DirectCallee binds an ordinary function and calls its code") {
    ShadowStackFrame frame;

    Rooted<Value> fn{movableFunction(2)};
    DirectCallee direct;
    REQUIRE(direct.bind(fn.get(), 2));
    CHECK(direct.bound());
    CHECK(Value(callWith(direct, fn, 3.0, 4.0)).asNumber() == 7.0);

    // More arguments than formals needs no padding, so it binds too.
    DirectCallee wide;
    CHECK(wide.bind(fn.get(), 5));
}

TEST_CASE("DirectCallee refuses every callee the generic path would have to re-check") {
    ShadowStackFrame frame;

    DirectCallee direct;

    // Not an object at all.
    CHECK_FALSE(direct.bind(Value::fromUndefined(), 2));
    CHECK_FALSE(direct.bind(Value::fromDouble(1.0), 2));
    CHECK_FALSE(direct.bound());

    // An object that is not a function.
    Rooted<Value> plain{Value(bronze_create_object())};
    CHECK_FALSE(direct.bind(plain.get(), 2));

    // UNDER-arity: the helper would pad the missing formals with undefined, and
    // the binding does not pad, so it declines and the caller keeps the helper.
    Rooted<Value> wide{movableFunction(4)};
    CHECK_FALSE(direct.bind(wide.get(), 2));
    CHECK(direct.bind(wide.get(), 4));

    // `arity == 0` is the variadic native's marking and not "takes nothing", so
    // any argument count reaches it.
    Rooted<Value> variadic{movableFunction(0)};
    CHECK(direct.bind(variadic.get(), 0));
    CHECK(direct.bind(variadic.get(), 3));

    // A callable PROXY. It answers IsCallable, and a call to it must run the
    // handler's `apply` trap — so it has no `code` of its own to call and the
    // binding must decline. This is the case JS cannot reach today.
    Rooted<Value> target{movableFunction(2)};
    Rooted<Value> handler{Value(bronze_create_object())};
    const uint64_t proxyArgs[2] = {target.get().rawBits(), handler.get().rawBits()};
    Rooted<Value> proxy{Value(rtProxyConstructor(0, 0, 2, proxyArgs))};
    REQUIRE(proxy.get().isObject());
    REQUIRE(rtIsCallableValue(proxy.get()));
    CHECK_FALSE(direct.bind(proxy.get(), 2));
}

TEST_CASE("a bound callee is re-read through its root, so it may move between bind and call") {
    ShadowStackFrame frame;

    // The root's slot is on the C++ heap, the function made and bound in a
    // frame of its own and its address kept hidden: a stack word naming it
    // would pin it in place (stack_scrub.h).
    auto fnRoot = bronze::test::heapRooted<Rooted<Value>>();
    Rooted<Value>& fn = *fnRoot;
    DirectCallee direct;
    const uint64_t before = bronze::test::fromOwnFrame([&] {
        fn.set(movableFunction(2));
        REQUIRE(direct.bind(fn.get(), 2));
        return bronze::test::hideBits(fn.get().rawBits());
    });

    // The collection a comparator's own allocation would have caused, at a
    // moment the test picks. A semispace copy relocates everything live, so the
    // function object the binding was made against is now at a different
    // address and its `code`/`env_record` fields live somewhere new.
    bronze::test::scrubStack();
    rtHeap().collect();
    CHECK(bronze::test::fromOwnFrame([&] { return bronze::test::hideBits(fn.get().rawBits()); }) != before);

    // The binding is still good, because it never held the address.
    CHECK(direct.bound());
    CHECK(Value(callWith(direct, fn, 20.0, 22.0)).asNumber() == 42.0);

    // And again, so a second relocation is covered too.
    rtHeap().collect();
    CHECK(Value(callWith(direct, fn, 1.0, 2.0)).asNumber() == 3.0);
}
