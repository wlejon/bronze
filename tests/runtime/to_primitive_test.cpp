// ECMA-262 7.1.1 ToPrimitive, at the level below the compiler.
//
// `cases/to_primitive` pins the answers a program sees. What is here is the one
// thing an end-to-end case cannot show cheaply: WHICH METHODS RAN, and in what
// order. The hint decides only that order, and getting it backwards produces
// the right answer for every object that defines one half — so a test that
// checked answers alone would pass with the pair reversed.
//
// Each probe's first method deliberately returns an OBJECT, which 7.1.1.1 step
// 3.d rejects, so the second one has to run and both get recorded.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// The call log. A file-scope vector rather than something threaded through the
// receiver, because a native method's only channels are its ABI arguments and
// there is no closure to carry one.
std::vector<std::string> g_calls;

// What each probe method answers, set per subcase. `undefined` means "answer
// with a fresh object", which is the value 7.1.1.1 rejects and the way this
// forces the second method to run.
//
// PERMANENT ROOTS, because a string answer is a heap object held across every
// allocation the rest of the test makes — building the probe object, interning
// its two keys, minting its two methods. A plain global would be a raw pointer
// the collector never updates, which under BRONZE_GC_STRESS is stale by the
// time the method it answers for runs.
Value g_toStringResult = Value::fromUndefined();
Value g_valueOfResult = Value::fromUndefined();

void rootResultsOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    rtHeap().add_permanent_root(&g_toStringResult);
    rtHeap().add_permanent_root(&g_valueOfResult);
}

uint64_t probeToString(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    g_calls.push_back("toString");
    if (g_toStringResult.isUndefined()) return bronze_create_object();
    return g_toStringResult.rawBits();
}

uint64_t probeValueOf(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    g_calls.push_back("valueOf");
    if (g_valueOfResult.isUndefined()) return bronze_create_object();
    return g_valueOfResult.rawBits();
}

const NativeMethod kProbeMethods[] = {
    {"toString", probeToString, 0},
    {"valueOf", probeValueOf, 0},
};

// An object carrying both halves as own properties, so the ordinary property
// walk finds them before `Object.prototype`'s.
Value probeObject() {
    Rooted<Value> obj{Value(bronze_create_object())};
    rtDefineMethods(obj, kProbeMethods, 2);
    return obj.get();
}

std::string joined() {
    std::string out;
    for (const std::string& s : g_calls) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}

void reset() {
    rootResultsOnce();
    g_calls.clear();
    g_toStringResult = Value::fromUndefined();
    g_valueOfResult = Value::fromUndefined();
}

std::string textOf(Value v) {
    REQUIRE(v.isString());
    return rtUtf8Chars(v.asString<StringHeader>());
}

}  // namespace

TEST_CASE("ToPrimitive tries the two methods in the order its hint names") {
    ShadowStackFrame frame;
    reset();

    SUBCASE("hint string asks toString first") {
        Rooted<Value> answer{rtMakeString("T")};
        g_toStringResult = answer.get();
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::String)};
        CHECK(joined() == "toString");
        CHECK(textOf(prim.get()) == "T");
    }

    SUBCASE("hint default asks valueOf first") {
        g_valueOfResult = Value::fromDouble(7.0);
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::Default)};
        CHECK(joined() == "valueOf");
        CHECK(prim.get().isNumber());
        CHECK(prim.get().asNumber() == 7.0);
    }

    // 7.1.1.1 takes number and default together — the hint is a two-way switch
    // whatever the clause calls its three values.
    SUBCASE("hint number is hint default's order") {
        g_valueOfResult = Value::fromDouble(7.0);
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::Number)};
        CHECK(joined() == "valueOf");
    }

    // Step 3.d: a result that is an Object is not accepted, so the OTHER method
    // runs. This is what makes `'' + { toString() {...} }` reach `toString` at
    // all — 20.1.3.7's `valueOf` answers with the receiver every time.
    SUBCASE("a method answering with an object is passed over") {
        Rooted<Value> answer{rtMakeString("T")};
        g_toStringResult = answer.get();
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::Default)};
        CHECK(joined() == "valueOf,toString");
        CHECK(textOf(prim.get()) == "T");
    }

    SUBCASE("and under hint string the pair runs the other way round") {
        g_valueOfResult = Value::fromDouble(5.0);
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::String)};
        CHECK(joined() == "toString,valueOf");
        CHECK(prim.get().isNumber());
        CHECK(prim.get().asNumber() == 5.0);
    }

    // Step 4. Thrown rather than fatal, because the clause names the error and
    // both callers are on `il::canThrow`'s list.
    SUBCASE("neither answering a primitive is the TypeError step 4 names") {
        Rooted<Value> obj{probeObject()};
        Rooted<Value> prim{rtToPrimitive(obj, ToPrimitiveHint::Default)};
        CHECK(joined() == "valueOf,toString");
        CHECK(rtExceptionPending());
        rtClearException();
        CHECK_FALSE(rtExceptionPending());
    }

    // Step 1: a primitive is returned untouched, and nothing is called.
    SUBCASE("a primitive input is already one") {
        Rooted<Value> n{Value::fromDouble(2.0)};
        CHECK(rtToPrimitive(n, ToPrimitiveHint::String).asNumber() == 2.0);
        Rooted<Value> s{rtMakeString("ab")};
        CHECK(textOf(rtToPrimitive(s, ToPrimitiveHint::Default)) == "ab");
        CHECK(joined().empty());
    }

    reset();
}

// 13.15.3 for `+`: ToPrimitive on BOTH operands, and only then the String test.
// Testing the raw operands would decide before converting, which for an object
// is the difference between a concatenation and a named ToNumber refusal.
TEST_CASE("dynamic + converts both operands before it decides on strings") {
    ShadowStackFrame frame;
    reset();

    SUBCASE("an object whose valueOf answers a number adds as a number") {
        g_valueOfResult = Value::fromDouble(7.0);
        Rooted<Value> obj{probeObject()};
        Rooted<Value> one{Value::fromDouble(1.0)};
        Value sum(bronze_dynamic_add(obj.get().rawBits(), one.get().rawBits()));
        CHECK(joined() == "valueOf");
        CHECK(sum.isNumber());
        CHECK(sum.asNumber() == 8.0);
    }

    // The same object against a string is a CONCATENATION, and of the converted
    // primitive rather than of the object: 13.15.3 asks for no hint either way,
    // so `valueOf` still answers first and its 7 is what gets stringified.
    SUBCASE("a string on the other side concatenates the converted primitive") {
        g_valueOfResult = Value::fromDouble(7.0);
        Rooted<Value> obj{probeObject()};
        Rooted<Value> empty{rtMakeString("")};
        Value sum(bronze_dynamic_add(empty.get().rawBits(), obj.get().rawBits()));
        CHECK(joined() == "valueOf");
        CHECK(textOf(sum) == "7");
    }

    reset();
}

// 7.1.17 ToString, which is what `String(x)` and a template substitution are —
// and which asks for hint STRING where `+` asks for none. The two disagreeing
// is the point rather than an accident.
TEST_CASE("ToString asks for hint string where + asks for none") {
    ShadowStackFrame frame;
    reset();

    Rooted<Value> answer{rtMakeString("T")};
    g_toStringResult = answer.get();
    g_valueOfResult = Value::fromDouble(7.0);

    Rooted<Value> obj{probeObject()};
    Value asString(bronze_to_string(obj.get().rawBits()));
    CHECK(joined() == "toString");
    CHECK(textOf(asString) == "T");

    g_calls.clear();
    Rooted<Value> empty{rtMakeString("")};
    Value added(bronze_dynamic_add(empty.get().rawBits(), obj.get().rawBits()));
    CHECK(joined() == "valueOf");
    CHECK(textOf(added) == "7");

    reset();
}

// 7.1.4 ToNumber step 1 and 7.1.19 ToPropertyKey step 1 are the same
// ToPrimitive call with different hints, and the hint is the thing an
// answer-only test cannot see: an object defining both halves produces a
// plausible number either way, and only the CALL LOG says which method made it.
TEST_CASE("ToNumber asks for hint number and ToPropertyKey for hint string") {
    ShadowStackFrame frame;
    reset();

    // `valueOf` answers a string so that both conversions have something usable
    // to work from, and `toString` answers a different one — so the two hints
    // cannot be told apart by luck.
    Rooted<Value> seven{rtMakeString("7")};
    Rooted<Value> eight{rtMakeString("8")};
    g_valueOfResult = seven.get();
    g_toStringResult = eight.get();

    SUBCASE("ToNumber runs valueOf first") {
        Rooted<Value> obj{probeObject()};
        CHECK(rtToNumber(obj.get()) == 7.0);
        CHECK(joined() == "valueOf");
        CHECK_FALSE(rtExceptionPending());
    }

    SUBCASE("ToPropertyKey runs toString first") {
        Rooted<Value> obj{probeObject()};
        CHECK(textOf(rtToPropertyKey(obj)) == "8");
        CHECK(joined() == "toString");
    }

    // A SYMBOL is already a property key, so 7.1.19 hands it back rather than
    // running ToString on it — which would be the TypeError that makes
    // `o[sym]` throw instead of reading.
    SUBCASE("ToPropertyKey keeps a symbol as a symbol") {
        Rooted<Value> desc{rtMakeString("s")};
        Rooted<Value> sym{rtMakeSymbol(desc.get())};
        g_toStringResult = sym.get();
        Rooted<Value> obj{probeObject()};
        Value key = rtToPropertyKey(obj);
        CHECK(key.isSymbol());
        CHECK(key.rawBits() == sym.get().rawBits());
        CHECK_FALSE(rtExceptionPending());
    }

    // A primitive costs no conversion at all, which is the half of ToNumber's
    // contract the builtins that hold a raw element pointer across it depend on.
    SUBCASE("a primitive is converted without any call") {
        CHECK(rtToNumber(rtMakeString("2.5")) == 2.5);
        CHECK(rtToNumber(Value::fromBool(true)) == 1.0);
        CHECK(joined().empty());
    }

    reset();
}

// 6.1.5.1: ToNumber of a Symbol is a TypeError, THROWN rather than fatal, so a
// program can catch it. The value handed back is NaN and is never read — the
// caller stores it and then tests the pending cell.
TEST_CASE("ToNumber of a symbol leaves a catchable TypeError pending") {
    ShadowStackFrame frame;
    reset();

    Rooted<Value> desc{rtMakeString("id")};
    Rooted<Value> sym{rtMakeSymbol(desc.get())};
    const double n = rtToNumber(sym.get());
    CHECK(n != n);  // NaN
    REQUIRE(rtExceptionPending());
    rtClearException();
    CHECK_FALSE(rtExceptionPending());

    reset();
}

// 7.2.14 steps 11-12: an object against a primitive is ToPrimitive'd with NO
// hint and the comparison restarts. Hint default is `valueOf` first, which is
// what an answer-only check would miss — `String(o)` of the same object asks the
// other way round.
TEST_CASE("loose equality converts an object with hint default and restarts") {
    ShadowStackFrame frame;
    reset();

    g_valueOfResult = Value::fromDouble(1.0);
    Rooted<Value> obj{probeObject()};
    Rooted<Value> one{Value::fromDouble(1.0)};
    CHECK(bronze_loose_eq(obj.get().rawBits(), one.get().rawBits()));
    CHECK(joined() == "valueOf");

    // The restart is a real second pass: a BOOLEAN is ToNumber'd first (step 9)
    // and only then is the object converted, so `o == true` reaches `1 == 1`.
    g_calls.clear();
    Rooted<Value> yes{Value::fromBool(true)};
    CHECK(bronze_loose_eq(obj.get().rawBits(), yes.get().rawBits()));
    CHECK(joined() == "valueOf");

    // Two objects answer by IDENTITY at step 1, before any conversion — so
    // neither method runs at all.
    g_calls.clear();
    Rooted<Value> other{probeObject()};
    CHECK(bronze_loose_eq(obj.get().rawBits(), obj.get().rawBits()));
    CHECK_FALSE(bronze_loose_eq(obj.get().rawBits(), other.get().rawBits()));
    CHECK(joined().empty());

    reset();
}
