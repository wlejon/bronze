// Math.random.
//
// No oracle case can pin this and none ever should: the harness greps every
// case for the name and fails it, because a case's whole contract is
// byte-identical stdout. That is exactly why the behaviour is pinned HERE
// instead of nowhere — the three things a broken implementation gets wrong are
// the range (a shift by the wrong width, or a division by the wrong power of
// two, puts values at or above 1), the movement (a generator that is never
// stepped, or is re-seeded per call from a coarse clock, repeats), and the
// distribution (a generator whose high bits are constant still passes a naive
// range check).

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// The caller owns the ShadowStackFrame these roots attach to: a `Rooted` with
// no frame above it registers nowhere and protects nothing, which under
// BRONZE_GC_STRESS=1 is a stale pointer at the first allocation rather than a
// missing test.
Value randomFunction() {
    Rooted<Value> math{rtMathObject()};
    Rooted<Value> key{rtMakeString("random")};
    return math.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

double draw(Rooted<Value>& fn) {
    uint64_t block[1] = {Value::fromUndefined().rawBits()};
    return Value(bronze_dynamic_call(fn.get().rawBits(), Value::fromUndefined().rawBits(), 0,
                                     block))
        .asNumber();
}

}  // namespace

TEST_CASE("Math.random is a real member of the Math namespace") {
    ShadowStackFrame frame;
    Rooted<Value> fn{randomFunction()};
    REQUIRE(fn.get().isObject());
    // flags == 2 is a function object, which is what makes `const r = Math.random`
    // and `[1].map(Math.random)` ordinary rather than special-cased.
    CHECK(fn.get().asObject<HeapObjectHeader>()->flags == 2);
}

TEST_CASE("Math.random stays in [0, 1), moves, and is flat across the unit interval") {
    ShadowStackFrame frame;
    Rooted<Value> fn{randomFunction()};

    constexpr int kDraws = 20000;
    std::vector<int> decile(10, 0);
    double sum = 0.0;
    double first = draw(fn);
    bool moved = false;

    for (int i = 0; i < kDraws; ++i) {
        const double x = draw(fn);
        // 21.3.2.27: "a Number value with positive sign, greater than or equal
        // to +0 but strictly less than 1". Exactly 1.0 is the classic
        // off-by-one of a 53-bit conversion and would break `arr[floor(r*len)]`
        // once in a very long while, which is the worst kind of rarely.
        REQUIRE(x >= 0.0);
        REQUIRE(x < 1.0);
        if (x != first) moved = true;
        sum += x;
        decile[static_cast<size_t>(x * 10.0)] += 1;
    }

    CHECK(moved);
    for (int d = 0; d < 10; ++d) {
        // 2000 expected per bucket; a generator with a constant bit anywhere in
        // the top four leaves at least one of these empty.
        CHECK(decile[static_cast<size_t>(d)] > 1000);
    }
    // The mean of a uniform [0, 1) is 0.5 with a standard error of
    // 0.2887/sqrt(20000) = 0.002, so +/-0.02 is ten sigma: a real generator
    // will not trip it, and one that is skewed will.
    const double mean = sum / kDraws;
    CHECK(mean > 0.48);
    CHECK(mean < 0.52);
}
