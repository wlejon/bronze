// The Map/Set table. Two things are pinned here that an oracle case cannot
// reach: SameValueZero on the edges the language cares about, and the hash
// index agreeing with a linear scan over hundreds of mixed-kind keys —
// including across collections, which is the whole reason the index carries an
// epoch. A wrong bucket answers "not found", which is a silent wrong answer, so
// it is proved against an independent oracle rather than against a handful of
// examples.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

using namespace bronze;

namespace {

// The answer the table must agree with: a scan of the entry vector in
// insertion order, which is the definition every method here is built on.
uint32_t linearFind(MapHeader* map, Value key) {
    for (uint32_t i = 0, used = map->used(); i < used; ++i) {
        if (!map->liveAt(i)) continue;
        if (sameValueZero(map->keyAt(i), key)) return i;
    }
    return UINT32_MAX;
}

}  // namespace

TEST_CASE("the Set operation table is one list, with seven distinct bodies") {
    // 24.2.4's seven set operations live in their own translation unit and are
    // reached through a table, from which `Set.prototype` is populated — so
    // `s.union` is an ordinary read off that object. What an oracle case
    // cannot see is the failure this table invites: seven near-identical rows,
    // where a copy-paste gives two names ONE body and every call still returns
    // a Set, just the wrong one. `rtNativeFunction` interns by code pointer, so
    // distinct bodies mean distinct function objects and a duplicated row is
    // visible as an identity collision on the prototype.
    ShadowStackFrame frame;
    size_t count = 0;
    const runtime::NativeMethod* ops = runtime::rtSetOperationMethods(count);
    REQUIRE(count == 7);

    Rooted<Value> setCtor{runtime::rtMapConstructor("Set")};
    REQUIRE(setCtor.get().isObject());
    Rooted<Value> proto{setCtor.get().asObject<FunctionHeader>()->prototype};
    REQUIRE(proto.get().isObject());

    auto member = [&](const char* name) {
        Rooted<Value> key{runtime::rtMakeString(name)};
        return proto.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key);
    };

    for (size_t i = 0; i < count; ++i) {
        REQUIRE(ops[i].name != nullptr);
        // The read off `Set.prototype` yields a callable.
        const Value fn = member(ops[i].name);
        REQUIRE(fn.isObject());
        CHECK(fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function);
        // Every one takes exactly one argument, and `length` is what a caller
        // reflects on.
        CHECK(ops[i].arity == 1);
    }

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            CHECK(std::string(ops[i].name) != std::string(ops[j].name));
            // The first is ROOTED before the second lookup runs: interning the
            // key allocates, and under GC stress that collection moves the
            // object the first read returned. Comparing a stale address
            // against a fresh one is the bug this file is otherwise about.
            Rooted<Value> first{member(ops[i].name)};
            const Value second = member(ops[j].name);
            CHECK(first.get().rawBits() != second.rawBits());
        }
    }

    // The seven the standard names, and no eighth.
    std::vector<std::string> names;
    for (size_t i = 0; i < count; ++i) names.push_back(ops[i].name);
    std::sort(names.begin(), names.end());
    const std::vector<std::string> expected = {"difference",   "intersection", "isDisjointFrom",
                                               "isSubsetOf",   "isSupersetOf", "symmetricDifference",
                                               "union"};
    CHECK(names == expected);
}

TEST_CASE("SameValueZero is === with NaN matching itself") {
    const Value nan = Value::fromDouble(std::nan(""));
    CHECK(sameValueZero(nan, nan));
    CHECK(sameValueZero(Value::fromDouble(0.0), Value::fromDouble(-0.0)));
    CHECK(sameValueZero(Value::fromDouble(-0.0), Value::fromDouble(0.0)));
    CHECK_FALSE(sameValueZero(Value::fromDouble(1.0), Value::fromDouble(2.0)));
    CHECK_FALSE(sameValueZero(Value::fromBool(false), Value::fromDouble(0.0)));
    CHECK_FALSE(sameValueZero(Value::fromNull(), Value::fromUndefined()));
}

// The three table tests below build on the RUNTIME's heap rather than a local
// one: `rtNewMap` allocates the object there (its shape and brand are the
// thread's intrinsics), and an entry table must live in the heap its owner
// does or the first collection of either would orphan the other.
TEST_CASE("two string objects with the same characters are one key") {
    Heap& heap = runtime::rtHeap();
    ShadowStackFrame frame;
    Rooted<Value> map{runtime::rtNewMap()};

    Rooted<Value> k1{Value::fromString(StringHeader::createFromUTF8(heap, "hello"))};
    Rooted<Value> k2{Value::fromString(StringHeader::createFromUTF8(heap, "hello"))};
    CHECK(k1.get().rawBits() != k2.get().rawBits());  // distinct objects

    Rooted<Value> v{Value::fromDouble(7.0)};
    MapHeader::set(heap, map, k1, v);
    CHECK(MapHeader::find(heap, map, k2) == 0);
    CHECK(map.get().asObject<MapHeader>()->liveSize() == 1);

    Rooted<Value> v2{Value::fromDouble(8.0)};
    MapHeader::set(heap, map, k2, v2);
    CHECK(map.get().asObject<MapHeader>()->liveSize() == 1);
    CHECK(map.get().asObject<MapHeader>()->valueAt(0).asNumber() == 8.0);
}

TEST_CASE("a re-added key moves to the end and an updated one does not") {
    Heap& heap = runtime::rtHeap();
    ShadowStackFrame frame;
    Rooted<Value> map{runtime::rtNewMap()};

    for (double d : {1.0, 2.0, 3.0}) {
        Rooted<Value> k{Value::fromDouble(d)};
        Rooted<Value> v{Value::fromDouble(d * 10)};
        MapHeader::set(heap, map, k, v);
    }
    Rooted<Value> two{Value::fromDouble(2.0)};
    Rooted<Value> updated{Value::fromDouble(99.0)};
    MapHeader::set(heap, map, two, updated);
    CHECK(map.get().asObject<MapHeader>()->keyAt(1).asNumber() == 2.0);
    CHECK(map.get().asObject<MapHeader>()->valueAt(1).asNumber() == 99.0);

    CHECK(MapHeader::remove(heap, map, two));
    CHECK_FALSE(MapHeader::remove(heap, map, two));
    Rooted<Value> back{Value::fromDouble(2.0)};
    Rooted<Value> v{Value::fromDouble(20.0)};
    MapHeader::set(heap, map, back, v);

    std::vector<double> order;
    auto* live = map.get().asObject<MapHeader>();
    for (uint32_t i = 0; i < live->used(); ++i) {
        if (live->liveAt(i)) order.push_back(live->keyAt(i).asNumber());
    }
    CHECK(order == std::vector<double>{1.0, 3.0, 2.0});
}

// The index hashes an OBJECT key by its address and the collector moves
// objects, so this is the case the epoch exists for: every lookup after a
// collection must still find every key.
TEST_CASE("the hash index agrees with a linear scan across collections") {
    Heap& heap = runtime::rtHeap();
    NonMovingArena& arena = runtime::rtArena();
    ShadowStackFrame frame;
    Rooted<Value> map{runtime::rtNewMap()};
    // The keys are held in an array of their own as well as by the map, so
    // that "the map lost it" and "the key moved" are different failures — one
    // root covers all of them and the collector forwards both copies.
    Rooted<Value> keys{Value::fromObject(ArrayHeader::create(heap, 8))};
    keys.get().asObject<ArrayHeader>()->header.flags = 1;

    constexpr uint32_t kCount = 300;
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k;
        switch (i % 4) {
            case 0: k.set(Value::fromDouble(static_cast<double>(i))); break;
            case 1:
                k.set(Value::fromString(
                    StringHeader::createFromUTF8(heap, "key-" + std::to_string(i))));
                break;
            case 2:
                k.set(Value::fromObject(
                    ObjectHeader::create(heap, arena, Shape::createRoot(arena))));
                break;
            // A handful of duplicates, so the table has to update rather than
            // insert and the live count has to stop growing.
            default: k.set(Value::fromDouble(static_cast<double>(i % 7))); break;
        }
        keys.get().asObject<ArrayHeader>()->setElem(heap, i, k);
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i) * 2.0)};
        MapHeader::set(heap, map, k, v);
    }

    auto checkAll = [&] {
        for (uint32_t i = 0; i < kCount; ++i) {
            Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
            const uint32_t viaIndex = MapHeader::find(heap, map, k);
            const uint32_t viaScan = linearFind(map.get().asObject<MapHeader>(), k.get());
            REQUIRE(viaIndex == viaScan);
            REQUIRE(viaIndex != UINT32_MAX);
        }
    };
    checkAll();
    heap.collect();
    checkAll();

    // Deleting every third key must leave the rest findable: the buckets of
    // the removed entries are left pointing at tombstones, and every probe
    // that used to run through them still has to reach its own entry.
    uint32_t removed = 0;
    for (uint32_t i = 0; i < kCount; i += 3) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        if (MapHeader::remove(heap, map, k)) ++removed;
    }
    CHECK(removed > 0);
    heap.collect();
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        CHECK(MapHeader::find(heap, map, k) ==
              linearFind(map.get().asObject<MapHeader>(), k.get()));
    }

    // Re-adding after the deletions forces a compaction, which rebuilds both
    // tables from scratch — the one path where a lost key would be silent.
    for (uint32_t i = 0; i < kCount; ++i) {
        Rooted<Value> k{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i))};
        MapHeader::set(heap, map, k, v);
    }
    checkAll();
}

// The counter the index hangs off is the one thing in this file that cannot be
// proved by comparing against a linear scan: the scan drives collections
// through `collect()` too, so a counter that only `collect()` maintains would
// look correct here and be wrong for any future collector with a second entry
// point. So the property is pinned directly — the epoch is a count of
// RELOCATIONS, moved by the copy itself.
TEST_CASE("the relocation epoch counts objects moved, not cycles finished") {
    Heap heap;
    NonMovingArena arena;
    ShadowStackFrame frame;

    CHECK(heap.relocation_epoch() == 0);

    // Allocation alone must not move it: an object that has never been copied
    // still lives where its hash says it does.
    Rooted<Value> kept{Value::fromObject(ObjectHeader::create(heap, arena,
                                                              Shape::createRoot(arena)))};
    CHECK(heap.relocation_epoch() == 0);

    // A collection with a live object moves that object, so the epoch moves by
    // at least one and the map's index is stale by construction.
    heap.collect();
    const uint64_t afterFirst = heap.relocation_epoch();
    CHECK(afterFirst >= 1);
    // Not an equality: under BRONZE_GC_STRESS every allocation collects, so
    // the number of CYCLES is not a fact this test may assert — which is the
    // point of the epoch not being one either.
    CHECK(heap.collection_count() >= 1);

    // And it counts moves rather than cycles: the first object is old now and
    // stays put, so a second collection moves only the new young one...
    Rooted<Value> second{Value::fromObject(ObjectHeader::create(heap, arena,
                                                               Shape::createRoot(arena)))};
    const Value keptBefore = kept.get();
    heap.collect();
    CHECK(kept.get() == keptBefore);
    const uint64_t afterSecond = heap.relocation_epoch();
    CHECK(afterSecond >= afterFirst + 1);

    // ...and a third, with every live object old, moves nothing.
    heap.collect();
    CHECK(heap.relocation_epoch() == afterSecond);

    // A collection with nothing live to move leaves it alone, which is what
    // makes it an honest "has anything moved?" and not a proxy for "has a
    // cycle run?".
    {
        Heap empty;
        empty.collect();
        CHECK(empty.relocation_epoch() == 0);
        CHECK(empty.collection_count() == 1);
    }
}

namespace {

using namespace bronze::runtime;

Value rtMakeNativeClosure(uint64_t (*fn)(uint64_t, uint64_t, uint32_t, const uint64_t*),
                          Rooted<Value>& env, uint32_t arity) {
    Rooted<Value> res{rtNativeFunction(fn, arity, "fn", arity)};
    res.get().asObject<FunctionHeader>()->env_record = env.get();
    return res.get();
}

uint64_t alwaysTrueFn(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(true).rawBits();
}

uint64_t mutatingGrowKeysNext(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> state{Value(envBits)};
    Rooted<Value> stepKey{rtMakeString("step")};
    double step = Value(bronze_elem_get(state.get().rawBits(), stepKey.get().rawBits())).asNumber();
    if (step == 0.0) {
        Rooted<Value> one{Value::fromDouble(1.0)};
        bronze_elem_set(state.get().rawBits(), stepKey.get().rawBits(), one.get().rawBits(), true);
        Rooted<Value> selfKey{rtMakeString("self")};
        Rooted<Value> selfSet{Value(bronze_elem_get(state.get().rawBits(), selfKey.get().rawBits()))};
        for (int i = 10; i < 30; ++i) {
            Rooted<Value> v{Value::fromDouble(static_cast<double>(i))};
            MapHeader::set(rtHeap(), selfSet, v, v);
        }
        Rooted<Value> matchVal{Value::fromDouble(25.0)};
        return rtCreateIterResult(matchVal, false).rawBits();
    }
    Rooted<Value> undef{Value::fromUndefined()};
    return rtCreateIterResult(undef, true).rawBits();
}

uint64_t mutatingGrowKeysFn(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> selfSet{Value(envBits)};
    Rooted<Value> state{Value(bronze_create_object())};
    Rooted<Value> stepKey{rtMakeString("step")};
    Rooted<Value> zero{Value::fromDouble(0.0)};
    bronze_elem_set(state.get().rawBits(), stepKey.get().rawBits(), zero.get().rawBits(), true);
    Rooted<Value> selfKey{rtMakeString("self")};
    bronze_elem_set(state.get().rawBits(), selfKey.get().rawBits(), selfSet.get().rawBits(), true);

    Rooted<Value> iter{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> nextFn{rtMakeNativeClosure(mutatingGrowKeysNext, state, 0)};
    bronze_elem_set(iter.get().rawBits(), nextKey.get().rawBits(), nextFn.get().rawBits(), true);
    return iter.get().rawBits();
}

uint64_t mutatingShrinkKeysNext(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> state{Value(envBits)};
    Rooted<Value> stepKey{rtMakeString("step")};
    double step = Value(bronze_elem_get(state.get().rawBits(), stepKey.get().rawBits())).asNumber();
    Rooted<Value> selfKey{rtMakeString("self")};
    Rooted<Value> selfSet{Value(bronze_elem_get(state.get().rawBits(), selfKey.get().rawBits()))};
    if (step == 0.0) {
        Rooted<Value> one{Value::fromDouble(1.0)};
        bronze_elem_set(state.get().rawBits(), stepKey.get().rawBits(), one.get().rawBits(), true);
        Rooted<Value> undef{Value::fromUndefined()};
        return rtCreateIterResult(undef, false).rawBits();
    }
    if (step == 1.0) {
        Rooted<Value> two{Value::fromDouble(2.0)};
        bronze_elem_set(state.get().rawBits(), stepKey.get().rawBits(), two.get().rawBits(), true);
        Rooted<Value> undef{Value::fromUndefined()};
        MapHeader::remove(rtHeap(), selfSet, undef);
        Rooted<Value> v1{Value::fromDouble(1.0)};
        return rtCreateIterResult(v1, false).rawBits();
    }
    Rooted<Value> undef{Value::fromUndefined()};
    return rtCreateIterResult(undef, true).rawBits();
}

uint64_t mutatingShrinkKeysFn(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> selfSet{Value(envBits)};
    Rooted<Value> state{Value(bronze_create_object())};
    Rooted<Value> stepKey{rtMakeString("step")};
    Rooted<Value> zero{Value::fromDouble(0.0)};
    bronze_elem_set(state.get().rawBits(), stepKey.get().rawBits(), zero.get().rawBits(), true);
    Rooted<Value> selfKey{rtMakeString("self")};
    bronze_elem_set(state.get().rawBits(), selfKey.get().rawBits(), selfSet.get().rawBits(), true);

    Rooted<Value> iter{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> nextFn{rtMakeNativeClosure(mutatingShrinkKeysNext, state, 0)};
    bronze_elem_set(iter.get().rawBits(), nextKey.get().rawBits(), nextFn.get().rawBits(), true);
    return iter.get().rawBits();
}

uint64_t reentrantReturnMapper(uint64_t envBits, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> helper{Value(envBits)};
    Rooted<Value> retKey{rtMakeString("return")};
    Rooted<Value> retFn{Value(bronze_elem_get(helper.get().rawBits(), retKey.get().rawBits()))};
    return bronze_dynamic_call(retFn.get().rawBits(), helper.get().rawBits(), 0, nullptr);
}

int g_outerReturnCalls = 0;

uint64_t countingOuterReturn(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    ++g_outerReturnCalls;
    Rooted<Value> undef{Value::fromUndefined()};
    return rtCreateIterResult(undef, true).rawBits();
}

uint64_t throwingInnerReturn(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return rtThrowTypeError("inner return error").rawBits();
}

uint64_t nonObjectReturn(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(123.0).rawBits();
}

uint64_t infiniteOneNext(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> one{Value::fromDouble(1.0)};
    return rtCreateIterResult(one, false).rawBits();
}

uint64_t flatMapInnerMapper(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    Rooted<Value> inner{Value(bronze_create_object())};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> nextFn{rtNativeFunction(infiniteOneNext, 0, "next", 0)};
    bronze_elem_set(inner.get().rawBits(), nextKey.get().rawBits(), nextFn.get().rawBits(), true);
    Rooted<Value> retKey{rtMakeString("return")};
    Rooted<Value> retFn{rtNativeFunction(throwingInnerReturn, 0, "return", 0)};
    bronze_elem_set(inner.get().rawBits(), retKey.get().rawBits(), retFn.get().rawBits(), true);
    return inner.get().rawBits();
}

uint64_t sumReducer(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    double a = argc > 0 ? Value(argv[0]).asNumber() : 0.0;
    double b = argc > 1 ? Value(argv[1]).asNumber() : 0.0;
    return Value::fromDouble(a + b).rawBits();
}

}  // namespace

TEST_CASE("Set.prototype.intersection mutation safety, Iterator helper return/close, and callable Proxy support") {
    ShadowStackFrame frame;

    Rooted<Value> setCtor{rtMapConstructor("Set")};
    Rooted<Value> setProto{setCtor.get().asObject<FunctionHeader>()->prototype};
    Rooted<Value> intersectKey{rtMakeString("intersection")};
    Rooted<Value> intersectFn{
        Value(bronze_elem_get(setProto.get().rawBits(), intersectKey.get().rawBits()))};

    // 1a. Set.prototype.intersection when other.keys() grows `self` beyond initial `thisSize`
    Rooted<Value> sGrow{rtNewSet()};
    for (int i = 1; i <= 3; ++i) {
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i))};
        MapHeader::set(rtHeap(), sGrow, v, v);
    }
    Rooted<Value> otherGrow{Value(bronze_create_object())};
    Rooted<Value> sizeKey{rtMakeString("size")};
    Rooted<Value> hasKey{rtMakeString("has")};
    Rooted<Value> keysKey{rtMakeString("keys")};
    Rooted<Value> oneVal{Value::fromDouble(1.0)};
    Rooted<Value> rawHasFn{rtNativeFunction(alwaysTrueFn, 1, "has", 1)};
    Rooted<Value> emptyHandler{Value(bronze_create_object())};
    Rooted<Value> proxyHasFn{
        Value::fromObject(ProxyHeader::create(rtHeap(), rawHasFn, emptyHandler))};
    Rooted<Value> rawGrowKeys{rtMakeNativeClosure(mutatingGrowKeysFn, sGrow, 0)};
    Rooted<Value> proxyGrowKeys{
        Value::fromObject(ProxyHeader::create(rtHeap(), rawGrowKeys, emptyHandler))};
    bronze_elem_set(otherGrow.get().rawBits(), sizeKey.get().rawBits(), oneVal.get().rawBits(), true);
    bronze_elem_set(otherGrow.get().rawBits(), hasKey.get().rawBits(), proxyHasFn.get().rawBits(), true);
    bronze_elem_set(otherGrow.get().rawBits(), keysKey.get().rawBits(), proxyGrowKeys.get().rawBits(),
                    true);

    Value growArgs[1] = {otherGrow.get()};
    Rooted<Value> growRes{Value(bronze_dynamic_call(
        intersectFn.get().rawBits(), sGrow.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(growArgs)))};
    REQUIRE(rtIsSetKind(growRes.get()));
    CHECK(growRes.get().asObject<MapHeader>()->liveSize() == 1);
    Rooted<Value> v25{Value::fromDouble(25.0)};
    CHECK(MapHeader::find(rtHeap(), growRes, v25) != UINT32_MAX);

    // 1b. Set.prototype.intersection when other.keys() deletes `undefined` from `self`
    Rooted<Value> sShrink{rtNewSet()};
    Rooted<Value> undef{Value::fromUndefined()};
    Rooted<Value> v1{Value::fromDouble(1.0)};
    Rooted<Value> v2{Value::fromDouble(2.0)};
    MapHeader::set(rtHeap(), sShrink, undef, undef);
    MapHeader::set(rtHeap(), sShrink, v1, v1);
    MapHeader::set(rtHeap(), sShrink, v2, v2);

    Rooted<Value> otherShrink{Value(bronze_create_object())};
    Rooted<Value> shrinkKeys{rtMakeNativeClosure(mutatingShrinkKeysFn, sShrink, 0)};
    bronze_elem_set(otherShrink.get().rawBits(), sizeKey.get().rawBits(), oneVal.get().rawBits(),
                    true);
    bronze_elem_set(otherShrink.get().rawBits(), hasKey.get().rawBits(), proxyHasFn.get().rawBits(),
                    true);
    bronze_elem_set(otherShrink.get().rawBits(), keysKey.get().rawBits(), shrinkKeys.get().rawBits(),
                    true);

    Value shrinkArgs[1] = {otherShrink.get()};
    Rooted<Value> shrinkRes{Value(bronze_dynamic_call(
        intersectFn.get().rawBits(), sShrink.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(shrinkArgs)))};
    REQUIRE(rtIsSetKind(shrinkRes.get()));
    // `undefined` was deleted from `sShrink` before the walk completed, so only `1` remains!
    CHECK(shrinkRes.get().asObject<MapHeader>()->liveSize() == 1);
    CHECK(MapHeader::find(rtHeap(), shrinkRes, undef) == UINT32_MAX);
    CHECK(MapHeader::find(rtHeap(), shrinkRes, v1) != UINT32_MAX);

    // 2. Re-entrant helper.return() while StateRunning throws TypeError
    Rooted<Value> iterProto{rtIteratorSharedPrototype()};
    Rooted<Value> mapKey{rtMakeString("map")};
    Rooted<Value> flatMapKey{rtMakeString("flatMap")};
    Rooted<Value> reduceKey{rtMakeString("reduce")};
    Rooted<Value> nextKey{rtMakeString("next")};
    Rooted<Value> returnKey{rtMakeString("return")};
    Rooted<Value> mapMethod{
        Value(bronze_elem_get(iterProto.get().rawBits(), mapKey.get().rawBits()))};
    Rooted<Value> flatMapMethod{
        Value(bronze_elem_get(iterProto.get().rawBits(), flatMapKey.get().rawBits()))};
    Rooted<Value> reduceMethod{
        Value(bronze_elem_get(iterProto.get().rawBits(), reduceKey.get().rawBits()))};

    Rooted<Value> baseIter{rtNewIteratorObject(IteratorProto::Helper)};
    // Build a custom iterator inheriting %Iterator.prototype% via Iterator.from
    Rooted<Value> iterCtor{rtIteratorConstructor("Iterator")};
    Rooted<Value> fromKey{rtMakeString("from")};
    Rooted<Value> iterFromFn{
        Value(bronze_elem_get(iterCtor.get().rawBits(), fromKey.get().rawBits()))};
    Rooted<Value> rawIter1{Value(bronze_create_object())};
    Rooted<Value> infNextFn{rtNativeFunction(infiniteOneNext, 0, "next", 0)};
    bronze_elem_set(rawIter1.get().rawBits(), nextKey.get().rawBits(), infNextFn.get().rawBits(),
                    true);
    Value fromArgs1[1] = {rawIter1.get()};
    Rooted<Value> wrapped1{Value(bronze_dynamic_call(
        iterFromFn.get().rawBits(), iterCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromArgs1)))};

    // Create a placeholder closure whose env we update to the helper itself
    Rooted<Value> dummyEnv{Value::fromUndefined()};
    Rooted<Value> reentrantFn{rtMakeNativeClosure(reentrantReturnMapper, dummyEnv, 1)};
    Value mapArgs1[1] = {reentrantFn.get()};
    Rooted<Value> helper1{Value(bronze_dynamic_call(
        mapMethod.get().rawBits(), wrapped1.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(mapArgs1)))};
    reentrantFn.get().asObject<FunctionHeader>()->env_record = helper1.get();

    Rooted<Value> helperNextFn{
        Value(bronze_elem_get(helper1.get().rawBits(), nextKey.get().rawBits()))};
    Value thrown;
    CHECK(rtTryCatch(
        [&] { bronze_dynamic_call(helperNextFn.get().rawBits(), helper1.get().rawBits(), 0, nullptr); },
        thrown));

    // 3. flatMap helper.return() closes outer iterator even when inner iterator's return() throws
    Rooted<Value> rawOuter{Value(bronze_create_object())};
    bronze_elem_set(rawOuter.get().rawBits(), nextKey.get().rawBits(), infNextFn.get().rawBits(),
                    true);
    Rooted<Value> outerRetFn{rtNativeFunction(countingOuterReturn, 0, "return", 0)};
    bronze_elem_set(rawOuter.get().rawBits(), returnKey.get().rawBits(), outerRetFn.get().rawBits(),
                    true);
    Value fromArgs2[1] = {rawOuter.get()};
    Rooted<Value> wrappedOuter{Value(bronze_dynamic_call(
        iterFromFn.get().rawBits(), iterCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromArgs2)))};
    Rooted<Value> fmMapper{rtNativeFunction(flatMapInnerMapper, 1, "fm", 1)};
    Value fmArgs[1] = {fmMapper.get()};
    Rooted<Value> fmHelper{Value(bronze_dynamic_call(
        flatMapMethod.get().rawBits(), wrappedOuter.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fmArgs)))};
    // Step once so inner iterator is active
    bronze_dynamic_call(helperNextFn.get().rawBits(), fmHelper.get().rawBits(), 0, nullptr);
    g_outerReturnCalls = 0;
    Rooted<Value> helperRetFn{
        Value(bronze_elem_get(fmHelper.get().rawBits(), returnKey.get().rawBits()))};
    CHECK(rtTryCatch(
        [&] { bronze_dynamic_call(helperRetFn.get().rawBits(), fmHelper.get().rawBits(), 0, nullptr); },
        thrown));
    CHECK(g_outerReturnCalls == 1);

    // 4. Non-callable return (42) and non-object return() result in wrapReturn / closeIteratorNow
    Rooted<Value> rawBadRet{Value(bronze_create_object())};
    bronze_elem_set(rawBadRet.get().rawBits(), nextKey.get().rawBits(), infNextFn.get().rawBits(),
                    true);
    Rooted<Value> num42{Value::fromDouble(42.0)};
    bronze_elem_set(rawBadRet.get().rawBits(), returnKey.get().rawBits(), num42.get().rawBits(),
                    true);
    Value fromBadRetArgs[1] = {rawBadRet.get()};
    Rooted<Value> wrappedBadRet{Value(bronze_dynamic_call(
        iterFromFn.get().rawBits(), iterCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromBadRetArgs)))};
    Rooted<Value> wrapRetFn{
        Value(bronze_elem_get(wrappedBadRet.get().rawBits(), returnKey.get().rawBits()))};
    CHECK(rtTryCatch(
        [&] {
            bronze_dynamic_call(wrapRetFn.get().rawBits(), wrappedBadRet.get().rawBits(), 0,
                                nullptr);
        },
        thrown));

    // Non-object return() result in closeIteratorNow (via helperReturn)
    Rooted<Value> rawNonObjRet{Value(bronze_create_object())};
    bronze_elem_set(rawNonObjRet.get().rawBits(), nextKey.get().rawBits(), infNextFn.get().rawBits(),
                    true);
    Rooted<Value> nonObjRetFn{rtNativeFunction(nonObjectReturn, 0, "return", 0)};
    bronze_elem_set(rawNonObjRet.get().rawBits(), returnKey.get().rawBits(),
                    nonObjRetFn.get().rawBits(), true);
    Value fromNonObjArgs[1] = {rawNonObjRet.get()};
    Rooted<Value> wrappedNonObj{Value(bronze_dynamic_call(
        iterFromFn.get().rawBits(), iterCtor.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(fromNonObjArgs)))};
    Value mapArgs2[1] = {proxyHasFn.get()};
    Rooted<Value> helperNonObj{Value(bronze_dynamic_call(
        mapMethod.get().rawBits(), wrappedNonObj.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(mapArgs2)))};
    CHECK(rtTryCatch(
        [&] {
            bronze_dynamic_call(helperRetFn.get().rawBits(), helperNonObj.get().rawBits(), 0,
                                nullptr);
        },
        thrown));

    // 5. Callable Proxy reducer in Iterator.prototype.reduce
    Rooted<Value> takeKey{rtMakeString("take")};
    Rooted<Value> takeMethod{
        Value(bronze_elem_get(iterProto.get().rawBits(), takeKey.get().rawBits()))};
    Value takeArgs[1] = {Value::fromDouble(3.0)};
    Rooted<Value> taken3{Value(bronze_dynamic_call(
        takeMethod.get().rawBits(), wrapped1.get().rawBits(), 1,
        reinterpret_cast<const uint64_t*>(takeArgs)))};
    Rooted<Value> rawReducer{rtNativeFunction(sumReducer, 2, "sum", 2)};
    Rooted<Value> proxyReducer{
        Value::fromObject(ProxyHeader::create(rtHeap(), rawReducer, emptyHandler))};
    Value reduceArgs[2] = {proxyReducer.get(), Value::fromDouble(0.0)};
    Rooted<Value> sumVal{Value(bronze_dynamic_call(
        reduceMethod.get().rawBits(), taken3.get().rawBits(), 2,
        reinterpret_cast<const uint64_t*>(reduceArgs)))};
    CHECK(sumVal.get().asNumber() == 3.0);
}

