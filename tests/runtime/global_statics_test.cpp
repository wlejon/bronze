// `abi/bronze_global_statics.h` claims a short list of provided globals keep
// their members in a real, shape-indexed statics box. The lowerer arms an
// inline cache on the strength of that claim; this file is where the claim is
// checked against the live runtime rather than against a comment.
//
// The two failure directions are not symmetric, and only one of them is a bug
// this file can see. A name on the list that stops being box-backed makes the
// lowerer emit an arm that can never hit — a silent cost in whatever inner
// loop reads it, exactly the regression the list was introduced to remove, and
// the case below fails. A box-backed global left OFF the list is only a missed
// optimisation, so no case here demands the list be complete.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_global_statics.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

TEST_CASE("every statics-box global really keeps its members in a box") {
    ShadowStackFrame frame;

    for (std::string_view name : abi::kStaticsBoxGlobals) {
        const std::string key(name);
        CAPTURE(key);

        Value resolved;
        REQUIRE(rtResolveBuiltinGlobal(key, resolved));
        Rooted<Value> global{resolved};

        // A function object, because the arm reaches the box by loading
        // `properties` out of a FunctionHeader and nothing else has one.
        REQUIRE(global.get().isObject());
        REQUIRE(global.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function);

        // Not one the intrinsic table decides. `installStaticsCacheEntry`
        // refuses these receivers outright, so an armed site would fall
        // through to the helper on every read for the life of the process.
        CHECK(rtIntrinsicConstructorName(global.get()) == nullptr);

        // And the box exists and is shape-indexed: a dictionary shape is
        // refused on the same terms a plain object's is.
        Value props = global.get().asObject<FunctionHeader>()->properties;
        REQUIRE(props.isObject());
        const Shape* shape = props.asObject<ObjectHeader>()->shape;
        REQUIRE(shape != nullptr);
        CHECK_FALSE(shape->isDictionary());
    }
}

TEST_CASE("the globals left off the list are the ones a table answers") {
    // Not an exhaustive complement — the list is an allowlist and does not owe
    // one. These four are the specific receivers whose arms cost measurable
    // time before the list existed, so each one is pinned as still excluded.
    CHECK_FALSE(abi::isStaticsBoxGlobal("Number"));
    CHECK_FALSE(abi::isStaticsBoxGlobal("Array"));
    CHECK_FALSE(abi::isStaticsBoxGlobal("String"));
    CHECK_FALSE(abi::isStaticsBoxGlobal("Float32Array"));
    // `Math` is not a function object at all (21.3), which is the other way a
    // receiver reaches the arm and cannot pass its flags test.
    CHECK_FALSE(abi::isStaticsBoxGlobal("Math"));
    CHECK(abi::isStaticsBoxGlobal("Object"));
}

TEST_CASE("a table-answered global's member is not an own property of a box") {
    // The mechanism behind the exclusion, stated once directly: `Number` has a
    // statics box AND `EPSILON` is in it, so nothing about the LOOKUP explains
    // the refusal — `rtIntrinsicConstructorName` does, and only that.
    ShadowStackFrame frame;

    Value resolved;
    REQUIRE(rtResolveBuiltinGlobal("Number", resolved));
    Rooted<Value> number{resolved};

    REQUIRE(number.get().isObject());
    CHECK(rtIntrinsicConstructorName(number.get()) != nullptr);
}
