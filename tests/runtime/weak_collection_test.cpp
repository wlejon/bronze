// The WeakMap/WeakSet surface below the compiler: the brand checks that keep a
// detached method from answering about a receiver it does not have, the
// CanBeHeldWeakly split between the loud writes and the quiet reads, and the
// strong-reference liveness the first implementation promises — an entry's key
// and value survive a forced collection for as long as the collection object
// does, which is the half of the weak contract a program can rely on today
// (builtin_weak_map.cpp's header owns the other half).
//
// Everything is driven through the property path (`bronze_elem_get`, then a
// call), because that is the only road a program has to these methods and the
// dispatch is part of what is being pinned.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// `receiver.<name>`, through the ordinary computed-read path.
Value member(Rooted<Value>& receiver, const char* name) {
    Rooted<Value> key{rtMakeString(name)};
    return Value(bronze_elem_get(receiver.get().rawBits(), key.get().rawBits()));
}

bool isFunction(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// `receiver.<name>(args...)`: the member is looked up rooted, the arguments
// arrive already rooted by the caller, and the block handed to `call` is
// filled from roots on the line before it — the pattern every builtin caller
// in this suite uses.
Value invoke2(Rooted<Value>& receiver, const char* name, Rooted<Value>& a, Rooted<Value>& b) {
    Rooted<Value> fn{member(receiver, name)};
    REQUIRE(isFunction(fn.get()));
    Value args[2] = {a.get(), b.get()};
    return fn.get().asObject<FunctionHeader>()->call(receiver.get(), 2, args);
}

Value invoke1(Rooted<Value>& receiver, const char* name, Rooted<Value>& a) {
    Rooted<Value> fn{member(receiver, name)};
    REQUIRE(isFunction(fn.get()));
    Value args[1] = {a.get()};
    return fn.get().asObject<FunctionHeader>()->call(receiver.get(), 1, args);
}

Value newWeakMap() {
    return Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakMapFlags));
}

Value newWeakSet() {
    return Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakSetFlags));
}

}  // namespace

TEST_CASE("WeakMap set/get/has/delete over object keys") {
    ShadowStackFrame frame;

    Rooted<Value> wm{newWeakMap()};
    Rooted<Value> key{Value(bronze_create_object())};
    Rooted<Value> val{Value::fromDouble(7.0)};

    // 24.3.3.5 answers the map itself, so `.set` chains.
    const auto setResult = invoke2(wm, "set", key, val);
    CHECK(setResult.rawBits() == wm.get().rawBits());
    CHECK(invoke1(wm, "has", key).asBool());
    CHECK(invoke1(wm, "get", key).asNumber() == 7.0);

    // A DIFFERENT object with the same shape is a different key: identity,
    // never content.
    Rooted<Value> other{Value(bronze_create_object())};
    CHECK_FALSE(invoke1(wm, "has", other).asBool());
    CHECK(invoke1(wm, "get", other).isUndefined());

    CHECK(invoke1(wm, "delete", key).asBool());
    CHECK_FALSE(invoke1(wm, "has", key).asBool());
    CHECK_FALSE(invoke1(wm, "delete", key).asBool());
}

TEST_CASE("CanBeHeldWeakly: writes throw, reads answer quietly") {
    ShadowStackFrame frame;

    Rooted<Value> wm{newWeakMap()};
    Rooted<Value> primitive{Value::fromDouble(1.0)};
    Rooted<Value> val{Value::fromDouble(2.0)};

    // 24.3.3.5 step 4: a primitive key is the TypeError the clause names.
    invoke2(wm, "set", primitive, val);
    CHECK(rtExceptionPending());
    rtClearException();

    // 24.3.3.4 step 4 and friends return before touching the table: `has`,
    // `get` and `delete` answer for a primitive rather than throwing.
    CHECK_FALSE(invoke1(wm, "has", primitive).asBool());
    CHECK(invoke1(wm, "get", primitive).isUndefined());
    CHECK_FALSE(invoke1(wm, "delete", primitive).asBool());
    CHECK_FALSE(rtExceptionPending());

    // An UNREGISTERED symbol can be held weakly (4.2.1); a registered one can
    // always be re-minted from its string and cannot.
    Rooted<Value> fresh{rtMakeSymbol(Value::fromUndefined())};
    invoke2(wm, "set", fresh, val);
    CHECK_FALSE(rtExceptionPending());
    CHECK(invoke1(wm, "has", fresh).asBool());

    Rooted<Value> regKey{rtMakeString("registered")};
    Rooted<Value> registered{rtSymbolFor(regKey)};
    invoke2(wm, "set", registered, val);
    CHECK(rtExceptionPending());
    rtClearException();

    // A WeakSet's write is the same split under its own message.
    Rooted<Value> ws{newWeakSet()};
    invoke1(ws, "add", primitive);
    CHECK(rtExceptionPending());
    rtClearException();
    Rooted<Value> obj{Value(bronze_create_object())};
    invoke1(ws, "add", obj);
    CHECK_FALSE(rtExceptionPending());
    CHECK(invoke1(ws, "has", obj).asBool());
}

TEST_CASE("a detached weak-collection method brand-checks its receiver") {
    ShadowStackFrame frame;

    Rooted<Value> wm{newWeakMap()};
    Rooted<Value> get{member(wm, "get")};
    REQUIRE(isFunction(get.get()));
    Rooted<Value> key{Value(bronze_create_object())};

    // A Map is NOT a WeakMap: same layout, different kind, and the brand is
    // the kind — a Map answering here would be a method reading a receiver it
    // was never defined over.
    Rooted<Value> plainMap{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
    Value args[1] = {key.get()};
    get.get().asObject<FunctionHeader>()->call(plainMap.get(), 1, args);
    CHECK(rtExceptionPending());
    rtClearException();

    // And so is a plain object, and nothing at all.
    Rooted<Value> plain{Value(bronze_create_object())};
    Value args2[1] = {key.get()};
    get.get().asObject<FunctionHeader>()->call(plain.get(), 1, args2);
    CHECK(rtExceptionPending());
    rtClearException();

    // A WeakSet is not a WeakMap either: `get` is a Map-side member and the
    // read path answers `undefined` for it on a WeakSet — the split
    // `rtWeakCollectionMethod` keys on the receiver for.
    Rooted<Value> ws{newWeakSet()};
    CHECK(member(ws, "get").isUndefined());
}

TEST_CASE("entries stay live across forced collections for as long as the map does") {
    ShadowStackFrame frame;

    Rooted<Value> wm{newWeakMap()};
    {
        // The key and value are reachable ONLY through the map after this
        // block: the strong-referencing implementation must keep both alive,
        // and — the harder half — keep the address-hashed index honest after
        // the collector has moved every key (map.h's epoch/anchor pair).
        Rooted<Value> key{Value(bronze_create_object())};
        Rooted<Value> marker{Value::fromString(
            StringHeader::createFromUTF8(rtHeap(), "weakmap_survivor"))};
        invoke2(wm, "set", key, marker);
        CHECK_FALSE(rtExceptionPending());

        for (int i = 0; i < 3; ++i) rtHeap().collect();

        CHECK(invoke1(wm, "has", key).asBool());
        Rooted<Value> read{invoke1(wm, "get", key)};
        REQUIRE(read.get().isString());
        CHECK(read.get().asString<StringHeader>()->getLength() == 16);
    }

    // With the key out of every root, the entry is unobservable — nothing can
    // name it again — so the collections here can only prove the table itself
    // stays walkable.
    for (int i = 0; i < 3; ++i) rtHeap().collect();
    Rooted<Value> fresh{Value(bronze_create_object())};
    CHECK_FALSE(invoke1(wm, "has", fresh).asBool());
}
