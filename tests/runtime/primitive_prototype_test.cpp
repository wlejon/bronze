// The four intrinsic prototypes a PRIMITIVE reaches, at the level below the
// oracle cases: identity, the brand that separates a wrapper from everything
// else with an internal slot, and the rooting that building one lazily demands.
//
// `cases/number_prototype_chain` and `cases/symbol_prototype` pin what a
// program sees. What is here is what a program cannot see and a collection can
// break — because the whole of "a real prototype" is that ONE object, built on
// first use, survives every allocation that first use triggers, and is the same
// object next time.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;

namespace {

// A member of an object by name, with no allocation once the key exists.
Value member(Rooted<Value>& obj, const char* name) {
    Rooted<Value> key{runtime::rtMakeString(name)};
    return obj.get().asObject<ObjectHeader>()->getProp(runtime::rtHeap(), key);
}

// The property path's answer for a primitive receiver, which is the route a
// program's `(5).toFixed` takes.
Value primitiveMember(Value prim, const char* name) {
    Rooted<Value> self{prim};
    Rooted<Value> key{runtime::rtMakeString(name)};
    StringHeader* header = key.get().asString<StringHeader>();
    return runtime::rtPrimitiveMember(self.get(), std::string(name), header, /*ic=*/nullptr);
}

}  // namespace

TEST_CASE("Number.prototype is one object and its members are one object each") {
    ShadowStackFrame frame;

    Rooted<Value> proto{runtime::rtNumberPrototype()};
    REQUIRE(proto.get().isObject());
    // Idempotent: the second call is the memoized intrinsic, not a second build.
    CHECK(runtime::rtNumberPrototype().rawBits() == proto.get().rawBits());

    // 21.1.3 makes it a Number object with [[NumberData]] +0, which is what
    // `Object.prototype.toString.call(Number.prototype)` reads and what makes
    // `Number.prototype + 1` be 1. The brand is the slot, so the intrinsic is
    // one of the objects its own test includes.
    Value data;
    REQUIRE(runtime::rtNumberWrapperData(proto.get(), data));
    CHECK(data.asNumber() == 0.0);

    // Its own [[Prototype]] is `Object.prototype` — the chain a number walks is
    // Number.prototype, Object.prototype, null.
    Rooted<Value> objectProto{runtime::rtObjectPrototype()};
    CHECK(proto.get().asObject<ObjectHeader>()->shape->prototypeValue().rawBits() ==
          objectProto.get().rawBits());

    // The identity that separates a prototype from a second copy of a member
    // table: the function object a NUMBER answers with is the one installed on
    // the intrinsic, not a fresh one per read.
    const Value fromProto = member(proto, "toFixed");
    REQUIRE(fromProto.isObject());
    CHECK(primitiveMember(Value::fromDouble(5.0), "toFixed").rawBits() == fromProto.rawBits());
    CHECK(primitiveMember(Value::fromDouble(-1.5), "toFixed").rawBits() == fromProto.rawBits());
    // And twice from the same receiver, which is the spelling the oracle case
    // uses and the one that would catch a per-read mint on its own.
    CHECK(primitiveMember(Value::fromDouble(5.0), "valueOf").rawBits() ==
          primitiveMember(Value::fromDouble(5.0), "valueOf").rawBits());

    // 21.1.3.1's back-pointer is the object the bare name `Number` resolves to.
    CHECK(member(proto, "constructor").rawBits() == runtime::rtNumberConstructorObject().rawBits());
}

TEST_CASE("Symbol.prototype is an ordinary object, not a wrapper") {
    ShadowStackFrame frame;

    Rooted<Value> proto{runtime::rtSymbolPrototype()};
    REQUIRE(proto.get().isObject());
    CHECK(runtime::rtSymbolPrototype().rawBits() == proto.get().rawBits());

    // 20.4.3: "it is not a Symbol instance and does not have a [[SymbolData]]
    // internal slot". So it must NOT answer the wrapper brand — which is the
    // one thing that would make `Object.prototype.toString.call(it)` and
    // `rtWrapperPrimitive` treat it as a boxed primitive.
    Value data;
    CHECK_FALSE(runtime::rtNumberWrapperData(proto.get(), data));
    CHECK_FALSE(runtime::rtStringWrapperData(proto.get(), data));
    CHECK_FALSE(runtime::rtBooleanWrapperData(proto.get(), data));
    CHECK(proto.get().asObject<ObjectHeader>()->internalSlotCount() == 0);

    Rooted<Value> objectProto{runtime::rtObjectPrototype()};
    CHECK(proto.get().asObject<ObjectHeader>()->shape->prototypeValue().rawBits() ==
          objectProto.get().rawBits());

    Rooted<Value> sym{runtime::rtMakeSymbol(Value::fromUndefined())};
    const Value fromProto = member(proto, "toString");
    REQUIRE(fromProto.isObject());
    CHECK(primitiveMember(sym.get(), "toString").rawBits() == fromProto.rawBits());

    // 20.4.3.2 is an ACCESSOR, and the shape has to say so — a data property
    // holding the getter would read as the function rather than calling it.
    Rooted<Value> key{runtime::rtMakeString("description")};
    PropertyInfo info;
    REQUIRE(proto.get().asObject<ObjectHeader>()->shape->lookupProperty(
        key.get().asString<StringHeader>(), info));
    CHECK(info.accessor);
    CHECK_FALSE(info.enumerable);
}

TEST_CASE("every member of the four intrinsic prototypes is non-enumerable") {
    ShadowStackFrame frame;

    // The attribute that made it survivable to slide a new link under every
    // primitive in a suite of pinned bytes: `for-in` walks the prototype chain,
    // so one enumerable member here would appear in every for-in over every
    // number, string, boolean and symbol in every program.
    Rooted<Value> protos[4] = {
        Rooted<Value>{runtime::rtNumberPrototype()},
        Rooted<Value>{runtime::rtStringPrototype()},
        Rooted<Value>{runtime::rtBooleanPrototype()},
        Rooted<Value>{runtime::rtSymbolPrototype()},
    };
    for (Rooted<Value>& proto : protos) {
        const std::vector<StringHeader*> keys =
            runtime::rtOwnStringKeysOrdered(proto.get().asObject<ObjectHeader>());
        CHECK(keys.empty());
    }
}

TEST_CASE("building an intrinsic prototype survives the allocation it triggers") {
    ShadowStackFrame frame;

    // The hazard this file exists for, and the one the previous chunk found in
    // rt_prop_primitive.cpp: the FIRST member read of a program builds the
    // intrinsic, and that build allocates a great deal — an object, a shape,
    // and every method on it. A raw `StringHeader*` held across it points into
    // dead from-space, and the walk then looks up a collected name.
    //
    // Under BRONZE_GC_STRESS every allocation collects, so a key that is not
    // rooted across the build cannot survive to be compared. The check is that
    // the name still resolves — a stale key answers `undefined`, which is a
    // silent wrong answer and exactly what this would look like.
    Rooted<Value> five{Value::fromDouble(5.0)};
    Rooted<Value> key{runtime::rtMakeString("toFixed")};
    // A HEAP key, deliberately: this is what `bronze_elem_get` hands down for a
    // computed read, and an arena-interned one would not move and would not
    // test anything.
    StringHeader* header = key.get().asString<StringHeader>();
    const Value found =
        runtime::rtPrimitiveMember(five.get(), std::string("toFixed"), header, /*ic=*/nullptr);
    REQUIRE(found.isObject());
    CHECK(found.asObject<HeapObjectHeader>()->flags == HeapKind::Function);

    // The same for a symbol, whose intrinsic is built by a different
    // initializer and whose `description` accessor allocates on top of it.
    Rooted<Value> sym{runtime::rtMakeSymbol(runtime::rtMakeString("tag"))};
    Rooted<Value> descKey{runtime::rtMakeString("description")};
    StringHeader* descHeader = descKey.get().asString<StringHeader>();
    Rooted<Value> desc{runtime::rtPrimitiveMember(sym.get(), std::string("description"),
                                                  descHeader, /*ic=*/nullptr)};
    REQUIRE(desc.get().isString());
    CHECK(runtime::rtAsciiChars(desc.get().asString<StringHeader>()) == "tag");
}

TEST_CASE("a Number wrapper is branded by its slot and nothing else is") {
    ShadowStackFrame frame;

    Rooted<Value> wrapper{runtime::rtMakeNumberWrapper(7.5)};
    Value data;
    REQUIRE(runtime::rtNumberWrapperData(wrapper.get(), data));
    CHECK(data.asNumber() == 7.5);
    // The brand is the slot COUNT paired with the slot's TYPE, so the three
    // wrapper kinds must not answer each other's test — a Number object read as
    // a String object would be a `StringHeader*` cast over a double.
    CHECK_FALSE(runtime::rtStringWrapperData(wrapper.get(), data));
    CHECK_FALSE(runtime::rtBooleanWrapperData(wrapper.get(), data));

    // 21.1.3's thisNumberValue takes the primitive or the box and refuses
    // everything else.
    Value out;
    CHECK(runtime::rtThisNumberValue(Value::fromDouble(3.0), out));
    CHECK(out.asNumber() == 3.0);
    CHECK(runtime::rtThisNumberValue(wrapper.get(), out));
    CHECK(out.asNumber() == 7.5);
    CHECK_FALSE(runtime::rtThisNumberValue(Value::fromBool(true), out));
    Rooted<Value> plain{Value::fromObject(
        ObjectHeader::create(runtime::rtHeap(), runtime::rtArena(), runtime::rtPlainObjectShape()))};
    plain.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    CHECK_FALSE(runtime::rtThisNumberValue(plain.get(), out));

    // 7.1.1's shortcut for a pristine wrapper: the answer OrdinaryToPrimitive
    // would produce, without running it. It is what makes `new Number(1) == 1`
    // true from a site that cannot call user code.
    Value prim;
    REQUIRE(runtime::rtWrapperPrimitive(wrapper.get(), prim));
    CHECK(prim.asNumber() == 7.5);
    CHECK_FALSE(runtime::rtWrapperPrimitive(plain.get(), prim));
}
