// Subclassing a native constructor, below the compiler.
//
// The oracle cases pin what a PROGRAM sees — that `new MyMap().set` works and
// that `v.map(f)` is a `Vec`. Four things they cannot reach are pinned here,
// because each is a decision the mechanism rests on rather than an observation:
//
//   - the DISPATCH BYTE surviving a recycled block. `bronze_construct` switches
//     on `FunctionHeader::native_base`, so residue in that byte makes an
//     ordinary closure allocate a Map — a wrong answer with no wrong-looking
//     line of JavaScript anywhere near it. `FunctionHeader::create` must write
//     it, and the only way to see that is to reuse memory a poisoned header
//     had.
//   - the FAST-PATH GUARD. Species dispatch is skipped for an array or a
//     collection with no property box, so "a plain one has no box" is the
//     entire performance argument and is asserted directly.
//   - the RECEIVER SCOPE's nesting. It is what tells `Map`'s body "fill the
//     object you were handed" from `Map.call(m)`, and a derived constructor
//     chain pushes one per `new` — so the inner one must win and the outer one
//     must come back.
//   - which HEAP KIND each base allocates, which is the one place the whole
//     feature could be wrong in a way that still runs.

#include <doctest/doctest.h>

#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

uint64_t inertBody(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

// `FunctionHeader::create` leaves the KIND to its caller — every runtime path
// that builds a function stamps it — so the helper does the same, or every
// probe below would answer about a plain object.
Value makeFunction() {
    FunctionHeader* fn = FunctionHeader::create(rtHeap(), inertBody, Value::fromUndefined(), 0);
    fn->header.flags = HeapKind::Function;
    return Value::fromObject(fn);
}

}  // namespace

TEST_CASE("FunctionHeader::create writes the native-base byte rather than inheriting residue") {
    ShadowStackFrame frame;

    // Poison a batch and let every one of them die immediately, so the blocks
    // are garbage the moment the loop turns over. Two cycles, because a
    // semispace collector hands the space back on the collection AFTER the one
    // that retired it — so a single pass would allocate the fresh batch out of
    // memory the poisoned one never occupied and prove nothing.
    for (int cycle = 0; cycle < 2; ++cycle) {
        for (int i = 0; i < 64; ++i) {
            Rooted<Value> fn{makeFunction()};
            fn.get().asObject<FunctionHeader>()->native_base = NativeBase::Map;
            CHECK(rtNativeBaseOf(fn.get()) == NativeBase::Map);
        }
        rtHeap().collect();
    }

    for (int i = 0; i < 64; ++i) {
        Rooted<Value> fn{makeFunction()};
        CHECK(fn.get().asObject<FunctionHeader>()->native_base == NativeBase::None);
        CHECK(rtNativeBaseOf(fn.get()) == NativeBase::None);
    }
}

TEST_CASE("the native base is inherited down an extends chain, not rediscovered") {
    ShadowStackFrame frame;

    Rooted<Value> base{makeFunction()};
    base.get().asObject<FunctionHeader>()->native_base = NativeBase::Set;
    Rooted<Value> derived{makeFunction()};
    Rooted<Value> deeper{makeFunction()};

    rtInheritNativeBase(derived, base);
    CHECK(rtNativeBaseOf(derived.get()) == NativeBase::Set);
    rtInheritNativeBase(deeper, derived);
    CHECK(rtNativeBaseOf(deeper.get()) == NativeBase::Set);

    // An ordinary base leaves an ordinary derived class: the byte is copied,
    // never accumulated.
    Rooted<Value> ordinary{makeFunction()};
    Rooted<Value> plainDerived{makeFunction()};
    plainDerived.get().asObject<FunctionHeader>()->native_base = NativeBase::Array;
    rtInheritNativeBase(plainDerived, ordinary);
    CHECK(rtNativeBaseOf(plainDerived.get()) == NativeBase::None);
}

TEST_CASE("the construct receiver scope nests, innermost first") {
    ShadowStackFrame frame;

    Rooted<Value> outer{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
    Rooted<Value> inner{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};

    CHECK_FALSE(rtIsNativeConstructReceiver(outer.get()));
    {
        NativeReceiverScope outerScope(outer.get());
        CHECK(rtIsNativeConstructReceiver(outer.get()));
        CHECK_FALSE(rtIsNativeConstructReceiver(inner.get()));
        {
            NativeReceiverScope innerScope(inner.get());
            CHECK(rtIsNativeConstructReceiver(inner.get()));
            CHECK_FALSE(rtIsNativeConstructReceiver(outer.get()));
        }
        CHECK(rtIsNativeConstructReceiver(outer.get()));
    }
    CHECK_FALSE(rtIsNativeConstructReceiver(outer.get()));
    // A plain call has no receiver at all, and undefined must never match.
    CHECK_FALSE(rtIsNativeConstructReceiver(Value::fromUndefined()));
}

TEST_CASE("an un-subclassed array or collection carries no property box") {
    ShadowStackFrame frame;

    Rooted<Value> arr{Value::fromObject(ArrayHeader::create(rtHeap()))};
    Rooted<Value> map{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
    Rooted<Value> set{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};

    // THE FAST-PATH GUARD. Every species and named-read probe starts here, so
    // an un-subclassed receiver costs one load and one tag test.
    CHECK_FALSE(rtExoticPropertyBox(arr.get()).isObject());
    CHECK_FALSE(rtExoticPropertyBox(map.get()).isObject());
    CHECK_FALSE(rtExoticPropertyBox(set.get()).isObject());
    CHECK(rtExoticSubclassPrototype(arr.get()).isUndefined());
    CHECK(rtExoticSubclassPrototype(map.get()).isUndefined());

    // A kind with no box at all answers the same way rather than reading a
    // field that is not there.
    Rooted<Value> fn{makeFunction()};
    CHECK(rtExoticPropertyBox(fn.get()).isUndefined());
    CHECK(rtExoticPropertyBox(Value::fromDouble(1)).isUndefined());
}

TEST_CASE("realized statics are the same objects the beside-the-value table answers") {
    ShadowStackFrame frame;

    // The invariant a second list would break. `Array.of` is answered off
    // `kCtors` for the intrinsic and off the property box for a subclass, and
    // the two must be ONE function object — otherwise `MyArr.of === Array.of`
    // is false and a program keying on function identity sees two members
    // where the language has one.
    Rooted<Value> arrayCtor{rtArrayConstructorObject()};
    rtRealizeNativeStatics(arrayCtor);
    Rooted<Value> props{arrayCtor.get().asObject<FunctionHeader>()->properties};
    REQUIRE(props.get().isObject());

    for (const char* name : {"of", "from", "isArray"}) {
        Value beside;
        REQUIRE(rtGlobalConstructorMember(arrayCtor.get(), name, beside));
        Rooted<Value> besideRoot{beside};
        Rooted<Value> key{rtMakeString(name)};
        PropertyInfo info;
        REQUIRE(props.get().asObject<ObjectHeader>()->shape != nullptr);
        REQUIRE(props.get().asObject<ObjectHeader>()->shape->lookupProperty(
            PropertyKey::forString(key.get().asString<StringHeader>()), info));
        // 23.1.2: an own property of an intrinsic, and not enumerable — so
        // `Object.keys(Array)` stays empty after the realization.
        CHECK_FALSE(info.enumerable);
        const Value stored = props.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
        CHECK(stored.rawBits() == besideRoot.get().rawBits());
    }

    // Idempotent: the link is made once per `extends`, but a class hierarchy
    // makes several and each one asks.
    rtRealizeNativeStatics(arrayCtor);
    rtRealizeNativeStatics(arrayCtor);
    Rooted<Value> ofKey{rtMakeString("of")};
    Value beside;
    REQUIRE(rtGlobalConstructorMember(arrayCtor.get(), "of", beside));
    CHECK(props.get().asObject<ObjectHeader>()->getProp(rtHeap(), ofKey).rawBits() ==
          beside.rawBits());

    // The Map family reads its own table through the same entry point.
    Rooted<Value> mapCtor{rtMapConstructor("Map")};
    rtRealizeNativeStatics(mapCtor);
    Rooted<Value> mapProps{mapCtor.get().asObject<FunctionHeader>()->properties};
    REQUIRE(mapProps.get().isObject());
    Rooted<Value> groupKey{rtMakeString("groupBy")};
    Value mapBeside;
    REQUIRE(rtMapStatic(mapCtor.get(), "groupBy", mapBeside));
    CHECK(mapProps.get().asObject<ObjectHeader>()->getProp(rtHeap(), groupKey).rawBits() ==
          mapBeside.rawBits());
}

TEST_CASE("each native base allocates its own heap kind, with NewTarget's prototype") {
    ShadowStackFrame frame;

    Rooted<Value> ctor{makeFunction()};
    rtEnsureFunctionPrototype(ctor);
    Rooted<Value> proto{ctor.get().asObject<FunctionHeader>()->prototype};
    REQUIRE(proto.get().isObject());

    Rooted<Value> arr{rtAllocateNativeBaseInstance(NativeBase::Array, ctor)};
    CHECK(arr.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array);
    Rooted<Value> map{rtAllocateNativeBaseInstance(NativeBase::Map, ctor)};
    CHECK(map.get().asObject<HeapObjectHeader>()->flags == MapHeader::kMapFlags);
    Rooted<Value> set{rtAllocateNativeBaseInstance(NativeBase::Set, ctor)};
    CHECK(set.get().asObject<HeapObjectHeader>()->flags == MapHeader::kSetFlags);

    // 10.1.14: the [[Prototype]] is the one NewTarget carries, and for these
    // kinds it lives on the property box rather than on the instance.
    CHECK(rtExoticSubclassPrototype(arr.get()).rawBits() == proto.get().rawBits());
    CHECK(rtExoticSubclassPrototype(map.get()).rawBits() == proto.get().rawBits());
    CHECK(rtExoticSubclassPrototype(set.get()).rawBits() == proto.get().rawBits());

    // Every instance of one class shares ONE box shape: a per-instance shape
    // would cost each of them its inline caches, which is the reason the box is
    // built through the memoized root shape instead of `setPrototype`.
    Rooted<Value> second{rtAllocateNativeBaseInstance(NativeBase::Map, ctor)};
    Shape* a = rtExoticPropertyBox(map.get()).asObject<ObjectHeader>()->shape;
    Shape* b = rtExoticPropertyBox(second.get()).asObject<ObjectHeader>()->shape;
    CHECK(a != nullptr);
    CHECK(a == b);
}
