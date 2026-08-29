#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"

using namespace bronze;

TEST_CASE("ObjectHeader property access and inline cache") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    REQUIRE(obj.get() != nullptr);

    Rooted<Value> key_a(Value::fromString(StringHeader::createFromUTF8(heap, "a")));
    Rooted<Value> key_b(Value::fromString(StringHeader::createFromUTF8(heap, "b")));
    Rooted<Value> val_a(Value::fromDouble(10.0));
    Rooted<Value> val_b(Value::fromDouble(20.0));

    InlineCache ic_set_a;
    InlineCache ic_set_b;
    // Read sites are SITES now (BRONZE_ABI_IC_WAYS entries); a first install
    // lands at way 0, which is what these assertions read.
    InlineCacheSite ic_get_a{};
    InlineCacheSite ic_get_b{};

    obj.get()->setProp(heap, arena, key_a, val_a, &ic_set_a);
    obj.get()->setProp(heap, arena, key_b, val_b, &ic_set_b);

    CHECK(ic_set_a.cached_slot == 0);
    CHECK(ic_set_b.cached_slot == 1);

    Value res_a = obj.get()->getProp(heap, key_a, &ic_get_a);
    Value res_b = obj.get()->getProp(heap, key_b, &ic_get_b);

    CHECK(res_a.isNumber());
    CHECK(res_a.asNumber() == 10.0);
    CHECK(res_b.isNumber());
    CHECK(res_b.asNumber() == 20.0);

    CHECK(ic_get_a.ways[0].cached_shape == obj.get()->shape);
    CHECK(ic_get_a.ways[0].cached_slot == 0);
    CHECK(ic_get_b.ways[0].cached_shape == obj.get()->shape);
    CHECK(ic_get_b.ways[0].cached_slot == 1);

    // Fast path IC hit
    Value res_a_ic = obj.get()->getProp(heap, key_a, &ic_get_a);
    CHECK(res_a_ic.asNumber() == 10.0);
}

TEST_CASE("properties beyond the inline slots spill to the overflow block") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    constexpr uint32_t kProps = 12;
    static_assert(kProps > ObjectHeader::kInlineSlots);

    for (uint32_t i = 0; i < kProps; ++i) {
        std::string name = "p" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        Rooted<Value> val(Value::fromDouble(static_cast<double>(i) * 1.5));
        ObjectHeader* live = obj.get()->setProp(heap, arena, key, val);
        obj = live;
    }

    CHECK(obj.get()->overflowCapacity() >= kProps - ObjectHeader::kInlineSlots);

    for (uint32_t i = 0; i < kProps; ++i) {
        std::string name = "p" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        Value v = obj.get()->getProp(heap, key);
        CHECK(v.isNumber());
        CHECK(v.asNumber() == static_cast<double>(i) * 1.5);
    }

    // Overwrite one out-of-line property in place (no new transition).
    {
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p9")));
        Rooted<Value> val(Value::fromDouble(-4.25));
        obj = obj.get()->setProp(heap, arena, key, val);
        Rooted<Value> key2(Value::fromString(StringHeader::createFromUTF8(heap, "p9")));
        CHECK(obj.get()->getProp(heap, key2).asNumber() == -4.25);
    }

    // Rooted string values stored out-of-line survive an explicit collection.
    {
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "s")));
        Rooted<Value> val(Value::fromString(StringHeader::createFromUTF8(heap, "spilled")));
        obj = obj.get()->setProp(heap, arena, key, val);
        heap.collect();
        Rooted<Value> key2(Value::fromString(StringHeader::createFromUTF8(heap, "s")));
        Value v = obj.get()->getProp(heap, key2);
        REQUIRE(v.isString());
        CHECK(v.asString<StringHeader>()->length == 7);
        CHECK(v.asString<StringHeader>()->charCodeAt(0) == 's');
    }
}

// The receiver's shape cannot see a property added to an object BETWEEN the
// receiver and the holder, so a depth > 1 entry needs the epoch. The oracle
// case pins the behaviour end to end; this pins the mechanism, so that a change
// which quietly stops bumping fails in the module that owns it rather than 300
// s later in the three.js run.
TEST_CASE("an inline cache entry above depth 1 is invalidated by a prototype add") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    // leaf -> mid -> top, and only `top` has `p`.
    Rooted<ObjectHeader*> top(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> mid(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(top.get()))));
    Rooted<ObjectHeader*> leaf(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(mid.get()))));

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p")));
    Rooted<Value> fromTop(Value::fromDouble(1.0));
    top.get()->setProp(heap, arena, key, fromTop);

    InlineCacheSite ic{};
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 1.0);
    // Warm, and at the depth the receiver's shape alone cannot vouch for.
    CHECK(ic.ways[0].cached_shape == leaf.get()->shape);
    CHECK(ic.ways[0].cached_depth == 2);
    const uint64_t filledAt = ic.ways[0].cached_epoch;

    // Shadow it on the INTERMEDIATE. `mid`'s shape changes; `leaf`'s does not,
    // which is exactly why the shape compare is not enough here.
    Shape* leafShapeBefore = leaf.get()->shape;
    Rooted<Value> fromMid(Value::fromDouble(2.0));
    mid.get()->setProp(heap, arena, key, fromMid);
    CHECK(leaf.get()->shape == leafShapeBefore);
    CHECK(protoMutationEpoch() != filledAt);
    CHECK_FALSE(ic.ways[0].describes(leaf.get()->shape));

    // The warm site must move to the nearer holder, and agree with a cold one.
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 2.0);
    InlineCacheSite cold{};
    CHECK(leaf.get()->getProp(heap, key, &cold).asNumber() == 2.0);
    // Refilled IN PLACE: the stale entry already named this shape, so
    // move-to-front rewrites it rather than evicting three healthy ways.
    CHECK(ic.ways[0].cached_depth == 1);
}

// The other half of the same decision: an add to an object that is NOT a
// prototype must not bump, or every proto cache in the program dies whenever a
// loop constructs anything (measured at 40% on exactly that shape).
TEST_CASE("an ordinary object's property add does not disturb proto caches") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> proto(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> inst(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(proto.get()))));

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "p")));
    Rooted<Value> val(Value::fromDouble(7.0));
    proto.get()->setProp(heap, arena, key, val);

    InlineCacheSite ic{};
    CHECK(inst.get()->getProp(heap, key, &ic).asNumber() == 7.0);
    const uint64_t filledAt = ic.ways[0].cached_epoch;

    // A brand new object, never anybody's prototype, gaining two properties.
    Rooted<ObjectHeader*> other(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<Value> keyX(Value::fromString(StringHeader::createFromUTF8(heap, "x")));
    Rooted<Value> keyY(Value::fromString(StringHeader::createFromUTF8(heap, "y")));
    Rooted<Value> one(Value::fromDouble(1.0));
    other.get()->setProp(heap, arena, keyX, one);
    other.get()->setProp(heap, arena, keyY, one);

    CHECK(protoMutationEpoch() == filledAt);
    CHECK(ic.ways[0].describes(inst.get()->shape));
}

// ---- the receiver of an `Object` member that needs a property table ---------
//
// The oracle case beside this one (`object_descriptor_receivers`) pins what a
// PRIMITIVE receiver is told, which is a catchable TypeError. What it cannot
// reach is the other half of the same fix: an array and a function are objects,
// so telling them they are not was a false statement, and what they get now is
// a hard error that names the kind and the storage reason. A hard error ends
// the process, so it can only be observed here.
//
// The one member that has stopped refusing is `hasOwn` on a function, because
// only a refusal that is still TRUE is a ratchet: `length` and `name` now have
// somewhere to live, so the question has an answer.

TEST_CASE("an Object member that needs a property table names the receiver it refuses") {
    ShadowStackFrame frame;

    Rooted<Value> ns{runtime::rtObjectNamespace()};
    Rooted<Value> arr{Value(bronze_create_array(2))};
    // A real function object: every member of the `Object` namespace is one.
    // Read through the ordinary computed-key funnel and not off an ObjectHeader:
    // `Object` is itself a function object (20.1.1.1 makes it callable), so its
    // statics live in the side property table every function object carries, and
    // the only reader that knows where that is is the funnel a program uses.
    Rooted<Value> keysKey{runtime::rtMakeString("keys")};
    Rooted<Value> fn{Value(bronze_elem_get(ns.get().rawBits(), keysKey.get().rawBits()))};
    REQUIRE(fn.get().isObject());
    REQUIRE(fn.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function);
    Rooted<Value> view{
        Value::fromObject(TypedArrayHeader::create(runtime::rtHeap(), ElementKind::Uint8, 1))};

    // The receiver is read from its ROOT after the two lookups below, which
    // both allocate: under BRONZE_GC_STRESS every allocation moves the live
    // set, so a Value copied into an argument list before them would name dead
    // from-space and the refusal would report the wrong kind. The extra
    // arguments are numbers, which no collection can move — none of these
    // calls gets far enough to look at one.
    auto call = [&](const char* member, Rooted<Value>& recv, uint32_t extraArgs) {
        Rooted<Value> key{runtime::rtMakeString(member)};
        Rooted<Value> target{Value(bronze_elem_get(ns.get().rawBits(), key.get().rawBits()))};
        REQUIRE(target.get().isObject());
        Value args[3] = {recv.get(), Value::fromDouble(0), Value::fromDouble(0)};
        return Value(target.get().asObject<FunctionHeader>()->call(Value::fromUndefined(),
                                                                  1 + extraArgs, args));
    };

    setFatalHandler([](const char* msg) { throw std::runtime_error(msg); });

    // The kind is named, and so is what about it cannot be done — never "this
    // is not an object", which is what an array used to be told.
    CHECK_THROWS_WITH_AS(call("defineProperty", arr, 2),
                         doctest::Contains("Object.defineProperty on an array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("defineProperty", arr, 2),
                         doctest::Contains("its own keys are ELEMENTS and a `length`"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("getOwnPropertyDescriptor", arr, 1),
                         doctest::Contains("Object.getOwnPropertyDescriptor on an array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("getOwnPropertyDescriptors", arr, 0),
                         doctest::Contains("Object.getOwnPropertyDescriptors on an array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("defineProperties", arr, 1),
                         doctest::Contains("Object.defineProperties on an array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("assign", arr, 0), doctest::Contains("Object.assign on an array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("getOwnPropertyNames", arr, 0),
                         doctest::Contains("Object.getOwnPropertyNames on an array"),
                         std::runtime_error);

    // A function is the other receiver the old message lied about, and its
    // reason is a different one: the storage is a slot and a side object. Every
    // member that would have to WRITE one still refuses on it; `hasOwn`, which
    // only tests, no longer does — see the bottom of this case.

    // And a kind with no property table at all says exactly that.
    CHECK_THROWS_WITH_AS(call("getOwnPropertyNames", view, 0),
                         doctest::Contains("Object.getOwnPropertyNames on a typed array"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(call("getOwnPropertyNames", view, 0),
                         doctest::Contains("keeps no property table"), std::runtime_error);

    // A NUMBER target for `Object.assign`, which is the one member here whose
    // primitive case ToObject does not settle. The four that only READ own keys
    // answer a number with the empty answer and never build the box
    // (`cases/object_own_keys_primitive`); this one has to BUILD it, because
    // the box is what it returns — and now it can, because `Number.prototype`
    // exists for the box to be an instance of.
    Rooted<Value> number{Value::fromDouble(5)};
    Rooted<Value> boxed{call("assign", number, 1)};
    REQUIRE(boxed.get().isObject());
    Value data;
    REQUIRE(runtime::rtNumberWrapperData(boxed.get(), data));
    CHECK(data.asNumber() == 5.0);

    // A SYMBOL is the one primitive still refused, and its reason is not
    // bronze's coverage: 20.4.3 gives `Symbol.prototype` no [[SymbolData]]
    // slot, so there is no Symbol object in this runtime for the box to be.
    Rooted<Value> sym{runtime::rtMakeSymbol(Value::fromUndefined())};
    CHECK_THROWS_WITH_AS(call("assign", sym, 1),
                         doctest::Contains("Object.assign with a symbol as the target"),
                         std::runtime_error);

    // `Object.hasOwn` was on the list above and has come off it. The refusal's
    // REASON was accurate — an array's own keys are its elements and a `length`
    // that lives outside the shape — but that is the reason bronze cannot
    // DESCRIBE them, not the reason it cannot test for one. An existence test
    // needs `hasElem` and nothing else, and `length` is an own property of
    // every array (10.4.2), so the language's answer to `Object.hasOwn([1,2],
    // 0)` is `true` and refusing it was a wrong answer given loudly.
    // `getOwnPropertyNames` above stays refused: listing the keys is the part
    // that needs somewhere to put them.
    auto hasOwn = [&](Rooted<Value>& recv, Rooted<Value>& key) {
        Rooted<Value> name{runtime::rtMakeString("hasOwn")};
        Rooted<Value> target{Value(bronze_elem_get(ns.get().rawBits(), name.get().rawBits()))};
        REQUIRE(target.get().isObject());
        // A plain block, and safe for the reason `FunctionHeader::call`'s
        // arity vector is: nothing between here and the callee allocates, and
        // `objectHasOwn`'s first statement is the RootedArgs copy.
        Value args[2] = {recv.get(), key.get()};
        return Value(
            target.get().asObject<FunctionHeader>()->call(Value::fromUndefined(), 2, args));
    };

    // `arr` is two units long, so it can answer both of the questions an index
    // has here. The HOLE — which is not an own key — is reachable only through
    // `delete`, so it is pinned end to end in `cases/object_own_keys_array`.
    {
        Rooted<Value> element{Value::fromDouble(7)};
        arr.get().asObject<ArrayHeader>()->setElem(runtime::rtHeap(), 0, element);
        Rooted<Value> undefVal{Value::fromUndefined()};
        arr.get().asObject<ArrayHeader>()->setElem(runtime::rtHeap(), 1, undefVal);
    }
    Rooted<Value> keyZero{Value::fromDouble(0)};
    Rooted<Value> keyOne{Value::fromDouble(1)};
    Rooted<Value> keyTwo{Value::fromDouble(2)};
    Rooted<Value> keyLength{runtime::rtMakeString("length")};
    Rooted<Value> keyName{runtime::rtMakeString("nope")};
    CHECK(hasOwn(arr, keyZero).asBool());
    // An element that holds `undefined` is still an own key: the question is
    // about the KEY, and answering it from the value is how `a[1] = undefined`
    // and `delete a[1]` would come to look alike.
    CHECK(hasOwn(arr, keyOne).asBool());
    CHECK_FALSE(hasOwn(arr, keyTwo).asBool());   // past `length`
    CHECK(hasOwn(arr, keyLength).asBool());      // 10.4.2's own `length`
    CHECK_FALSE(hasOwn(arr, keyName).asBool());  // a name an array cannot hold

    // A FUNCTION has come off the same list, and for the same shape of reason:
    // the refusal said its own keys live in a `prototype` slot and a side
    // object, which is where they are, and testing for one needs neither a
    // shape nor anywhere to list them.
    Rooted<Value> keyPrototype{runtime::rtMakeString("prototype")};
    CHECK(hasOwn(fn, keyPrototype).asBool());   // 10.2.4, the slot
    CHECK_FALSE(hasOwn(fn, keyName).asBool());  // "nope", in neither place
    // `Object.keys` is a NATIVE builtin: no key index ever named it, so its
    // header carries neither `name` nor `length`, and answering `true` here
    // would be the plausible-but-wrong kind of answer. A function the COMPILER
    // created has both — pinned by the `function_name_length` oracle case,
    // which is where JS can actually run.
    Rooted<Value> keyFnName{runtime::rtMakeString("name")};
    CHECK_FALSE(hasOwn(fn, keyFnName).asBool());
    CHECK_FALSE(hasOwn(fn, keyLength).asBool());

    setFatalHandler(nullptr);
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
}

// ---- the multi-way site and its negative entries ---------------------------

TEST_CASE("a site holds several shapes, and the fifth rotates the oldest out") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "v")));
    Rooted<Value> pad0(Value::fromString(StringHeader::createFromUTF8(heap, "p0")));
    Rooted<Value> pad1(Value::fromString(StringHeader::createFromUTF8(heap, "p1")));
    Rooted<Value> pad2(Value::fromString(StringHeader::createFromUTF8(heap, "p2")));
    Rooted<Value> pad3(Value::fromString(StringHeader::createFromUTF8(heap, "p3")));
    Rooted<Value> zero(Value::fromDouble(0));

    // Five shapes, and `v` lands at a different SLOT in each: a way answering
    // for the wrong shape would read a real slot of the receiver and return a
    // pad's 0 rather than accidentally agreeing.
    Rooted<ObjectHeader*> o0(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> o1(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> o2(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> o3(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> o4(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    o1.set(o1.get()->setProp(heap, arena, pad0, zero));
    o2.set(o2.get()->setProp(heap, arena, pad0, zero));
    o2.set(o2.get()->setProp(heap, arena, pad1, zero));
    o3.set(o3.get()->setProp(heap, arena, pad0, zero));
    o3.set(o3.get()->setProp(heap, arena, pad1, zero));
    o3.set(o3.get()->setProp(heap, arena, pad2, zero));
    o4.set(o4.get()->setProp(heap, arena, pad0, zero));
    o4.set(o4.get()->setProp(heap, arena, pad1, zero));
    o4.set(o4.get()->setProp(heap, arena, pad2, zero));
    o4.set(o4.get()->setProp(heap, arena, pad3, zero));

    Rooted<Value> v1(Value::fromDouble(1));
    Rooted<Value> v2(Value::fromDouble(2));
    Rooted<Value> v3(Value::fromDouble(3));
    Rooted<Value> v4(Value::fromDouble(4));
    Rooted<Value> v5(Value::fromDouble(5));
    o0.set(o0.get()->setProp(heap, arena, key, v1));
    o1.set(o1.get()->setProp(heap, arena, key, v2));
    o2.set(o2.get()->setProp(heap, arena, key, v3));
    o3.set(o3.get()->setProp(heap, arena, key, v4));
    o4.set(o4.get()->setProp(heap, arena, key, v5));

    ObjectHeader* objs[5] = {o0.get(), o1.get(), o2.get(), o3.get(), o4.get()};

    InlineCacheSite site{};
    // Four shapes fit; each install lands at way 0 and pushes the rest down, so
    // after four reads the ways name the receivers in reverse order.
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(objs[i]->getProp(heap, key, &site).asNumber() == static_cast<double>(i) + 1.0);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(site.ways[i].cached_shape == objs[3 - i]->shape);
    }

    // The fifth evicts the least recently installed -- way 3, which is objs[0].
    CHECK(objs[4]->getProp(heap, key, &site).asNumber() == 5.0);
    CHECK(site.ways[0].cached_shape == objs[4]->shape);
    CHECK(site.find(objs[0]->shape, BRONZE_ABI_IC_WAYS) == nullptr);
    for (uint32_t i = 1; i < 5; ++i) {
        CHECK(site.find(objs[i]->shape, BRONZE_ABI_IC_WAYS) != nullptr);
    }

    // Re-reading a shape a way already names must REWRITE that way rather than
    // evict three healthy entries to say the same thing about a fourth.
    InlineCache* before = site.find(objs[2]->shape, BRONZE_ABI_IC_WAYS);
    CHECK(objs[2]->getProp(heap, key, &site).asNumber() == 3.0);
    CHECK(site.find(objs[2]->shape, BRONZE_ABI_IC_WAYS) == before);
    for (uint32_t i = 1; i < 5; ++i) {
        CHECK(site.find(objs[i]->shape, BRONZE_ABI_IC_WAYS) != nullptr);
    }

    // Every answer is still the receiver's own, whatever the ways hold.
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(objs[i]->getProp(heap, key, &site).asNumber() == static_cast<double>(i) + 1.0);
    }
}

TEST_CASE("BRONZE_NO_POLY_IC narrows a site to one way, reader and writer alike") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "v")));
    Rooted<Value> pad(Value::fromString(StringHeader::createFromUTF8(heap, "pad")));
    Rooted<Value> zero(Value::fromDouble(0));
    Rooted<Value> one(Value::fromDouble(1));
    Rooted<Value> two(Value::fromDouble(2));

    Rooted<ObjectHeader*> a(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> b(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    b.set(b.get()->setProp(heap, arena, pad, zero));
    a.set(a.get()->setProp(heap, arena, key, one));
    b.set(b.get()->setProp(heap, arena, key, two));

    bronze_tls_block_addr()->poly_ic_enabled = 0;
    InlineCacheSite site{};
    CHECK(a.get()->getProp(heap, key, &site).asNumber() == 1.0);
    CHECK(b.get()->getProp(heap, key, &site).asNumber() == 2.0);
    // The second install overwrote the first rather than moving into way 1: the
    // seam has to narrow the WRITER as well, or ways the reader will not look
    // at would fill with entries the seam is supposed to have prevented.
    CHECK(site.ways[0].cached_shape == b.get()->shape);
    CHECK(site.ways[1].cached_shape == nullptr);
    bronze_tls_block_addr()->poly_ic_enabled = 1;
}

TEST_CASE("an absent read is recorded, and the epoch is what retires it") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<ObjectHeader*> proto(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<ObjectHeader*> inst(ObjectHeader::create(
        heap, arena, Shape::createRoot(arena, Value::fromObject(proto.get()))));

    Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "missing")));

    // The walk says so, and the chain's structure allows the entry.
    bool witness = false;
    CHECK(inst.get()->getProp(heap, key, nullptr, nullptr, &witness).isUndefined());
    CHECK(witness);
    CHECK(inst.get()->chainIsCacheable());

    InlineCacheSite site{};
    runtime::rtInstallAbsentEntry(&site, Value::fromObject(inst.get()), "missing");
    CHECK(site.ways[0].isAbsent());
    CHECK(site.ways[0].describesAbsent(inst.get()->shape));
    // An absent entry is NOT a positive one: `describes` must refuse it, or a
    // caller would walk to depth 0x40000000 or read slot 0 off the receiver.
    CHECK_FALSE(site.ways[0].describes(inst.get()->shape));
    CHECK(inst.get()->getProp(heap, key, &site).isUndefined());

    // A property added to the PROTOTYPE leaves the receiver's shape alone, so
    // only the epoch can notice.
    Shape* instShapeBefore = inst.get()->shape;
    Rooted<Value> val(Value::fromDouble(11));
    proto.set(proto.get()->setProp(heap, arena, key, val));
    CHECK(inst.get()->shape == instShapeBefore);
    CHECK_FALSE(site.ways[0].describesAbsent(inst.get()->shape));
    CHECK(inst.get()->getProp(heap, key, &site).asNumber() == 11.0);
    CHECK_FALSE(site.ways[0].isAbsent());
}

TEST_CASE("a chain the epoch does not cover is refused a negative entry") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    // A receiver in dictionary mode: its slots are not shape-indexed, and its
    // shape belongs to it alone.
    {
        Rooted<Value> self{
            Value::fromObject(ObjectHeader::create(heap, arena, Shape::createRoot(arena)))};
        ObjectHeader::toDictionary(arena, self);
        CHECK_FALSE(self.get().asObject<ObjectHeader>()->chainIsCacheable());
    }

    // A link that is not a plain object cannot be walked, so the chain never
    // reaches an end and absence is never proven.
    {
        Rooted<Value> arrVal{Value::fromObject(ArrayHeader::create(heap, 0))};
        Rooted<ObjectHeader*> inst(
            ObjectHeader::create(heap, arena, Shape::createRoot(arena, arrVal.get())));
        CHECK_FALSE(inst.get()->chainIsCacheable());
    }

    // A prototype whose shape is not MARKED: an add to it would not bump the
    // epoch, so nothing could retire an entry filled over it. Real chains are
    // marked by Shape::createRoot; this one is unmarked by hand to pin that the
    // check is the cache's own and not an assumption about that function.
    {
        Rooted<ObjectHeader*> proto(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
        Rooted<ObjectHeader*> inst(ObjectHeader::create(
            heap, arena, Shape::createRoot(arena, Value::fromObject(proto.get()))));
        CHECK(inst.get()->chainIsCacheable());
        proto.get()->shape->used_as_prototype = false;
        CHECK_FALSE(inst.get()->chainIsCacheable());
        proto.get()->shape->used_as_prototype = true;
    }
}

TEST_CASE("Object.defineProperty reads its descriptor's fields off the whole chain") {
    // 6.2.6.5 asks HasProperty and then Get for each of six field names, and
    // the decode answers both from one walk of the descriptor's prototype chain
    // wherever the field is a data slot. What that walk owes the pair of
    // generic calls it stands in for is here: an INHERITED field is a mentioned
    // field, and a field whose value is `undefined` is mentioned too — the
    // question is presence, not truthiness, and collapsing the two silently
    // freezes a property the descriptor meant to leave alone.
    ShadowStackFrame frame;
    Heap& heap = runtime::rtHeap();
    NonMovingArena& arena = runtime::rtArena();

    auto setField = [](Rooted<Value>& obj, const char* name, Value v) {
        Rooted<Value> key{runtime::rtMakeString(name)};
        Rooted<Value> val{v};
        obj.get().asObject<ObjectHeader>()->setProp(runtime::rtHeap(), runtime::rtArena(), key,
                                                    val);
    };
    auto define = [](Rooted<Value>& target, const char* name, Rooted<Value>& desc) {
        Rooted<Value> key{runtime::rtMakeString(name)};
        const uint64_t args[3] = {target.get().rawBits(), key.get().rawBits(),
                                  desc.get().rawBits()};
        runtime::rtObjectDefineProperty(0, 0, 3, args);
    };
    auto lookup = [](Rooted<Value>& obj, const char* name, PropertyInfo& out) {
        Rooted<Value> key{runtime::rtMakeString(name)};
        auto* o = obj.get().asObject<ObjectHeader>();
        return o->shape != nullptr &&
               o->shape->lookupProperty(PropertyKey::fromValue(key.get()), out);
    };

    SUBCASE("a field inherited from the descriptor's prototype is a mentioned field") {
        Rooted<Value> descProto{Value(bronze_create_object())};
        setField(descProto, "enumerable", Value::fromBool(true));
        Rooted<Value> desc{Value::fromObject(ObjectHeader::create(
            heap, arena, runtime::rtRootShapeForPrototype(descProto.get())))};
        setField(desc, "value", Value::fromDouble(7));

        Rooted<Value> target{Value(bronze_create_object())};
        define(target, "c", desc);

        PropertyInfo info;
        REQUIRE(lookup(target, "c", info));
        CHECK(target.get().asObject<ObjectHeader>()->getSlot(info.slot).asNumber() == 7.0);
        // `enumerable` came off the prototype; the two the descriptor is silent
        // about default to false on a new property (10.1.6.3 step 3).
        CHECK(info.enumerable);
        CHECK_FALSE(info.writable);
        CHECK_FALSE(info.configurable);
    }

    SUBCASE("`value: undefined` edits the value and leaves the attributes alone") {
        Rooted<Value> target{Value(bronze_create_object())};
        {
            Rooted<Value> full{Value(bronze_create_object())};
            setField(full, "value", Value::fromDouble(1));
            setField(full, "writable", Value::fromBool(true));
            setField(full, "enumerable", Value::fromBool(true));
            setField(full, "configurable", Value::fromBool(true));
            define(target, "b", full);
        }
        {
            Rooted<Value> onlyValue{Value(bronze_create_object())};
            setField(onlyValue, "value", Value::fromUndefined());
            define(target, "b", onlyValue);
        }
        PropertyInfo info;
        REQUIRE(lookup(target, "b", info));
        CHECK(target.get().asObject<ObjectHeader>()->getSlot(info.slot).isUndefined());
        CHECK(info.writable);
        CHECK(info.enumerable);
        CHECK(info.configurable);
    }

    SUBCASE("an empty descriptor mentions nothing and edits nothing") {
        Rooted<Value> target{Value(bronze_create_object())};
        {
            Rooted<Value> full{Value(bronze_create_object())};
            setField(full, "value", Value::fromDouble(1));
            setField(full, "writable", Value::fromBool(true));
            setField(full, "enumerable", Value::fromBool(true));
            setField(full, "configurable", Value::fromBool(true));
            define(target, "b", full);
        }
        {
            Rooted<Value> empty{Value(bronze_create_object())};
            define(target, "b", empty);
        }
        PropertyInfo info;
        REQUIRE(lookup(target, "b", info));
        CHECK(target.get().asObject<ObjectHeader>()->getSlot(info.slot).asNumber() == 1.0);
        CHECK(info.writable);
        CHECK(info.enumerable);
        CHECK(info.configurable);
    }
}
