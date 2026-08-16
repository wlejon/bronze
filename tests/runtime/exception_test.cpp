// The pending-exception cell and the `Error` family, below the compiler.
//
// The oracle cases pin only what ECMA-262 fixes — which constructor, which
// order, which value — because an oracle expectation is supposed to be
// derivable from the standard and an error's message text is not. The text is
// bronze's own choice, so the raise helpers are exercised here, where a
// message can be pinned as the decision it is.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// Every test leaves the cell as it found it: it is process-global state, and
// a test that raised without clearing would make the next one's rtThrow trip
// the "second exception while one is pending" tripwire.
struct ClearCell {
    ~ClearCell() { bronze_exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS; }
};

Value takePending() {
    const Value v(bronze_exception_cell);
    bronze_exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    return v;
}

std::string textOf(Value v) {
    std::string out;
    CHECK(rtErrorText(v, out));
    return out;
}

}  // namespace

TEST_CASE("the empty cell is the Hole singleton, and generated code agrees") {
    // Generated code compares the cell against this constant inline, so the two
    // spellings of "nothing pending" have to be one value. A Hole is never
    // user-visible, which is what makes it usable as the sentinel: no program
    // can throw one.
    CHECK(Value::fromHole().rawBits() == BRONZE_ABI_NO_EXCEPTION_BITS);
    CHECK(bronze_exception_cell == BRONZE_ABI_NO_EXCEPTION_BITS);
    CHECK_FALSE(rtExceptionPending());
}

TEST_CASE("a raise sets the cell and returns undefined") {
    ShadowStackFrame frame;
    ClearCell guard;

    // Every raise helper returns `undefined` rather than anything else, because
    // the caller stores the result into a GC root slot before it tests the
    // cell.
    const Value returned = rtThrowTypeError("boom");
    CHECK(returned.isUndefined());
    CHECK(rtExceptionPending());

    const Value thrown = takePending();
    CHECK_FALSE(rtExceptionPending());
    CHECK(rtIsErrorInstance(thrown));
    CHECK(textOf(thrown) == "TypeError: boom");
}

TEST_CASE("the error classes are distinct objects with a shared root") {
    ShadowStackFrame frame;

    Rooted<Value> error{rtErrorConstructor("Error")};
    Rooted<Value> typeError{rtErrorConstructor("TypeError")};
    Rooted<Value> rangeError{rtErrorConstructor("RangeError")};
    Rooted<Value> referenceError{rtErrorConstructor("ReferenceError")};
    REQUIRE(error.get().isObject());
    REQUIRE(typeError.get().isObject());
    REQUIRE(rangeError.get().isObject());
    REQUIRE(referenceError.get().isObject());

    // They were once ONE object: native function objects are interned by code
    // pointer, and all three constructors shared a body, so the last class
    // built won every `.prototype` and `new Error("x").name` answered
    // "RangeError". Nothing above the runtime could see it.
    CHECK(error.get().rawBits() != typeError.get().rawBits());
    CHECK(error.get().rawBits() != rangeError.get().rawBits());
    CHECK(typeError.get().rawBits() != rangeError.get().rawBits());

    // `ReferenceError` was NOT a class here until bronze had something to raise
    // one for: an unresolvable name, evaluated. It needs its own code pointer
    // for the same reason the others do.
    CHECK(referenceError.get().rawBits() != error.get().rawBits());
    CHECK(referenceError.get().rawBits() != typeError.get().rawBits());
    CHECK(referenceError.get().rawBits() != rangeError.get().rawBits());

    CHECK(rtErrorConstructor("Math").isUndefined());
}

TEST_CASE("an error's name comes from its own prototype and its message from itself") {
    ShadowStackFrame frame;
    ClearCell guard;

    rtThrowError(ErrorKind::Error, "plain");
    CHECK(textOf(takePending()) == "Error: plain");

    rtThrowRangeError("out of range");
    CHECK(textOf(takePending()) == "RangeError: out of range");

    // An empty message drops the separator, which is 20.5.3.4's rule and the
    // reason `console.log(new Error())` prints just `Error`.
    rtThrowError(ErrorKind::TypeError, "");
    CHECK(textOf(takePending()) == "TypeError");
}

TEST_CASE("only an Error instance renders as an error") {
    ShadowStackFrame frame;

    Rooted<Value> str{rtMakeString("negative")};
    CHECK_FALSE(rtIsErrorInstance(str.get()));
    CHECK_FALSE(rtIsErrorInstance(Value::fromDouble(7)));
    CHECK_FALSE(rtIsErrorInstance(Value::fromNull()));

    std::string unused;
    CHECK_FALSE(rtErrorText(str.get(), unused));
    CHECK_FALSE(rtErrorText(Value::fromDouble(7), unused));
    CHECK(unused.empty());

    // A plain object is not an error however error-shaped it looks: the test
    // is the prototype chain, not the presence of a `message`.
    Rooted<Value> obj{Value::fromObject(ObjectHeader::create(rtHeap(), rtArena(),
                                                            rtPlainObjectShape()))};
    Rooted<Value> key{rtMakeString("message")};
    Rooted<Value> val{rtMakeString("not really")};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    CHECK_FALSE(rtIsErrorInstance(obj.get()));
}

TEST_CASE("an uncaught value is reported as itself, not coerced") {
    ShadowStackFrame frame;
    ClearCell guard;

    rtThrowTypeError("bad receiver");
    CHECK(rtUncaughtText(takePending()) == "Uncaught TypeError: bad receiver");

    // `throw "negative"` and `throw 7` are different programs, so the report
    // uses console.log's rendering — which quotes a string — rather than
    // ToString.
    Rooted<Value> str{rtMakeString("negative")};
    CHECK(rtUncaughtText(str.get()) == "Uncaught 'negative'");
    CHECK(rtUncaughtText(Value::fromDouble(7)) == "Uncaught 7");
    CHECK(rtUncaughtText(Value::fromNull()) == "Uncaught null");
}

TEST_CASE("an error message survives a collection") {
    // The classes and the pending value are rooted through a root SOURCE
    // registered on first use. Registering it from a static initializer put
    // it into a heap that had not been constructed yet, and the heap's own
    // constructor then dropped it — which under BRONZE_GC_STRESS=1 collected
    // the error prototypes out from under the classes.
    ShadowStackFrame frame;
    ClearCell guard;

    rtThrowTypeError("survives");
    Rooted<Value> thrown{takePending()};
    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }
    CHECK(rtIsErrorInstance(thrown.get()));
    CHECK(textOf(thrown.get()) == "TypeError: survives");
}
