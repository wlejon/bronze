// How a raise travels, and the `Error` family, below the compiler.
//
// The oracle cases pin only what ECMA-262 fixes — which constructor, which
// order, which value — because an oracle expectation is supposed to be
// derivable from the standard and an error's message text is not. The text is
// bronze's own choice, so the raise helpers are exercised here, where a
// message can be pinned as the decision it is.

#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/stack_trace.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// The value `body` threw; a failed CHECK when it returned instead.
template <typename Body>
Value thrownBy(Body&& body) {
    Value thrown = Value::fromUndefined();
    CHECK(rtTryCatch(std::forward<Body>(body), thrown));
    return thrown;
}

std::string textOf(Value v) {
    std::string out;
    CHECK(rtErrorText(v, out));
    return out;
}

}  // namespace

TEST_CASE("a raise is a C++ BrassException carrying the thrown value") {
    ShadowStackFrame frame;

    // The one exception type every raise is: what compiled code's landing pads
    // and the host boundary catch.
    bool caught = false;
    uint64_t bits = 0;
    try {
        rtThrowTypeError("boom");
    } catch (const brass::runtime::BrassException& e) {
        caught = true;
        bits = e.value().raw();
    }
    REQUIRE(caught);
    const Value thrown(bits);
    CHECK(rtIsErrorInstance(thrown));
    CHECK(textOf(thrown) == "TypeError: boom");

    // A body that returns catches nothing and leaves `thrown` alone.
    Value untouched = Value::fromDouble(3);
    CHECK_FALSE(rtTryCatch([] {}, untouched));
    CHECK(untouched.asNumber() == 3);
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

    // Native function objects are interned by code pointer, so constructors
    // that shared a body would be one object and the last class built would
    // win every `.prototype`: each class needs a code pointer of its own.
    CHECK(error.get().rawBits() != typeError.get().rawBits());
    CHECK(error.get().rawBits() != rangeError.get().rawBits());
    CHECK(typeError.get().rawBits() != rangeError.get().rawBits());
    CHECK(referenceError.get().rawBits() != error.get().rawBits());
    CHECK(referenceError.get().rawBits() != typeError.get().rawBits());
    CHECK(referenceError.get().rawBits() != rangeError.get().rawBits());

    CHECK(rtErrorConstructor("Math").isUndefined());
}

TEST_CASE("an error's name comes from its own prototype and its message from itself") {
    ShadowStackFrame frame;

    CHECK(textOf(thrownBy([] { rtThrowError(ErrorKind::Error, "plain"); })) == "Error: plain");
    CHECK(textOf(thrownBy([] { rtThrowRangeError("out of range"); })) == "RangeError: out of range");

    // An empty message drops the separator, which is 20.5.3.4's rule and the
    // reason `console.log(new Error())` prints just `Error`.
    CHECK(textOf(thrownBy([] { rtThrowError(ErrorKind::TypeError, ""); })) == "TypeError");
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

    CHECK(rtUncaughtText(thrownBy([] { rtThrowTypeError("bad receiver"); })) ==
          "Uncaught TypeError: bad receiver");

    // `throw "negative"` and `throw 7` are different programs, so the report
    // uses console.log's rendering — which quotes a string — rather than
    // ToString.
    Rooted<Value> str{rtMakeString("negative")};
    CHECK(rtUncaughtText(str.get()) == "Uncaught 'negative'");
    CHECK(rtUncaughtText(Value::fromDouble(7)) == "Uncaught 7");
    CHECK(rtUncaughtText(Value::fromNull()) == "Uncaught null");
}

TEST_CASE("an error message survives a collection") {
    // The error classes are rooted through a root SOURCE registered on first
    // use, not from a static initializer, which would register it into a heap
    // not yet constructed. Under BRONZE_GC_STRESS=1 that is what this pins.
    ShadowStackFrame frame;

    Rooted<Value> thrown{thrownBy([] { rtThrowTypeError("survives"); })};
    for (int i = 0; i < 32; ++i) {
        Rooted<Value> garbage{rtMakeString("junk")};
        (void)garbage;
        rtHeap().collect();
    }
    CHECK(rtIsErrorInstance(thrown.get()));
    CHECK(textOf(thrown.get()) == "TypeError: survives");
}

TEST_CASE("stack trace formatter produces error header when called without JS frames") {
    ShadowStackFrame frame;
    Rooted<Value> err{rtNewErrorValue(ErrorKind::Error, "test error")};
    std::string stack = bronze_format_stack_trace(err.get());
    CHECK(stack == "Error: test error");
}

TEST_CASE("code range registry registration and lookup") {
    uint8_t dummyCode[128];
    bronze_fn_desc desc{"testFn", "test.js", 1, 1, 0, 0, nullptr};
    bronze_code_range r{dummyCode, 128, 0, &desc, nullptr};
    bronze_register_code_ranges(&r, 1);
    CHECK(find_code_range(dummyCode) != nullptr);
    CHECK(find_code_range(dummyCode + 50) != nullptr);
    CHECK(find_code_range(dummyCode + 127) != nullptr);
    CHECK(find_code_range(dummyCode + 128) == nullptr);
    bronze_unregister_code_ranges(&r, 1);
    CHECK(find_code_range(dummyCode) == nullptr);
}

TEST_CASE("a pc entry's file is the position's own, falling back to the descriptor's") {
    // A program's merged top level is one compiled function holding lines of
    // several source files, so a code site reads its file off the pc entry.
    uint8_t dummyCode[64];
    bronze_fn_desc desc{"", "dep.js", 1, 1, BRONZE_FN_DESC_TOPLEVEL, 0, nullptr};
    const char* const files[] = {"main.js", "dep.js"};
    const bronze_pc_entry pcs[] = {
        {0, 3, 1, 1},                    // dep.js:3
        {16, 8, 1, 0},                   // main.js:8
        {32, 9, 5, BRONZE_PC_FILE_DESC}, // the descriptor's file
        {48, 10, 1, 7},                  // an index past file_count: the same
    };
    bronze_code_range r{dummyCode, 64, 4, &desc, pcs, files, 2, 0};
    bronze_register_code_ranges(&r, 1);
    CodeSite site;
    REQUIRE(find_code_site(dummyCode + 4, site));
    CHECK(std::string(site.file) == "dep.js");
    CHECK(site.line == 3);
    REQUIRE(find_code_site(dummyCode + 20, site));
    CHECK(std::string(site.file) == "main.js");
    CHECK(site.line == 8);
    REQUIRE(find_code_site(dummyCode + 40, site));
    CHECK(std::string(site.file) == "dep.js");
    CHECK(site.col == 5);
    REQUIRE(find_code_site(dummyCode + 50, site));
    CHECK(std::string(site.file) == "dep.js");
    bronze_unregister_code_ranges(&r, 1);
}

TEST_CASE("exotic receiver and invalid operations raise catchable TypeError rather than fatal abort") {
    ShadowStackFrame frame;

    auto isTypeError = [](Value thrown) {
        return rtIsErrorInstance(thrown) && textOf(thrown).find("TypeError") != std::string::npos;
    };

    // 1. bronze_object_rest on non-plain receiver
    CHECK(isTypeError(thrownBy([] {
        bronze_object_rest(Value::fromDouble(42.0).rawBits(), Value::fromUndefined().rawBits());
    })));

    // 2. bronze_elem_set on invalid receiver (null)
    CHECK(isTypeError(thrownBy([] {
        bronze_elem_set(Value::fromNull().rawBits(), Value::fromDouble(0.0).rawBits(),
                        Value::fromDouble(1.0).rawBits(), false);
    })));

    // 3. bronze_accessor_def on exotic receiver (Array)
    const uint32_t keyIndex = bronze_register_key_string("exoticProp");
    Rooted<Value> arr{Value(bronze_create_array(0))};
    CHECK(isTypeError(thrownBy([&] {
        bronze_accessor_def(arr.get().rawBits(), keyIndex, Value::fromUndefined().rawBits(),
                            Value::fromUndefined().rawBits(), false);
    })));

    // 4. bronze_method_def on exotic receiver (Array)
    CHECK(isTypeError(thrownBy([&] {
        bronze_method_def(arr.get().rawBits(), keyIndex, Value::fromUndefined().rawBits());
    })));

    // 5. ObjectHeader::getProp with invalid property key
    Rooted<Value> obj{Value(bronze_create_object())};
    Rooted<Value> invalidKey{Value::fromBool(true)};
    CHECK(isTypeError(thrownBy([&] { obj.get().asObject<ObjectHeader>()->getProp(rtHeap(), invalidKey); })));
}
