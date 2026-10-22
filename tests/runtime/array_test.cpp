#include <doctest/doctest.h>

#include <stdexcept>

#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap.h"

using namespace bronze;

TEST_CASE("ArrayHeader element access and bounds") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ArrayHeader*> arr(ArrayHeader::create(heap, 4));
    REQUIRE(arr.get() != nullptr);
    CHECK(arr.get()->header.flags == HeapKind::Array);
    CHECK(arr.get()->length == 0);
    CHECK(arr.get()->capacity == 4);

    Rooted<Value> v0(Value::fromDouble(10.0));
    Rooted<Value> v1(Value::fromDouble(20.0));

    arr.get()->setElem(heap, 0, v0);
    CHECK(arr.get()->length == 1);

    arr.get()->setElem(heap, 1, v1);
    CHECK(arr.get()->length == 2);

    CHECK(arr.get()->getElem(0).asNumber() == 10.0);
    CHECK(arr.get()->getElem(1).asNumber() == 20.0);
    CHECK(arr.get()->getElem(2).isUndefined());

    // Sparse write grows the array and fills intermediate slots with holes
    Rooted<Value> v_sparse(Value::fromDouble(99.0));
    arr.get()->setElem(heap, 10, v_sparse);
    CHECK(arr.get()->length == 11);
    CHECK(arr.get()->getElem(10).asNumber() == 99.0);
    CHECK(arr.get()->hasElem(10));
    CHECK(arr.get()->getElem(5).isUndefined());
    CHECK(!arr.get()->hasElem(5));
}

#include "abi/bronze_abi.h"
#include "runtime/builtin_array_internal.h"
#include "runtime/builtin_constructors_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/iterator.h"
#include "runtime/microtask.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze::runtime;

inline uint64_t customJoinNumber(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(42.0).rawBits();
}

inline uint64_t customJoinBool(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(true).rawBits();
}

TEST_CASE("Array.prototype.join safely converts non-string join result without type confusion") {
    ShadowStackFrame frame;

    // Create an inner array [1]
    Rooted<Value> inner{Value(bronze_create_array(1))};
    Rooted<Value> one{Value::fromDouble(1.0)};
    inner.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, one);

    // Provide a custom join method returning a number
    Rooted<Value> joinKey{rtMakeString("join")};
    Rooted<Value> fnNum{rtNativeFunction(customJoinNumber, 0, "join", 0)};
    bronze_elem_set(inner.get().rawBits(), joinKey.get().rawBits(), fnNum.get().rawBits(), false);

    // Create outer array [inner, "end"]
    Rooted<Value> outer{Value(bronze_create_array(2))};
    outer.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, inner);
    Rooted<Value> endStr{rtMakeString("end")};
    outer.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, endStr);

    uint64_t resBits = arrayJoin(0, outer.get().rawBits(), 0, nullptr);
    Rooted<Value> res{Value(resBits)};
    REQUIRE(res.get().isString());
    CHECK(rtUtf8Chars(res.get().asString<StringHeader>()) == "42,end");

    // Provide a custom join method returning a boolean
    Rooted<Value> fnBool{rtNativeFunction(customJoinBool, 0, "join", 0)};
    bronze_elem_set(inner.get().rawBits(), joinKey.get().rawBits(), fnBool.get().rawBits(), false);

    resBits = arrayJoin(0, outer.get().rawBits(), 0, nullptr);
    res = Value(resBits);
    REQUIRE(res.get().isString());
    CHECK(rtUtf8Chars(res.get().asString<StringHeader>()) == "true,end");
}

TEST_CASE("ArrayHeader large index assignment throws RangeError instead of bad_alloc") {
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ArrayHeader*> arr(ArrayHeader::create(heap, 4));
    Rooted<Value> val(Value::fromDouble(1.0));

    // Large index >= 1000000000u
    Value exVal;
    CHECK(rtTryCatch([&] { arr.get()->setElem(heap, 1000000000u, val); }, exVal));
    CHECK(exVal.isObject());
    std::string errText;
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    CHECK(errText.find("Array allocation failed") != std::string::npos);

    // Large index exceeding heap capacity
    CHECK(rtTryCatch([&] { arr.get()->setElem(heap, 100000000u, val); }, exVal));
    CHECK(exVal.isObject());
    errText.clear();
    CHECK(rtErrorText(exVal, errText));
    CHECK(errText.find("RangeError") != std::string::npos);
    CHECK(errText.find("Array allocation failed") != std::string::npos);
}

namespace {

uint64_t doubleMapper(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    double v = argc > 0 ? Value(argv[0]).asNumber() : 0.0;
    return Value::fromDouble(v * 2.0).rawBits();
}

uint64_t customCtorBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> tagKey{rtMakeString("customCtorArg")};
    Rooted<Value> argVal{argc > 0 ? Value(argv[0]) : Value::fromDouble(-1.0)};
    bronze_elem_set(self.get().rawBits(), tagKey.get().rawBits(), argVal.get().rawBits(), true);
    return self.get().rawBits();
}

int g_iterGetterCalls = 0;

uint64_t singleStepIterNext(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> state{Value(envBits)};
    Rooted<Value> idxKey{rtMakeString("idx")};
    double idx = Value(bronze_elem_get(state.get().rawBits(), idxKey.get().rawBits())).asNumber();
    if (idx < 2.0) {
        Rooted<Value> nextIdx{Value::fromDouble(idx + 1.0)};
        bronze_elem_set(state.get().rawBits(), idxKey.get().rawBits(), nextIdx.get().rawBits(),
                        true);
        Rooted<Value> val{Value::fromDouble((idx + 1.0) * 10.0)};
        return rtCreateIterResult(val, false).rawBits();
    }
    Rooted<Value> undef{Value::fromUndefined()};
    return rtCreateIterResult(undef, true).rawBits();
}

uint64_t makeSingleStepIterator(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> state{Value(bronze_create_object())};
    Rooted<Value> idxKey{rtMakeString("idx")};
    Rooted<Value> zero{Value::fromDouble(0.0)};
    bronze_elem_set(state.get().rawBits(), idxKey.get().rawBits(), zero.get().rawBits(), true);
    Rooted<Value> iter{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> nextFn{rtMakeNativeClosure(singleStepIterNext, state, 0)};
    bronze_elem_set(iter.get().rawBits(), nextKey.get().rawBits(), nextFn.get().rawBits(), true);
    return iter.get().rawBits();
}

uint64_t countingIterGetter(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    ++g_iterGetterCalls;
    return rtNativeFunction(makeSingleStepIterator, 0, "iter", 0).rawBits();
}

uint64_t brokenAsyncIterMethod(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> iter{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> badNext{Value::fromDouble(123.0)};
    bronze_elem_set(iter.get().rawBits(), nextKey.get().rawBits(), badNext.get().rawBits(), true);
    return iter.get().rawBits();
}

uint64_t missingNextAsyncIterMethod(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return bronze_create_object();
}

uint64_t proxyAsyncIterMethod(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> state{Value(bronze_create_object())};
    Rooted<Value> idxKey{rtMakeString("idx")};
    Rooted<Value> zero{Value::fromDouble(0.0)};
    bronze_elem_set(state.get().rawBits(), idxKey.get().rawBits(), zero.get().rawBits(), true);
    Rooted<Value> nextFn{rtMakeNativeClosure(singleStepIterNext, state, 0)};
    Rooted<Value> emptyHandler{Value(bronze_create_object())};
    Rooted<Value> proxiedNext{
        Value::fromObject(ProxyHeader::create(rtHeap(), nextFn, emptyHandler))};
    Rooted<Value> iter{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    bronze_elem_set(iter.get().rawBits(), nextKey.get().rawBits(), proxiedNext.get().rawBits(),
                    true);
    return iter.get().rawBits();
}

}  // namespace

TEST_CASE("Array.of, Array.from, and Array.fromAsync constructor/callable dispatch and GetMethod") {
    ShadowStackFrame frame;

    Rooted<Value> arrayCtor{rtArrayConstructorObject()};
    Rooted<Value> ofKey{rtMakeString("of")};
    Rooted<Value> fromKey{rtMakeString("from")};
    Rooted<Value> fromAsyncKey{rtMakeString("fromAsync")};
    Rooted<Value> arrayOfFn{Value(bronze_elem_get(arrayCtor.get().rawBits(), ofKey.get().rawBits()))};
    Rooted<Value> arrayFromFn{
        Value(bronze_elem_get(arrayCtor.get().rawBits(), fromKey.get().rawBits()))};

    // 1. Non-constructor `this` (Math.max and arrow-like non-constructor) falls back to ArrayCreate
    Rooted<Value> mathObj{rtMathObject()};
    Rooted<Value> maxKey{rtMakeString("max")};
    Rooted<Value> mathMax{Value(bronze_elem_get(mathObj.get().rawBits(), maxKey.get().rawBits()))};
    CHECK(!rtIsConstructorValue(mathMax.get()));

    Value ofArgs[2] = {Value::fromDouble(10.0), Value::fromDouble(20.0)};
    Rooted<Value> ofRes{Value(bronze_dynamic_call(
        arrayOfFn.get().rawBits(), mathMax.get().rawBits(), 2,
        reinterpret_cast<const uint64_t*>(ofArgs)))};
    REQUIRE(ofRes.get().isObject());
    CHECK(ofRes.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array);
    CHECK(ofRes.get().asObject<ArrayHeader>()->length == 2);
    CHECK(ofRes.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 10.0);
    CHECK(ofRes.get().asObject<ArrayHeader>()->getElem(1).asNumber() == 20.0);

    // Array.from.call(Math.max, [10, 20], new Proxy(doubleMapper, {}))
    Rooted<Value> rawMapper{rtNativeFunction(doubleMapper, 1, "doubleMapper", 1)};
    Rooted<Value> emptyHandler{Value(bronze_create_object())};
    Rooted<Value> proxyMapper{
        Value::fromObject(ProxyHeader::create(rtHeap(), rawMapper, emptyHandler))};
    Value fromArgs[2] = {ofRes.get(), proxyMapper.get()};
    Rooted<Value> fromRes{Value(bronze_dynamic_call(
        arrayFromFn.get().rawBits(), mathMax.get().rawBits(), 2,
        reinterpret_cast<const uint64_t*>(fromArgs)))};
    REQUIRE(fromRes.get().isObject());
    CHECK(fromRes.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array);
    CHECK(fromRes.get().asObject<ArrayHeader>()->length == 2);
    CHECK(fromRes.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 20.0);
    CHECK(fromRes.get().asObject<ArrayHeader>()->getElem(1).asNumber() == 40.0);

    // 2. Constructible Proxy receiver constructs through the Proxy
    Rooted<Value> customCtor{rtNativeFunction(customCtorBody, 1, "CustomCtor", 1)};
    Rooted<Value> proxyCtor{
        Value::fromObject(ProxyHeader::create(rtHeap(), customCtor, emptyHandler))};
    CHECK(rtIsConstructorValue(proxyCtor.get()));

    Rooted<Value> proxyOfRes{Value(bronze_dynamic_call(
        arrayOfFn.get().rawBits(), proxyCtor.get().rawBits(), 2,
        reinterpret_cast<const uint64_t*>(ofArgs)))};
    REQUIRE(proxyOfRes.get().isObject());
    Rooted<Value> tagKey{rtMakeString("customCtorArg")};
    CHECK(Value(bronze_elem_get(proxyOfRes.get().rawBits(), tagKey.get().rawBits())).asNumber() ==
          2.0);
    Rooted<Value> lenKey{rtMakeString("length")};
    CHECK(Value(bronze_elem_get(proxyOfRes.get().rawBits(), lenKey.get().rawBits())).asNumber() ==
          2.0);

    // 3. Array.from evaluates [Symbol.iterator] getter only once and works on Functions
    Rooted<Value> iterKey{rtIteratorKey()};
    Rooted<Value> getterFn{rtNativeFunction(countingIterGetter, 0, "get [Symbol.iterator]", 0)};
    Rooted<Value> noSetter{Value::fromUndefined()};
    Rooted<Value> objWithGetter{Value(bronze_create_object())};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), objWithGetter, iterKey, getterFn, noSetter,
                                 true);
    g_iterGetterCalls = 0;
    Value fromGetterArgs[1] = {objWithGetter.get()};
    Rooted<Value> fromGetterRes{Value(bronze_dynamic_call(
        arrayFromFn.get().rawBits(), arrayCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromGetterArgs)))};
    CHECK(g_iterGetterCalls == 1);
    REQUIRE(fromGetterRes.get().isObject());
    CHECK(fromGetterRes.get().asObject<ArrayHeader>()->length == 2);
    CHECK(fromGetterRes.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 10.0);
    CHECK(fromGetterRes.get().asObject<ArrayHeader>()->getElem(1).asNumber() == 20.0);

    // Function object with [Symbol.iterator]
    Rooted<Value> fnIterable{rtNativeFunction(doubleMapper, 0)};
    Rooted<Value> iterMethodFn{rtNativeFunction(makeSingleStepIterator, 0, "iter", 0)};
    bronze_elem_set(fnIterable.get().rawBits(), iterKey.get().rawBits(),
                    iterMethodFn.get().rawBits(), true);
    Value fromFnArgs[1] = {fnIterable.get()};
    Rooted<Value> fromFnRes{Value(bronze_dynamic_call(
        arrayFromFn.get().rawBits(), arrayCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromFnArgs)))};
    REQUIRE(fromFnRes.get().isObject());
    CHECK(fromFnRes.get().asObject<ArrayHeader>()->length == 2);

    // Invalid non-nullish [Symbol.iterator] throws TypeError in Array.from
    Rooted<Value> badIterObj{Value(bronze_create_object())};
    Rooted<Value> badMethodVal{Value::fromDouble(42.0)};
    bronze_elem_set(badIterObj.get().rawBits(), iterKey.get().rawBits(), badMethodVal.get().rawBits(),
                    true);
    Value badIterArgs[1] = {badIterObj.get()};
    Value thrown;
    CHECK(rtTryCatch(
        [&] {
            bronze_dynamic_call(arrayFromFn.get().rawBits(), arrayCtor.get().rawBits(), 1,
                                reinterpret_cast<const uint64_t*>(badIterArgs));
        },
        thrown));

    // 4. Array.fromAsync tests: non-constructor `this`, callable Proxy mapFn,
    //    non-callable @@asyncIterator, missing/non-callable next, and callable Proxy async iterator
    uint64_t faArgs1[2] = {ofRes.get().rawBits(), proxyMapper.get().rawBits()};
    Rooted<Value> p1{Value(rtArrayFromAsyncBuiltin(0, mathMax.get().rawBits(), 2, faArgs1))};
    rtDrainMicrotasks();
    REQUIRE(rtIsPromise(p1.get()));
    CHECK(rtPromiseStateOf(p1.get()) == PromiseState::Fulfilled);
    Rooted<Value> p1Res{rtPromiseResultOf(p1.get())};
    REQUIRE(p1Res.get().isObject());
    CHECK(p1Res.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array);
    CHECK(p1Res.get().asObject<ArrayHeader>()->length == 2);
    CHECK(p1Res.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 20.0);
    CHECK(p1Res.get().asObject<ArrayHeader>()->getElem(1).asNumber() == 40.0);

    // Non-callable [Symbol.asyncIterator] rejects with TypeError
    Rooted<Value> asyncIterKey{rtAsyncIteratorKey()};
    Rooted<Value> badAsyncObj{Value(bronze_create_object())};
    bronze_elem_set(badAsyncObj.get().rawBits(), asyncIterKey.get().rawBits(),
                    badMethodVal.get().rawBits(), true);
    uint64_t faBadAsyncArgs[1] = {badAsyncObj.get().rawBits()};
    Rooted<Value> pBadAsync{
        Value(rtArrayFromAsyncBuiltin(0, arrayCtor.get().rawBits(), 1, faBadAsyncArgs))};
    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(pBadAsync.get()) == PromiseState::Rejected);

    // Async iterator with non-callable next (123) rejects instead of crashing
    Rooted<Value> badNextObj{Value(bronze_create_object())};
    Rooted<Value> badNextMethod{rtNativeFunction(brokenAsyncIterMethod, 0, "asyncIter", 0)};
    bronze_elem_set(badNextObj.get().rawBits(), asyncIterKey.get().rawBits(),
                    badNextMethod.get().rawBits(), true);
    uint64_t faBadNextArgs[1] = {badNextObj.get().rawBits()};
    Rooted<Value> pBadNext{
        Value(rtArrayFromAsyncBuiltin(0, arrayCtor.get().rawBits(), 1, faBadNextArgs))};
    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(pBadNext.get()) == PromiseState::Rejected);

    // Async iterator with missing next (undefined) rejects instead of crashing
    Rooted<Value> noNextObj{Value(bronze_create_object())};
    Rooted<Value> noNextMethod{rtNativeFunction(missingNextAsyncIterMethod, 0, "asyncIter", 0)};
    bronze_elem_set(noNextObj.get().rawBits(), asyncIterKey.get().rawBits(),
                    noNextMethod.get().rawBits(), true);
    uint64_t faNoNextArgs[1] = {noNextObj.get().rawBits()};
    Rooted<Value> pNoNext{
        Value(rtArrayFromAsyncBuiltin(0, arrayCtor.get().rawBits(), 1, faNoNextArgs))};
    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(pNoNext.get()) == PromiseState::Rejected);

    // Async iterator where both [Symbol.asyncIterator] and iter.next are callable Proxies
    Rooted<Value> rawAsyncIterFn{rtNativeFunction(proxyAsyncIterMethod, 0, "asyncIter", 0)};
    Rooted<Value> proxiedAsyncIterFn{
        Value::fromObject(ProxyHeader::create(rtHeap(), rawAsyncIterFn, emptyHandler))};
    Rooted<Value> proxyAsyncSrc{Value(bronze_create_object())};
    bronze_elem_set(proxyAsyncSrc.get().rawBits(), asyncIterKey.get().rawBits(),
                    proxiedAsyncIterFn.get().rawBits(), true);
    uint64_t faProxyAsyncArgs[1] = {proxyAsyncSrc.get().rawBits()};
    Rooted<Value> pProxyAsync{
        Value(rtArrayFromAsyncBuiltin(0, arrayCtor.get().rawBits(), 1, faProxyAsyncArgs))};
    rtDrainMicrotasks();
    CHECK(rtPromiseStateOf(pProxyAsync.get()) == PromiseState::Fulfilled);
    Rooted<Value> pProxyAsyncRes{rtPromiseResultOf(pProxyAsync.get())};
    REQUIRE(pProxyAsyncRes.get().isObject());
    CHECK(pProxyAsyncRes.get().asObject<ArrayHeader>()->length == 2);
    CHECK(pProxyAsyncRes.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 10.0);
    CHECK(pProxyAsyncRes.get().asObject<ArrayHeader>()->getElem(1).asNumber() == 20.0);
}


