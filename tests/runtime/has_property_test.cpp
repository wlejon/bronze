// `in` against every heap kind, asked below the compiler — which is the level
// the operator's bug lived at.
//
// `bronze_has_property` used to be an if-chain with a fall-through: a receiver
// kind with no arm was cast to `ObjectHeader*` and its first payload word read
// as a `Shape*`. For a Map that word is the entries table, for a RegExp the
// source string, for a module namespace the export count — so the answer was
// not wrong, it was a dereference of an integer, and `'size' in new Map()`
// killed the process with no diagnostic at all.
//
// What is pinned here is therefore not a list of answers — `cases/
// in_operator_receivers` pins those, and pins them as a program sees them.
// It is the EXHAUSTIVENESS of the dispatch: that one receiver of every kind a
// program can hold is answered rather than crashed, for a string key and for a
// symbol key, and that the registry cannot grow a kind without this file
// failing to compile.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

// The tripwire, mirrored from rt_operator.cpp. `flags` is a `uint16_t` and
// HeapKind is an unnamed enum, so no compiler warning can prove either switch
// there is total — and a runtime `default:` that hard-errors only tells you
// about the kind you happened to run. Pinning the registry's SIZE in both
// places is what makes adding a kind a build failure at the dispatch AND at the
// test that covers it, rather than a segfault a year later.
static_assert(HeapKind::Count == 15,
              "a HeapKind was added or removed: give `in` an arm for it in rt_operator.cpp, "
              "and give it a receiver in `everyKind` below — a kind with no arm is exactly "
              "what used to read its payload's first word as a Shape*");

namespace {

// Whether `key in receiver`. Both operands are already Values, so this is the
// operator with nothing between it and the runtime.
bool has(Value key, Value receiver) {
    return bronze_has_property(key.rawBits(), receiver.rawBits());
}

bool hasName(const char* key, Rooted<Value>& receiver) {
    Rooted<Value> keyStr{rtMakeString(key)};
    return has(keyStr.get(), receiver.get());
}

uint64_t nothing(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

// A namespace exporting one name, built the way the linker builds one: an
// object of getters, converted. `namespace_test.cpp` proves the conversion; all
// this needs is a namespace that really is one.
Value namespaceExporting(const char* name) {
    Rooted<Value> obj{Value(bronze_create_object())};
    Rooted<Value> getter{rtNativeFunction(nothing, 0)};
    Rooted<Value> absent{Value::fromUndefined()};
    Rooted<Value> key{rtMakeString(name)};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), obj, key, getter, absent,
                                 /*enumerable=*/true);
    return Value(bronze_module_namespace(obj.get().rawBits()));
}

}  // namespace

// The ten kinds a JS program can be holding. The two it cannot — an iteration
// record and an environment record — are refused by name in the dispatch, which
// is a `fatal` and so is not callable from a test that intends to return; that
// they have arms at all is what the static_assert above protects.
TEST_CASE("`in` answers every heap kind a program can hold, for a string key") {
    ShadowStackFrame frame;

    SUBCASE("a plain object") {
        Rooted<Value> o{Value(bronze_create_object())};
        Rooted<Value> key{rtMakeString("k")};
        Rooted<Value> val{Value::fromDouble(1.0)};
        o.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
        CHECK(hasName("k", o));
        CHECK_FALSE(hasName("missing", o));
    }

    SUBCASE("an array") {
        Rooted<Value> a{Value(bronze_create_array(0))};
        Rooted<Value> zero{Value::fromDouble(7.0)};
        a.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, zero);
        CHECK(hasName("0", a));
        CHECK(hasName("length", a));
        CHECK_FALSE(hasName("1", a));
        // `Array.prototype`'s members, which an array answers beside the value
        // rather than off an object on its chain — so the member TABLE stands
        // in for that object here, exactly as the typed array's does above.
        // Without it `'push' in a` was false while `a.push` was a function.
        CHECK(hasName("push", a));
        CHECK(hasName("constructor", a));
        // A member 23.1.3 defines that bronze has not built: the property
        // exists and only its value is missing, so `in` says true where a READ
        // is a named hard error.
        CHECK(hasName("flat", a));
        // And the chain does not stop at that table: `Object.prototype` is one
        // link further up.
        CHECK(hasName("hasOwnProperty", a));
        CHECK_FALSE(hasName("missing", a));
        // A NAMED own property, which a program can write on an array like any
        // other object — asked of the same storage the read answers from.
        Rooted<Value> key{rtMakeString("tag")};
        Rooted<Value> val{Value::fromDouble(1.0)};
        rtArrayNamedSet(a, key, val);
        CHECK(hasName("tag", a));
    }

    SUBCASE("a function") {
        Rooted<Value> f{rtNativeFunction(nothing, 0)};
        CHECK(hasName("prototype", f));
        CHECK_FALSE(hasName("missing", f));
    }

    SUBCASE("a typed array") {
        Rooted<Value> v{
            Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 2))};
        CHECK(hasName("0", v));
        CHECK_FALSE(hasName("2", v));
        CHECK(hasName("byteLength", v));
        CHECK(hasName("map", v));
        CHECK_FALSE(hasName("missing", v));
    }

    SUBCASE("an ArrayBuffer") {
        Rooted<Value> b{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
        CHECK(hasName("byteLength", b));
        CHECK(hasName("constructor", b));
        CHECK_FALSE(hasName("missing", b));
    }

    SUBCASE("a DataView") {
        Rooted<Value> buf{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
        Rooted<Value> v{Value::fromObject(DataViewHeader::create(rtHeap(), buf, 0, 8))};
        CHECK(hasName("byteOffset", v));
        CHECK(hasName("getInt8", v));
        CHECK_FALSE(hasName("missing", v));
    }

    // The two that used to be the crash: a MapHeader's first payload word is a
    // `Value` holding the entry table, and the old tail read it as a `Shape*`
    // and followed it.
    SUBCASE("a Map") {
        Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
        CHECK(hasName("size", m));
        CHECK(hasName("get", m));
        CHECK(hasName("entries", m));
        CHECK_FALSE(hasName("add", m));
        CHECK_FALSE(hasName("missing", m));
    }

    SUBCASE("a Set") {
        Rooted<Value> s{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
        CHECK(hasName("size", s));
        CHECK(hasName("add", s));
        // 24.2.3 is not 24.1.3 with a different receiver: `get` and `set` are a
        // Map's members and a Set has neither.
        CHECK_FALSE(hasName("get", s));
        CHECK_FALSE(hasName("missing", s));
    }

    SUBCASE("a RegExp") {
        Rooted<Value> src{rtMakeString("a")};
        Rooted<Value> re{rtRegExpFromParts(src, "g")};
        CHECK(hasName("source", re));
        CHECK(hasName("lastIndex", re));
        CHECK(hasName("sticky", re));
        CHECK(hasName("exec", re));
        CHECK_FALSE(hasName("missing", re));
    }

    SUBCASE("a module namespace") {
        Rooted<Value> ns{namespaceExporting("exported")};
        CHECK(hasName("exported", ns));
        // 10.4.6.1 fixes [[Prototype]] at null, so an export name is the only
        // thing that can be true — there is no chain for anything else to be
        // found on.
        CHECK_FALSE(hasName("missing", ns));
        CHECK_FALSE(hasName("toString", ns));
    }

    SUBCASE("a proxy") {
        Rooted<Value> target{Value(bronze_create_object())};
        Rooted<Value> key{rtMakeString("k")};
        Rooted<Value> val{Value::fromDouble(1.0)};
        target.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
        Rooted<Value> handler{Value(bronze_create_object())};
        const uint64_t ctorArgs[2] = {target.get().rawBits(), handler.get().rawBits()};
        Rooted<Value> proxy{Value(rtProxyConstructor(0, 0, 2, ctorArgs))};
        // No `has` trap (10.5.7 step 7): the target's whole answer — its own
        // key, its chain past the member tables, and its absences.
        CHECK(hasName("k", proxy));
        CHECK(hasName("hasOwnProperty", proxy));
        CHECK_FALSE(hasName("missing", proxy));

        // With a `has` trap, the handler's answer replaces the target's.
        Rooted<Value> trapKey{rtMakeString("has")};
        Rooted<Value> trap{rtNativeFunction(
            [](uint64_t, uint64_t, uint32_t, const uint64_t*) -> uint64_t {
                return Value::fromBool(true).rawBits();
            },
            2)};
        handler.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), trapKey, trap);
        const uint64_t trapArgs[2] = {target.get().rawBits(), handler.get().rawBits()};
        Rooted<Value> trapped{Value(rtProxyConstructor(0, 0, 2, trapArgs))};
        CHECK(hasName("missing", trapped));
    }
}

// The step past the member table, which is the same step the READ path takes
// and had to take at the same time: `'hasOwnProperty' in f` answered false
// while `f.hasOwnProperty` answered `undefined`, and both were the search
// stopping at a table that stands in for a prototype rather than IS the end of
// a chain. Every receiver above inherits from `Object.prototype` (20.2.3,
// 23.1.3, 24.1.3, 24.2.3, 22.2.6, 23.2.3, 25.1.6, 25.3.4), so 20.1.3's six
// members are true on all of them.
//
// The module namespace is the one exception and belongs in this test rather
// than beside it: 10.4.6.1 fixes its [[Prototype]] at null, so its exports
// really are the end of its chain, and a step it did NOT gain is what makes the
// step the others gained a chain walk rather than a blanket answer.
TEST_CASE("`in` does not stop at the member table a shapeless receiver answers from") {
    ShadowStackFrame frame;

    Rooted<Value> f{rtNativeFunction(nothing, 0)};
    Rooted<Value> a{Value(bronze_create_array(0))};
    Rooted<Value> v{Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 1))};
    Rooted<Value> buf{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
    Rooted<Value> view{Value::fromObject(DataViewHeader::create(rtHeap(), buf, 0, 8))};
    Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
    Rooted<Value> s{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
    Rooted<Value> src{rtMakeString("a")};
    Rooted<Value> re{rtRegExpFromParts(src, "g")};

    // These four and not the other two of 20.1.3, because the step is a WALK
    // and a nearer prototype gets there first: `Array.prototype`,
    // `%TypedArray%.prototype` and `Function.prototype` each define `toString`,
    // and bronze has built none of the three — so `'toString' in a` is that
    // prototype's named refusal rather than this object's `true`, which is the
    // shadowing rule working and not a gap in it.
    for (const char* name :
         {"hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "valueOf"}) {
        CHECK(hasName(name, f));
        CHECK(hasName(name, a));
        CHECK(hasName(name, v));
        CHECK(hasName(name, buf));
        CHECK(hasName(name, view));
        CHECK(hasName(name, m));
        CHECK(hasName(name, s));
        CHECK(hasName(name, re));
    }

    // A name that is on no prototype in the chain is still false, which is what
    // keeps the step a walk rather than a yes.
    CHECK_FALSE(hasName("missing", m));
    CHECK_FALSE(hasName("missing", re));

    // The other side of the shadowing rule: 24.1.3 gives `Map.prototype` no
    // `toString`, so a Map reaches 20.1.3.6's, and 22.2.6.13 gives
    // `RegExp.prototype` one bronze HAS built, so a RegExp stops there.
    CHECK(hasName("toString", m));
    CHECK(hasName("toString", re));

    // An index OUTSIDE a typed array's length is absent and not inherited
    // (10.4.5.2), so it must not be carried up to this step and answered there.
    CHECK_FALSE(hasName("1", v));

    Rooted<Value> ns{namespaceExporting("exported")};
    CHECK_FALSE(hasName("hasOwnProperty", ns));
    CHECK_FALSE(hasName("valueOf", ns));
}

// A symbol key takes its own dispatch, because ToString of one is a TypeError
// and so the key can never be converted. It must be exhaustive for the same
// reason the named one must, and it must AGREE with the property read path
// about the kinds that have no shape: it used to report `false` for
// `Symbol.iterator in m` while `m[Symbol.iterator]` handed back
// `Map.prototype.entries`.
TEST_CASE("`in` answers every heap kind a program can hold, for a symbol key") {
    ShadowStackFrame frame;

    const Value iterator = Value::fromSymbol(rtSymbolIterator());
    Rooted<Value> other{rtMakeSymbol(Value::fromUndefined())};

    SUBCASE("a receiver with a shape does the ordinary own-key walk") {
        Rooted<Value> o{Value(bronze_create_object())};
        Rooted<Value> key{rtMakeSymbol(Value::fromUndefined())};
        Rooted<Value> val{Value::fromDouble(1.0)};
        o.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
        CHECK(has(key.get(), o.get()));
        CHECK_FALSE(has(other.get(), o.get()));
        CHECK_FALSE(has(iterator, o.get()));

        Rooted<Value> f{rtNativeFunction(nothing, 0)};
        CHECK_FALSE(has(other.get(), f.get()));
    }

    SUBCASE("@@iterator is on the four iterable prototypes and no others") {
        Rooted<Value> a{Value(bronze_create_array(0))};
        Rooted<Value> v{
            Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 1))};
        Rooted<Value> m{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};
        Rooted<Value> s{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags))};
        // 23.1.3.34, 23.2.3.34, 24.1.3.12, 24.2.3.11.
        CHECK(has(iterator, a.get()));
        CHECK(has(iterator, v.get()));
        CHECK(has(iterator, m.get()));
        CHECK(has(iterator, s.get()));
        // A symbol the program made is never on one of these: none of them has
        // a shape for it to have been added to.
        CHECK_FALSE(has(other.get(), m.get()));

        Rooted<Value> buf{Value::fromObject(ArrayBufferHeader::create(rtHeap(), 8))};
        Rooted<Value> view{Value::fromObject(DataViewHeader::create(rtHeap(), buf, 0, 8))};
        Rooted<Value> src{rtMakeString("a")};
        Rooted<Value> re{rtRegExpFromParts(src, "")};
        Rooted<Value> ns{namespaceExporting("exported")};
        CHECK_FALSE(has(iterator, buf.get()));
        CHECK_FALSE(has(iterator, view.get()));
        CHECK_FALSE(has(iterator, re.get()));
        // 10.4.6.5 sends a symbol to OrdinaryGetOwnProperty, and a namespace
        // has no own symbol-keyed property to find there.
        CHECK_FALSE(has(iterator, ns.get()));
    }
}

// The two kinds that joined with the weak collections, covered the way the
// TEST_CASEs above cover the other ten: an arm that answers rather than
// crashes, agreement with the read path's tables, and the chain past them.
TEST_CASE("`in` answers a WeakMap and a WeakSet") {
    ShadowStackFrame frame;

    Rooted<Value> wm{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakMapFlags))};
    Rooted<Value> ws{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kWeakSetFlags))};

    CHECK(hasName("get", wm));
    CHECK(hasName("set", wm));
    CHECK(hasName("has", wm));
    CHECK(hasName("delete", wm));
    // 24.3.3 defines no `size` accessor and no iteration members — the absence
    // is the language's, so `in` must answer false rather than borrowing a
    // Map's yes.
    CHECK_FALSE(hasName("size", wm));
    CHECK_FALSE(hasName("forEach", wm));
    CHECK_FALSE(hasName("keys", wm));
    CHECK_FALSE(hasName("missing", wm));

    CHECK(hasName("add", ws));
    CHECK(hasName("has", ws));
    CHECK(hasName("delete", ws));
    CHECK_FALSE(hasName("get", ws));
    CHECK_FALSE(hasName("missing", ws));

    // The chain continues past the member table, as it does for every other
    // shapeless receiver.
    CHECK(hasName("hasOwnProperty", wm));
    CHECK(hasName("toString", ws));

    // @@toStringTag is on both prototypes (24.3.3.6, 24.4.3.5); @@iterator is
    // on neither — a WeakMap is not iterable, and that is half of the type.
    const Value tag = Value::fromSymbol(rtSymbolToStringTag());
    const Value iterator = Value::fromSymbol(rtSymbolIterator());
    CHECK(has(tag, wm.get()));
    CHECK(has(tag, ws.get()));
    CHECK_FALSE(has(iterator, wm.get()));
    CHECK_FALSE(has(iterator, ws.get()));
}

// 13.10.2 step 5 throws for a primitive right-hand side rather than answering
// false, which is what keeps `in` from being usable as a guard on an unknown
// value — and it must throw before the key is looked at, or a symbol key would
// reach a conversion that is itself a TypeError.
TEST_CASE("`in` on a non-object right-hand side is a TypeError") {
    ShadowStackFrame frame;

    Rooted<Value> key{rtMakeString("k")};
    CHECK_FALSE(has(key.get(), Value::fromDouble(1.0)));
    CHECK(rtExceptionPending());
    rtClearException();

    Rooted<Value> sym{rtMakeSymbol(Value::fromUndefined())};
    CHECK_FALSE(has(sym.get(), Value::fromUndefined()));
    CHECK(rtExceptionPending());
    rtClearException();
}
