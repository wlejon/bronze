#include <doctest/doctest.h>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
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
    const uint32_t nameKey = bronze_register_key_string("adder");
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

TEST_CASE("Function static property caching serves classes, Number.EPSILON, mega_prop, and Function.prototype methods") {
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    NonMovingArena& arena = runtime::rtArena();

    // 1. Number.EPSILON caches in a site way and hits rtFunctionCachedMember,
    //    whereas Array keeps table statics uncached in statics boxes.
    Rooted<Value> numCtor{runtime::rtNumberConstructorObject()};
    CHECK_FALSE(runtime::rtIntrinsicConstructorHasTableStatics(numCtor.get()));
    CHECK(runtime::rtIntrinsicConstructorHasTableStatics(runtime::rtArrayConstructorObject()));

    const uint32_t epsKey = bronze_register_key_string("EPSILON");
    InlineCacheSite epsSite{};
    const double eps1 =
        Value(bronze_prop_get(numCtor.get().rawBits(), epsKey, reinterpret_cast<uint64_t*>(&epsSite)))
            .asNumber();
    CHECK(eps1 > 0.0);
    CHECK(epsSite.ways[0].isRealShape());
    uint64_t cachedEps = 0;
    REQUIRE(runtime::rtFunctionCachedMember(numCtor.get(), &epsSite, epsKey, cachedEps));
    CHECK(Value(cachedEps).asNumber() == eps1);

    // 2. ES6 class constructors (> 4 distinct static shapes) warm site + mega_prop.
    const uint32_t slerpKey = bronze_register_key_string("slerpFlat");
    InlineCacheSite classSite{};
    std::vector<Value> classes;
    Rooted<Value> classesArr{Value(bronze_create_array(6))};
    for (uint32_t i = 0; i < 6; ++i) {
        Rooted<Value> cls{Value::fromObject(FunctionHeader::create(
            heap, dummyAdd, Value::fromUndefined(), 0,
            BRONZE_ABI_FN_FLAG_PROTOTYPE | BRONZE_ABI_FN_FLAG_CONSTRUCT |
                BRONZE_ABI_FN_FLAG_CLASS_CTOR))};
        runtime::rtEnsureFunctionProperties(cls);
        Rooted<Value> box{cls.get().asObject<FunctionHeader>()->properties};
        const std::string extraName = "extra_" + std::to_string(i);
        Rooted<Value> extraKey{runtime::rtMakeString(extraName)};
        Rooted<Value> extraVal{Value::fromDouble(i)};
        box.get().asObject<ObjectHeader>()->setProp(heap, arena, extraKey, extraVal);
        Rooted<Value> k{Value::fromString(runtime::rtKeyHeader(slerpKey))};
        Rooted<Value> v{Value::fromDouble(100.0 + i)};
        box.get().asObject<ObjectHeader>()->setProp(heap, arena, k, v);
        classesArr.get().asObject<ArrayHeader>()->setElem(heap, i, cls);
    }
    for (int round = 0; round < 3; ++round) {
        for (uint32_t i = 0; i < 6; ++i) {
            Value cls = classesArr.get().asObject<ArrayHeader>()->getElem(i);
            Value got{
                bronze_prop_get(cls.rawBits(), slerpKey, reinterpret_cast<uint64_t*>(&classSite))};
            CHECK(got.asNumber() == 100.0 + i);
        }
    }

    // 3. Box-less native and source functions answer apply / call / length / name cleanly.
    const uint32_t applyKey = bronze_register_key_string("apply");
    const uint32_t callKey = bronze_register_key_string("call");
    const uint32_t lenKey = bronze_register_key_string("length");
    const uint32_t fnNameKey = bronze_register_key_string("name");

    // Ensure Function.prototype is initialized once, then read on a box-less function.
    (void)runtime::rtFunctionPrototypeObject();
    const uint32_t hostName = bronze_register_key_string("texImage2D");
    Rooted<Value> hostFn{
        Value::fromObject(FunctionHeader::create(heap, dummyAdd, Value::fromUndefined(), 2))};
    runtime::rtSetFunctionNameAndLength(hostFn.get().asObject<FunctionHeader>(), hostName, 6);
    CHECK_FALSE(hostFn.get().asObject<FunctionHeader>()->properties.isObject());

    Value applyFn{bronze_prop_get(hostFn.get().rawBits(), applyKey, nullptr)};
    Value callFn{bronze_prop_get(hostFn.get().rawBits(), callKey, nullptr)};
    CHECK(applyFn.isObject());
    CHECK(callFn.isObject());
    CHECK(Value(bronze_prop_get(hostFn.get().rawBits(), lenKey, nullptr)).asNumber() == 6.0);
    CHECK(runtime::rtUtf8Chars(
              Value(bronze_prop_get(hostFn.get().rawBits(), fnNameKey, nullptr))
                  .asString<StringHeader>()) == "texImage2D");

    // Invoke via `hostFn.call(undefined, 7, 8)` -> 15.
    uint64_t callArgs[3] = {Value::fromUndefined().rawBits(), Value::fromDouble(7.0).rawBits(),
                            Value::fromDouble(8.0).rawBits()};
    CHECK(Value(bronze_dynamic_call(callFn.rawBits(), hostFn.get().rawBits(), 3, callArgs))
              .asNumber() == 15.0);
}
