// What `Object.defineProperty` writes into STORAGE, which is the half the
// oracle cases cannot see.
//
// ECMA-262 10.1.6.3 is written over an abstract Property Descriptor, and bronze
// keeps a property in one of two places: a shape slot, whose four attributes
// every object reaching that shape shares, or a dictionary entry, which one
// object owns. A define that cannot be expressed as a shape transition falls
// through to the dictionary road, and the two roads have to agree — a bug in
// one of them shows up only for objects that happen to have taken it.
//
// Two things are asserted here that a printed descriptor cannot distinguish.
//
// The first is what a PARTIAL descriptor leaves alone. Step 5 sets the fields
// the descriptor HAS, so an absent `value` must leave the slot holding what it
// held. Writing the slot unconditionally on the dictionary road stored
// `undefined` over a live value while getting all four attributes right, so the
// only visible symptom was the data.
//
// The second is the RELATION step 4 compares with. A non-configurable property
// still accepts a redefinition to the value it already has, and "the same
// value" is 7.2.11 SameValue — two heap strings with the same characters are
// one value and not one pointer. A bit compare of the two slots gets every
// number right and every string wrong, which is why it survived so long.

#include <doctest/doctest.h>

#include <cmath>

#include "abi/bronze_abi.h"
#include "runtime/builtin_object.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

Value text(const char* s) { return Value::fromString(StringHeader::createFromUTF8(rtHeap(), s)); }

void put(Rooted<Value>& obj, const char* key, Value v) {
    Rooted<Value> val{v};
    Rooted<Value> k{text(key)};
    bronze_elem_set(obj.get().rawBits(), k.get().rawBits(), val.get().rawBits(),
                    /*strict=*/false);
}

Value object() { return Value(bronze_create_object()); }

// A descriptor object built one field at a time, which is the only way to build
// one that OMITS a field — the whole subject below.
Rooted<Value> descriptor() { return Rooted<Value>{object()}; }

bool define(Rooted<Value>& obj, const char* key, Rooted<Value>& desc) {
    Rooted<Value> k{text(key)};
    const uint64_t argv[3] = {obj.get().rawBits(), k.get().rawBits(), desc.get().rawBits()};
    return rtObjectDefineOwnProperty(3, argv, /*throwOnRefusal=*/false);
}

// The four attributes and the value, read from whichever storage the object is
// actually in. `found` is false for a name the object does not own.
struct Own {
    bool found = false;
    bool accessor = false;
    bool writable = false;
    bool enumerable = false;
    bool configurable = false;
    Value value = Value::fromUndefined();
    Value second = Value::fromUndefined();
};

Own own(Value objVal, const char* key) {
    Own out;
    Rooted<Value> k{text(key)};
    const PropertyKey name = rtInternPropertyKey(k.get());
    auto* obj = objVal.asObject<ObjectHeader>();
    PropertyInfo info;
    if (!obj->shape || !obj->shape->lookupProperty(name, info)) return out;
    out.found = true;
    out.accessor = info.accessor;
    out.writable = info.writable;
    out.enumerable = info.enumerable;
    out.configurable = info.configurable;
    out.value = obj->getSlot(info.slot);
    if (info.accessor) out.second = obj->getSlot(info.slot + 1);
    return out;
}

bool isDictionary(Value objVal) {
    auto* obj = objVal.asObject<ObjectHeader>();
    return obj->shape != nullptr && obj->shape->isDictionary();
}

Value freeze(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectFreeze(0, 0, 1, argv));
}

struct ClearCell {
    ~ClearCell() { bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS; }
};

// A callable for the one test that needs its getter to be real. Interned by
// code pointer, so every call here names the same function object.
uint64_t getterCode(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(11.0).rawBits();
}

}  // namespace

TEST_CASE("a partial define leaves the value in the slot it was in") {
    ShadowStackFrame frame;
    ClearCell clear;

    // The shape road first. `{writable: false}` cannot be a transition off the
    // shape this object is on — a Shape is shared, so demoting one object's
    // property has to leave it — and that fall-through is where the value was
    // being overwritten.
    Rooted<Value> o{object()};
    put(o, "p", Value::fromDouble(4.0));
    CHECK_FALSE(isDictionary(o.get()));

    Rooted<Value> d = descriptor();
    put(d, "writable", Value::fromBool(false));
    CHECK(define(o, "p", d));

    Own after = own(o.get(), "p");
    REQUIRE(after.found);
    CHECK(after.value.isNumber());
    CHECK(after.value.asNumber() == 4.0);
    CHECK_FALSE(after.writable);
    // The three attributes the descriptor did not name are the ones the
    // property had, not the false a new property would get.
    CHECK(after.enumerable);
    CHECK(after.configurable);

    // And again on an object already in dictionary mode, which is the other
    // way into the same code and must not answer differently.
    Rooted<Value> dictObj{object()};
    put(dictObj, "p", Value::fromDouble(7.0));
    put(dictObj, "q", Value::fromDouble(8.0));
    dictObj.get().asObject<ObjectHeader>()->deleteProperty(rtArena(),
                                                           rtInternPropertyKey(text("q")));
    REQUIRE(isDictionary(dictObj.get()));

    Rooted<Value> d2 = descriptor();
    put(d2, "enumerable", Value::fromBool(false));
    CHECK(define(dictObj, "p", d2));

    Own after2 = own(dictObj.get(), "p");
    REQUIRE(after2.found);
    CHECK(after2.value.isNumber());
    CHECK(after2.value.asNumber() == 7.0);
    CHECK_FALSE(after2.enumerable);
    CHECK(after2.writable);
    CHECK(after2.configurable);
}

TEST_CASE("a generic descriptor does not turn an accessor into a data property") {
    ShadowStackFrame frame;
    ClearCell clear;

    // 6.2.6.1: a descriptor naming neither a data field nor an accessor field
    // says nothing about the KIND, so 10.1.6.3 leaves the kind alone. Written
    // as "not an accessor descriptor, therefore a data one", this replaced the
    // getter with a slot holding `undefined`.
    Rooted<Value> o{object()};
    Rooted<Value> acc = descriptor();
    put(acc, "get", Value(bronze_create_object()));  // a placeholder half
    put(acc, "enumerable", Value::fromBool(true));
    put(acc, "configurable", Value::fromBool(true));
    // A plain object is not callable, so the decode must reject it — which is
    // the check 6.2.6.5 step 7.c makes and is worth one assertion of its own.
    CHECK_FALSE(define(o, "p", acc));
    CHECK(rtExceptionPending());
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

    // With a real function for the getter the accessor is defined, and then a
    // generic redefinition keeps it.
    Rooted<Value> fn{rtNativeFunction(getterCode, 0)};
    Rooted<Value> acc2 = descriptor();
    put(acc2, "get", fn.get());
    put(acc2, "enumerable", Value::fromBool(true));
    put(acc2, "configurable", Value::fromBool(true));
    REQUIRE(define(o, "p", acc2));
    REQUIRE(own(o.get(), "p").accessor);

    Rooted<Value> gen = descriptor();
    put(gen, "enumerable", Value::fromBool(false));
    CHECK(define(o, "p", gen));

    Own after = own(o.get(), "p");
    REQUIRE(after.found);
    CHECK(after.accessor);
    CHECK_FALSE(after.enumerable);
    CHECK(after.configurable);
    // The getter is the one that was there, not a fresh `undefined` half, and
    // the setter half it never had is still absent.
    CHECK(after.value.rawBits() == fn.get().rawBits());
    CHECK(after.second.isUndefined());
}

TEST_CASE("a frozen property accepts the value it already has, by SameValue") {
    ShadowStackFrame frame;
    ClearCell clear;

    // `freeze` records the level in a dictionary, so this is the dictionary
    // road through 10.1.6.3 step 4.
    Rooted<Value> o{object()};
    put(o, "n", Value::fromDouble(1.0));
    put(o, "s", text("abc"));
    put(o, "z", Value::fromDouble(0.0));
    put(o, "nan", Value::fromDouble(std::nan("")));
    freeze(o.get());
    REQUIRE(isDictionary(o.get()));

    auto tryValue = [&](const char* key, Value v) {
        Rooted<Value> d = descriptor();
        put(d, "value", v);
        const bool ok = define(o, key, d);
        bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
        return ok;
    };

    // The same number: step 4.e.ii's SameValue is true, so there is nothing to
    // refuse.
    CHECK(tryValue("n", Value::fromDouble(1.0)));
    CHECK_FALSE(tryValue("n", Value::fromDouble(2.0)));

    // A DISTINCT heap string with the same characters is the same value. This
    // is the assertion a bit compare of the two slots fails.
    Rooted<Value> other{text("abc")};
    CHECK(other.get().rawBits() != own(o.get(), "s").value.rawBits());
    CHECK(tryValue("s", other.get()));
    CHECK_FALSE(tryValue("s", text("abd")));

    // The two numeric corrections that separate SameValue from `===`: NaN
    // matches itself, and the zeroes do not match each other.
    CHECK(tryValue("nan", Value::fromDouble(std::nan(""))));
    CHECK(tryValue("z", Value::fromDouble(0.0)));
    CHECK_FALSE(tryValue("z", Value::fromDouble(-0.0)));

    // Nothing above stored anything: a define that is allowed because it
    // changes nothing must also change nothing.
    Own n = own(o.get(), "n");
    CHECK(n.value.asNumber() == 1.0);
    CHECK_FALSE(n.writable);
    CHECK_FALSE(n.configurable);
}

TEST_CASE("the shape road compares a frozen value by SameValue too") {
    ShadowStackFrame frame;
    ClearCell clear;

    // A property defined non-writable and non-configurable straight onto a
    // shape never reaches a dictionary, so step 4 is decided by the branch
    // above the fall-through — which held its own copy of the comparison.
    Rooted<Value> o{object()};
    Rooted<Value> d = descriptor();
    put(d, "value", text("abc"));
    put(d, "writable", Value::fromBool(false));
    put(d, "enumerable", Value::fromBool(true));
    put(d, "configurable", Value::fromBool(false));
    REQUIRE(define(o, "p", d));
    REQUIRE_FALSE(isDictionary(o.get()));

    Rooted<Value> same = descriptor();
    put(same, "value", text("abc"));
    CHECK(define(o, "p", same));
    CHECK_FALSE(isDictionary(o.get()));

    Rooted<Value> differs = descriptor();
    put(differs, "value", text("abd"));
    CHECK_FALSE(define(o, "p", differs));
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

    // A refused define is not a demotion: the object is still on its shape and
    // still holds what it held.
    CHECK_FALSE(isDictionary(o.get()));
    Own after = own(o.get(), "p");
    REQUIRE(after.found);
    CHECK(after.value.isString());
    CHECK(after.value.asString<StringHeader>()->equals(*text("abc").asString<StringHeader>()));
}

TEST_CASE("a refusal is a value, and only Object.defineProperty raises it") {
    ShadowStackFrame frame;
    ClearCell clear;

    // 28.1.3 returns the boolean [[DefineOwnProperty]] answered. The refusal
    // must not leave an exception pending, or `Reflect.defineProperty` would
    // report false and throw at the same time.
    Rooted<Value> o{object()};
    put(o, "p", Value::fromDouble(1.0));
    freeze(o.get());

    Rooted<Value> d = descriptor();
    put(d, "value", Value::fromDouble(2.0));
    CHECK_FALSE(define(o, "p", d));
    CHECK_FALSE(rtExceptionPending());

    // The same refusal through the throwing entry point.
    Rooted<Value> k{text("p")};
    const uint64_t argv[3] = {o.get().rawBits(), k.get().rawBits(), d.get().rawBits()};
    rtObjectDefineProperty(0, 0, 3, argv);
    CHECK(rtExceptionPending());
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

    CHECK(own(o.get(), "p").value.asNumber() == 1.0);
}
