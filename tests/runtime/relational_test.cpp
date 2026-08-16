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
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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

// 13.10.1 step 1: an OBJECT operand is ToPrimitive'd with hint NUMBER, so
// `valueOf` is asked before `toString`. The answers alone cannot show that — an
// object defining both halves orders plausibly either way — so what is checked
// here is the CALL LOG and the ORDER of the two conversions.
//
// LeftFirst is the part a whole-program case can only observe indirectly.
// `a > b` asks IsLessThan(b, a) and passes LeftFirst false, which is what keeps
// the SOURCE's left operand converting first for all four operators.
namespace {

std::vector<std::string> g_relCalls;

// Never reached under hint number while `valueOf` answers a primitive, which
// is exactly what the log below is checking.
uint64_t relToString(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    g_relCalls.push_back("toString");
    return rtMakeString("s").rawBits();
}

// Two probes, told apart by the number their `valueOf` answers with and by the
// name they log.
double g_leftValue = 0.0;
double g_rightValue = 0.0;

uint64_t leftValueOf(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    g_relCalls.push_back("L");
    return Value::fromDouble(g_leftValue).rawBits();
}

uint64_t rightValueOf(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    g_relCalls.push_back("R");
    return Value::fromDouble(g_rightValue).rawBits();
}

const NativeMethod kLeftMethods[] = {{"valueOf", leftValueOf, 0}, {"toString", relToString, 0}};
const NativeMethod kRightMethods[] = {{"valueOf", rightValueOf, 0}, {"toString", relToString, 0}};

Value leftProbe() {
    Rooted<Value> obj{Value(bronze_create_object())};
    rtDefineMethods(obj, kLeftMethods, 2);
    return obj.get();
}

Value rightProbe() {
    Rooted<Value> obj{Value(bronze_create_object())};
    rtDefineMethods(obj, kRightMethods, 2);
    return obj.get();
}

std::string relLog() {
    std::string out;
    for (const std::string& s : g_relCalls) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

}  // namespace

TEST_CASE("a relational operator converts an object with hint number") {
    ShadowStackFrame frame;
    g_leftValue = 1.0;
    g_rightValue = 2.0;

    Rooted<Value> left{leftProbe()};
    Rooted<Value> right{rightProbe()};

    // `valueOf` answers a primitive, so `toString` is never reached — which is
    // the whole difference between hint number and hint string.
    g_relCalls.clear();
    CHECK(bronze_rel_lt(left.get().rawBits(), right.get().rawBits()));
    CHECK(relLog() == "L,R");

    // All four operators convert the source's LEFT operand first, including the
    // two that swap IsLessThan's arguments.
    g_relCalls.clear();
    CHECK_FALSE(bronze_rel_gt(left.get().rawBits(), right.get().rawBits()));
    CHECK(relLog() == "L,R");

    g_relCalls.clear();
    CHECK(bronze_rel_le(left.get().rawBits(), right.get().rawBits()));
    CHECK(relLog() == "L,R");

    g_relCalls.clear();
    CHECK_FALSE(bronze_rel_ge(left.get().rawBits(), right.get().rawBits()));
    CHECK(relLog() == "L,R");

    // One object against a plain number converts only the object.
    g_relCalls.clear();
    CHECK(bronze_rel_lt(left.get().rawBits(), num(5.0)));
    CHECK(relLog() == "L");

    // Two numbers convert nothing at all, which is the shape a typed comparison
    // that stayed boxed arrives in.
    g_relCalls.clear();
    CHECK(bronze_rel_lt(num(1.0), num(2.0)));
    CHECK(relLog().empty());
}
