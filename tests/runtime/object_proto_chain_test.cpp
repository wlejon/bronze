// `Object.prototype`'s members reached from a receiver that has no shape, and
// the own-property questions two of them ask about storage that is in no shape
// either.
//
// The bug this pins is not a wrong answer, it is a MISSING step. A function, an
// array, a Map, a Set, a RegExp, a typed array, an ArrayBuffer and a DataView
// each answer their members out of a C table standing in for a prototype object
// bronze has not built — and the search used to END at that table. So
// `f.toString` was diagnosed by name from `Function.prototype`, one link
// nearer, while `f.valueOf`, one link further up, read `undefined`: a silent
// fallback about a prototype bronze DOES have.
//
// Two things are checked at every kind, and the second is what makes this a
// chain rather than a ninth table. The member is found — and it is the SAME
// FUNCTION OBJECT a plain object finds, so a program that compares
// `a.hasOwnProperty === Object.prototype.hasOwnProperty`, or replaces the
// member, sees one object rather than a copy per receiver kind.
//
// `cases/object_proto_chain_end` pins the answers as a program sees them. What
// is here is the level below: identity, the exhaustiveness of the
// own-property dispatch, and the shadowing rule that keeps a nearer prototype's
// member ahead of this one.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

uint64_t nothing(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

// `receiver.<name>`, through the ordinary computed-read path — which delegates
// to the same by-name dispatch `o.k` takes, so this is the property read and
// not a second opinion about one.
Value member(Rooted<Value>& receiver, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return Value(bronze_elem_get(receiver.get().rawBits(), key.get().rawBits()));
}

// The member off `Object.prototype` itself, which is what every answer below is
// compared against.
Value protoMember(const char* name) {
    Rooted<Value> proto{rtObjectPrototype()};
    return member(proto, name);
}

bool isFunction(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// `receiver.<name>(arg)`, for the members that take one. The ARGUMENT is rooted
// before the member is looked up, because that lookup allocates the key string
// and a collection there would leave a raw `Value` pointing into dead
// from-space — which under BRONZE_GC_STRESS is every allocation.
Value invoke(Rooted<Value>& receiver, const char* name, Value arg) {
    Rooted<Value> argRoot{arg};
    Rooted<Value> fn{member(receiver, name)};
    REQUIRE(isFunction(fn.get()));
    Value args[1] = {argRoot.get()};
    return fn.get().asObject<FunctionHeader>()->call(receiver.get(), 1, args);
}

bool hasOwn(Rooted<Value>& receiver, const char* key) {
    Rooted<Value> keyStr{rtMakeString(key)};
    return invoke(receiver, "hasOwnProperty", keyStr.get()).asBool();
}

bool enumerableOwn(Rooted<Value>& receiver, const char* key) {
    Rooted<Value> keyStr{rtMakeString(key)};
    return invoke(receiver, "propertyIsEnumerable", keyStr.get()).asBool();
}

Value aRegExp() {
    Rooted<Value> src{rtMakeString("ab")};
    return rtRegExpFromParts(src, "g");
}

}  // namespace

// The tripwire, mirrored from builtin_object_proto.cpp's own-property switch.
// `flags` is a `uint16_t` and HeapKind is an unnamed enum, so nothing the
// compiler checks can prove that switch total — and `hasOwnProperty` is now
// reachable from EVERY receiver, so a kind with no arm there is a receiver
// those clauses cannot answer about.
static_assert(HeapKind::Count == 20,
              "a HeapKind was added or removed: give it an arm in the own-property switch in "
              "builtin_object_proto.cpp, and a receiver in the subcases below — unless it is "
              "a kind no program can hold (an environment record, an iteration record, a "
              "private-element table, an object's slot block), which the switch refuses by "
              "name and which therefore has no receiver to test.");

TEST_CASE("a receiver with no shape still reaches Object.prototype") {
    ShadowStackFrame frame;

    // Held across every subcase: identity against these is the whole check.
    Rooted<Value> hasOwnProperty{protoMember("hasOwnProperty")};
    Rooted<Value> isPrototypeOf{protoMember("isPrototypeOf")};
    Rooted<Value> propertyIsEnumerable{protoMember("propertyIsEnumerable")};
    Rooted<Value> valueOf{protoMember("valueOf")};
    Rooted<Value> toString{protoMember("toString")};
    REQUIRE(isFunction(hasOwnProperty.get()));

    // The three of 20.1.3 that NO prototype between a receiver and this object
    // redefines, so every kind must find these exact function objects. Compared
    // against the ROOTED copies above rather than against a second lookup: two
    // lookups in one expression are two allocations, and the first result would
    // be a raw pointer across the second's collection.
    auto reaches = [&](Rooted<Value>& receiver) {
        const auto hasOwn = member(receiver, "hasOwnProperty");
        CHECK(hasOwn.rawBits() == hasOwnProperty.get().rawBits());
        const auto isProto = member(receiver, "isPrototypeOf");
        CHECK(isProto.rawBits() == isPrototypeOf.get().rawBits());
        const auto propIsEnum = member(receiver, "propertyIsEnumerable");
        CHECK(propIsEnum.rawBits() == propertyIsEnumerable.get().rawBits());
    };
    // `valueOf` is one 21.1.3.7 and 20.4.3.4 DO redefine, so it is asked only of
    // the kinds whose intermediate prototype leaves it alone.
    auto reachesValueOf = [&](Rooted<Value>& receiver) {
        const auto valOf = member(receiver, "valueOf");
        CHECK(valOf.rawBits() == valueOf.get().rawBits());
    };

    SUBCASE("a function") {
        Rooted<Value> f{rtNativeFunction(nothing, 0)};
        reaches(f);
        reachesValueOf(f);
    }

    SUBCASE("an array") {
        Rooted<Value> a{Value(bronze_create_array(0))};
        reaches(a);
        reachesValueOf(a);
    }

    SUBCASE("a typed array, an ArrayBuffer and a DataView") {
        Rooted<Value> v{
            Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 2))};
        reaches(v);
        reachesValueOf(v);
        Rooted<Value> buf{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
        reaches(buf);
        reachesValueOf(buf);
        Rooted<Value> view{Value::fromObject(DataViewHeader::create(rtHeap(), buf, 0, 8))};
        reaches(view);
        reachesValueOf(view);
    }

    SUBCASE("a Map and a Set") {
        Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
        reaches(m);
        reachesValueOf(m);
        Rooted<Value> s{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
        reaches(s);
        reachesValueOf(s);
    }

    SUBCASE("a RegExp") {
        Rooted<Value> re{aRegExp()};
        reaches(re);
        reachesValueOf(re);
    }

    SUBCASE("a proxy with no get trap forwards, chain and all") {
        Rooted<Value> target{Value(bronze_create_object())};
        Rooted<Value> handler{Value(bronze_create_object())};
        const uint64_t ctorArgs[2] = {target.get().rawBits(), handler.get().rawBits()};
        Rooted<Value> p{Value(rtProxyConstructor(0, 0, 2, ctorArgs))};
        reaches(p);
        reachesValueOf(p);
    }

    // A primitive whose members are handed out beside the value rather than
    // found on a prototype object — the same arrangement, one level down. Both
    // of these DO carry a `valueOf` of their own (21.1.3.7, 20.4.3.4), so what
    // is checked is the three that pass through them.
    SUBCASE("a number and a symbol") {
        Rooted<Value> n{Value::fromDouble(5.0)};
        reaches(n);
        Rooted<Value> sym{rtMakeSymbol(Value::fromUndefined())};
        reaches(sym);
    }

    // The shadowing rule, from both sides. 22.2.6.13 gives `RegExp.prototype`
    // its own `toString`, which is NEARER than this object's and must win;
    // 24.1.3 gives `Map.prototype` none, so a Map gets 20.1.3.6's.
    SUBCASE("a nearer prototype's member wins, and only where there is one") {
        Rooted<Value> re{aRegExp()};
        CHECK(isFunction(member(re, "toString")));
        CHECK(member(re, "toString").rawBits() != toString.get().rawBits());

        Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
        CHECK(member(m, "toString").rawBits() == toString.get().rawBits());
    }
}

// 20.1.3.2 and 20.1.3.4 ask about the RECEIVER ALONE, so every answer here is
// about storage bronze keeps outside a shape — and about the members that only
// LOOK like own data because the property path answers them from a header.
TEST_CASE("hasOwnProperty and propertyIsEnumerable answer for a receiver with no shape") {
    ShadowStackFrame frame;

    SUBCASE("a function: 10.2.9, 10.2.10 and 10.2.11, all non-enumerable") {
        const uint32_t nameKey = bronze_register_key_string("adder");
        Rooted<Value> f{rtNativeFunction(nothing, 2)};
        rtSetFunctionNameAndLength(f.get().asObject<FunctionHeader>(), nameKey, 1);

        CHECK(hasOwn(f, "length"));
        CHECK(hasOwn(f, "name"));
        CHECK(hasOwn(f, "prototype"));
        // 20.2.3.3 puts `call` on `Function.prototype`, so it is inherited and
        // not own — which is exactly the difference from `'call' in f`.
        CHECK_FALSE(hasOwn(f, "call"));
        CHECK_FALSE(hasOwn(f, "missing"));
        CHECK_FALSE(enumerableOwn(f, "length"));
        CHECK_FALSE(enumerableOwn(f, "name"));
    }

    SUBCASE("a proxy: the target's own keys, because no vetted trap can differ") {
        Rooted<Value> target{Value(bronze_create_object())};
        Rooted<Value> key{rtMakeString("k")};
        Rooted<Value> val{Value::fromDouble(1.0)};
        target.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
        Rooted<Value> handler{Value(bronze_create_object())};
        const uint64_t ctorArgs[2] = {target.get().rawBits(), handler.get().rawBits()};
        Rooted<Value> p{Value(rtProxyConstructor(0, 0, 2, ctorArgs))};
        CHECK(hasOwn(p, "k"));
        CHECK(enumerableOwn(p, "k"));
        CHECK_FALSE(hasOwn(p, "missing"));
    }

    SUBCASE("an array: its indices and its length, and a hole is neither") {
        Rooted<Value> a{Value(bronze_create_array(0))};
        Rooted<Value> zero{Value::fromDouble(7.0)};
        Rooted<Value> one{Value::fromDouble(8.0)};
        a.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, zero);
        a.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, one);

        CHECK(hasOwn(a, "0"));
        CHECK(hasOwn(a, "length"));
        CHECK_FALSE(hasOwn(a, "2"));
        // 23.1.3.23's `push` is on the prototype the member table stands in for.
        CHECK_FALSE(hasOwn(a, "push"));
        // CreateDataProperty makes an index enumerable; ArrayCreate does not
        // make `length` one.
        CHECK(enumerableOwn(a, "0"));
        CHECK_FALSE(enumerableOwn(a, "length"));

        a.get().asObject<ArrayHeader>()->deleteElem(1);
        CHECK_FALSE(hasOwn(a, "1"));
        CHECK(a.get().asObject<ArrayHeader>()->length == 2);
    }

    SUBCASE("a typed array: 10.4.5's indices, and nothing that reads like one") {
        Rooted<Value> v{
            Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 2))};
        CHECK(hasOwn(v, "0"));
        CHECK_FALSE(hasOwn(v, "5"));
        CHECK(enumerableOwn(v, "0"));
        // 23.2.3.21 is an accessor on `%TypedArray%.prototype` and 23.2.6.2 is a
        // property of the constructor's prototype: neither is own here, however
        // much the property path's header-backed answer looks like one.
        CHECK_FALSE(hasOwn(v, "length"));
        CHECK_FALSE(hasOwn(v, "byteLength"));
        CHECK_FALSE(hasOwn(v, "BYTES_PER_ELEMENT"));
    }

    SUBCASE("a Map, a Set, an ArrayBuffer and a DataView have no own property") {
        Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
        CHECK_FALSE(hasOwn(m, "size"));
        CHECK_FALSE(hasOwn(m, "get"));
        Rooted<Value> s{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
        CHECK_FALSE(hasOwn(s, "size"));
        Rooted<Value> buf{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
        CHECK_FALSE(hasOwn(buf, "byteLength"));
        Rooted<Value> view{Value::fromObject(DataViewHeader::create(rtHeap(), buf, 0, 8))};
        CHECK_FALSE(hasOwn(view, "byteOffset"));
    }

    SUBCASE("a RegExp: 22.2.3.1's lastIndex and nothing else") {
        Rooted<Value> re{aRegExp()};
        CHECK(hasOwn(re, "lastIndex"));
        CHECK_FALSE(enumerableOwn(re, "lastIndex"));
        // 22.2.6.10 and 22.2.6.5 are accessors on the prototype.
        CHECK_FALSE(hasOwn(re, "source"));
        CHECK_FALSE(hasOwn(re, "global"));
        CHECK_FALSE(hasOwn(re, "exec"));
    }

    // Step 2's ToObject would box. A String exotic object's own keys are
    // 10.4.3.4's `length` and 10.4.3.5's one property per code unit; the other
    // three wrappers have none at all, so the answer is false whatever the key.
    // That stays true of a NUMBER now that its box is buildable: `toFixed` is a
    // member of `Number.prototype`, which is inherited and not own.
    SUBCASE("a primitive, whose box bronze does not build and does not need") {
        Rooted<Value> s{rtMakeString("ab")};
        CHECK(hasOwn(s, "0"));
        CHECK(hasOwn(s, "length"));
        CHECK_FALSE(hasOwn(s, "2"));
        CHECK(enumerableOwn(s, "0"));
        CHECK_FALSE(enumerableOwn(s, "length"));

        Rooted<Value> n{Value::fromDouble(5.0)};
        CHECK_FALSE(hasOwn(n, "toFixed"));
        Rooted<Value> b{Value::fromBool(true)};
        CHECK_FALSE(hasOwn(b, "valueOf"));
        Rooted<Value> sym{rtMakeSymbol(Value::fromUndefined())};
        CHECK_FALSE(hasOwn(sym, "description"));
    }
}

// 20.1.3.3, whose walk is the one place bronze's missing intrinsics could have
// stopped it. Above a shapeless object the chain is `<Kind>.prototype` and then
// `Object.prototype`, and bronze never hands the first of those to a program —
// so it cannot be the receiver, and the whole remaining question is whether the
// receiver is `Object.prototype`. Before this the answer was a silent `false`.
TEST_CASE("isPrototypeOf walks past the prototypes bronze has not built") {
    ShadowStackFrame frame;

    Rooted<Value> proto{rtObjectPrototype()};
    Rooted<Value> plain{Value(bronze_create_object())};
    Rooted<Value> a{Value(bronze_create_array(0))};
    Rooted<Value> f{rtNativeFunction(nothing, 0)};
    Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};

    CHECK(invoke(proto, "isPrototypeOf", plain.get()).asBool());
    CHECK(invoke(proto, "isPrototypeOf", a.get()).asBool());
    CHECK(invoke(proto, "isPrototypeOf", f.get()).asBool());
    CHECK(invoke(proto, "isPrototypeOf", m.get()).asBool());

    // Nothing is its own prototype: the walk starts one link up.
    CHECK_FALSE(invoke(proto, "isPrototypeOf", proto.get()).asBool());
    // Step 1: a primitive argument is false without a walk at all.
    CHECK_FALSE(invoke(proto, "isPrototypeOf", Value::fromDouble(5.0)).asBool());
    // A receiver that is not `Object.prototype` is not on a shapeless object's
    // chain, because the only other link there is one bronze never hands out.
    CHECK_FALSE(invoke(plain, "isPrototypeOf", a.get()).asBool());
    CHECK_FALSE(invoke(f, "isPrototypeOf", f.get()).asBool());
}

// The two kinds that joined with the weak collections: the chain reaches
// `Object.prototype` from both, the members found are the SAME function
// objects a plain object finds, and the own-property switch answers rather
// than crashes — a WeakMap keeps everything in internal slots, so it has no
// own property of any kind (24.3.3).
TEST_CASE("a WeakMap and a WeakSet reach Object.prototype") {
    ShadowStackFrame frame;

    Rooted<Value> hasOwnProperty{protoMember("hasOwnProperty")};
    Rooted<Value> valueOf{protoMember("valueOf")};
    REQUIRE(isFunction(hasOwnProperty.get()));

    Rooted<Value> wm{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakMapFlags))};
    Rooted<Value> ws{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakSetFlags))};

    CHECK(member(wm, "hasOwnProperty").rawBits() == hasOwnProperty.get().rawBits());
    CHECK(member(ws, "hasOwnProperty").rawBits() == hasOwnProperty.get().rawBits());
    CHECK(member(wm, "valueOf").rawBits() == valueOf.get().rawBits());
    CHECK(member(ws, "valueOf").rawBits() == valueOf.get().rawBits());

    // No own properties: the methods a read answers come from the stand-in
    // table, not from the object, so `hasOwnProperty` says no.
    CHECK_FALSE(hasOwn(wm, "get"));
    CHECK_FALSE(hasOwn(wm, "has"));
    CHECK_FALSE(hasOwn(ws, "add"));
    CHECK_FALSE(enumerableOwn(wm, "get"));
}

// The two kinds that joined with the weak REFERENCES, checked for the same
// three things: the chain reaches `Object.prototype`, the members found there
// are the same function objects a plain object finds, and the own-property
// switch answers rather than crashing. A WeakRef has no own property at all
// (26.1.3 puts `deref` on a prototype), and its one payload word is the
// untraced target — the word the own-property switch would have read as a
// `Shape*` without an arm of its own.
TEST_CASE("a WeakRef and a FinalizationRegistry reach Object.prototype") {
    ShadowStackFrame frame;

    Rooted<Value> hasOwnProperty{protoMember("hasOwnProperty")};
    Rooted<Value> valueOf{protoMember("valueOf")};
    REQUIRE(isFunction(hasOwnProperty.get()));

    Rooted<Value> target{Value(bronze_create_object())};
    Rooted<Value> wr{rtMakeWeakRef(target)};
    Rooted<Value> callback{protoMember("valueOf")};
    Rooted<Value> reg{rtMakeFinalizationRegistry(callback)};

    CHECK(member(wr, "hasOwnProperty").rawBits() == hasOwnProperty.get().rawBits());
    CHECK(member(reg, "hasOwnProperty").rawBits() == hasOwnProperty.get().rawBits());
    CHECK(member(wr, "valueOf").rawBits() == valueOf.get().rawBits());
    CHECK(member(reg, "valueOf").rawBits() == valueOf.get().rawBits());

    CHECK_FALSE(hasOwn(wr, "deref"));
    CHECK_FALSE(hasOwn(reg, "register"));
    CHECK_FALSE(enumerableOwn(wr, "deref"));
}
