#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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
    InlineCache ic_get_a;
    InlineCache ic_get_b;

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

    CHECK(ic_get_a.cached_shape == obj.get()->shape);
    CHECK(ic_get_a.cached_slot == 0);
    CHECK(ic_get_b.cached_shape == obj.get()->shape);
    CHECK(ic_get_b.cached_slot == 1);

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

    InlineCache ic;
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 1.0);
    // Warm, and at the depth the receiver's shape alone cannot vouch for.
    CHECK(ic.cached_shape == leaf.get()->shape);
    CHECK(ic.cached_depth == 2);
    const uint64_t filledAt = ic.cached_epoch;

    // Shadow it on the INTERMEDIATE. `mid`'s shape changes; `leaf`'s does not,
    // which is exactly why the shape compare is not enough here.
    Shape* leafShapeBefore = leaf.get()->shape;
    Rooted<Value> fromMid(Value::fromDouble(2.0));
    mid.get()->setProp(heap, arena, key, fromMid);
    CHECK(leaf.get()->shape == leafShapeBefore);
    CHECK(protoMutationEpoch() != filledAt);
    CHECK_FALSE(ic.describes(leaf.get()->shape));

    // The warm site must move to the nearer holder, and agree with a cold one.
    CHECK(leaf.get()->getProp(heap, key, &ic).asNumber() == 2.0);
    InlineCache cold;
    CHECK(leaf.get()->getProp(heap, key, &cold).asNumber() == 2.0);
    CHECK(ic.cached_depth == 1);
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

    InlineCache ic;
    CHECK(inst.get()->getProp(heap, key, &ic).asNumber() == 7.0);
    const uint64_t filledAt = ic.cached_epoch;

    // A brand new object, never anybody's prototype, gaining two properties.
    Rooted<ObjectHeader*> other(ObjectHeader::create(heap, arena, Shape::createRoot(arena)));
    Rooted<Value> keyX(Value::fromString(StringHeader::createFromUTF8(heap, "x")));
    Rooted<Value> keyY(Value::fromString(StringHeader::createFromUTF8(heap, "y")));
    Rooted<Value> one(Value::fromDouble(1.0));
    other.get()->setProp(heap, arena, keyX, one);
    other.get()->setProp(heap, arena, keyY, one);

    CHECK(protoMutationEpoch() == filledAt);
    CHECK(ic.describes(inst.get()->shape));
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
