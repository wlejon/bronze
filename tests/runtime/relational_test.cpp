// The four relational operators, below the compiler: ECMA-262 13.10 and the
// 13.10.1 IsLessThan it is written over.
//
// The oracle cases pin what a whole program prints; these pin the two facts
// that were wrong and are easiest to break again — that IsLessThan has THREE
// answers and that its third one folds to false for all four operators, and
// that two Strings are compared by code unit before any conversion is
// considered.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

uint64_t num(double d) { return Value::fromDouble(d).rawBits(); }

const double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

TEST_CASE("every relational operator is false when an operand is NaN") {
    ShadowStackFrame frame;

    // 13.10.1 step 4.c produces *undefined* for a NaN operand, and 13.10 maps
    // undefined to false for all four operators. `<=` and `>=` are the two the
    // old `!(a > b)` lowering got wrong, because a negation maps that same
    // undefined to true.
    CHECK_FALSE(bronze_rel_lt(num(kNaN), num(1.0)));
    CHECK_FALSE(bronze_rel_gt(num(kNaN), num(1.0)));
    CHECK_FALSE(bronze_rel_le(num(kNaN), num(1.0)));
    CHECK_FALSE(bronze_rel_ge(num(kNaN), num(1.0)));

    CHECK_FALSE(bronze_rel_lt(num(1.0), num(kNaN)));
    CHECK_FALSE(bronze_rel_gt(num(1.0), num(kNaN)));
    CHECK_FALSE(bronze_rel_le(num(1.0), num(kNaN)));
    CHECK_FALSE(bronze_rel_ge(num(1.0), num(kNaN)));

    // NaN against itself, which `<=` and `>=` would otherwise call true for the
    // reason `x <= x` is true of every other number.
    CHECK_FALSE(bronze_rel_le(num(kNaN), num(kNaN)));
    CHECK_FALSE(bronze_rel_ge(num(kNaN), num(kNaN)));
}

TEST_CASE("<= and >= still answer the equal case they exist for") {
    ShadowStackFrame frame;

    CHECK(bronze_rel_le(num(1.0), num(2.0)));
    CHECK(bronze_rel_le(num(2.0), num(2.0)));
    CHECK_FALSE(bronze_rel_le(num(3.0), num(2.0)));

    CHECK_FALSE(bronze_rel_ge(num(1.0), num(2.0)));
    CHECK(bronze_rel_ge(num(2.0), num(2.0)));
    CHECK(bronze_rel_ge(num(3.0), num(2.0)));

    CHECK(bronze_rel_lt(num(1.0), num(2.0)));
    CHECK_FALSE(bronze_rel_lt(num(2.0), num(2.0)));
    CHECK(bronze_rel_gt(num(3.0), num(2.0)));

    // Both zeroes are one number to a comparison, however they are signed.
    CHECK(bronze_rel_le(num(-0.0), num(0.0)));
    CHECK(bronze_rel_ge(num(-0.0), num(0.0)));
    CHECK_FALSE(bronze_rel_lt(num(-0.0), num(0.0)));

    const double inf = std::numeric_limits<double>::infinity();
    CHECK(bronze_rel_lt(num(-inf), num(inf)));
    CHECK(bronze_rel_le(num(inf), num(inf)));
    CHECK_FALSE(bronze_rel_lt(num(inf), num(inf)));
}

TEST_CASE("two strings are compared by code unit, and nothing is converted") {
    ShadowStackFrame frame;

    Rooted<Value> a{rtMakeString("a")};
    Rooted<Value> b{rtMakeString("b")};
    CHECK(bronze_rel_lt(a.get().rawBits(), b.get().rawBits()));
    CHECK_FALSE(bronze_rel_lt(b.get().rawBits(), a.get().rawBits()));
    CHECK(bronze_rel_le(a.get().rawBits(), a.get().rawBits()));
    CHECK_FALSE(bronze_rel_le(b.get().rawBits(), a.get().rawBits()));
    CHECK(bronze_rel_ge(b.get().rawBits(), a.get().rawBits()));

    // Step 3 fires before step 4 ever looks at the digits, so the numeric
    // reading of these two never happens: "2" is above "1".
    Rooted<Value> two{rtMakeString("2")};
    Rooted<Value> ten{rtMakeString("10")};
    CHECK_FALSE(bronze_rel_lt(two.get().rawBits(), ten.get().rawBits()));
    CHECK(bronze_rel_lt(num(2.0), num(10.0)));
}

TEST_CASE("one string and one number is the numeric branch, step 4") {
    ShadowStackFrame frame;

    // Step 3 needs BOTH operands to be Strings. With one of each it fails and
    // ToNumeric decides, so a numeric string is read as its number.
    Rooted<Value> ten{rtMakeString("10")};
    CHECK_FALSE(bronze_rel_lt(ten.get().rawBits(), num(9.0)));
    CHECK(bronze_rel_gt(ten.get().rawBits(), num(9.0)));

    // A string that does not parse is NaN, which is step 4.c's undefined, so
    // all four are false — including the two that a negation would flip.
    Rooted<Value> word{rtMakeString("a")};
    CHECK_FALSE(bronze_rel_lt(word.get().rawBits(), num(1.0)));
    CHECK_FALSE(bronze_rel_gt(word.get().rawBits(), num(1.0)));
    CHECK_FALSE(bronze_rel_le(word.get().rawBits(), num(1.0)));
    CHECK_FALSE(bronze_rel_ge(word.get().rawBits(), num(1.0)));

    // The empty string is 0 through ToNumber, and `null` is 0 as well — both
    // reached through step 4, which is why they order against numbers at all.
    Rooted<Value> empty{rtMakeString("")};
    CHECK(bronze_rel_lt(empty.get().rawBits(), num(1.0)));
    CHECK(bronze_rel_ge(Value::fromNull().rawBits(), num(0.0)));
    CHECK_FALSE(bronze_rel_gt(Value::fromNull().rawBits(), num(0.0)));

    // `undefined` is NaN through ToNumber, so it orders against nothing.
    CHECK_FALSE(bronze_rel_le(Value::fromUndefined().rawBits(), num(0.0)));
    CHECK_FALSE(bronze_rel_ge(Value::fromUndefined().rawBits(), num(0.0)));

    // A boolean is 1 or 0 in a numeric position.
    CHECK(bronze_rel_lt(Value::fromBool(false).rawBits(), Value::fromBool(true).rawBits()));
    CHECK(bronze_rel_le(Value::fromBool(true).rawBits(), num(1.0)));
}
