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
static_assert(HeapKind::Count == 12,
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
