// Where an object's integrity level is RECORDED, and what reads it back.
//
// The oracle cases pin what ECMA-262 fixes about `Object.freeze` and friends.
// What they cannot see is the decision underneath: an array and a function keep
// the level in the side object their named properties already live in, and
// nothing new is stored on the array or the function itself. That choice is
// what the tests below hold, because it is the one a later change could quietly
// undo — moving the bit somewhere the element write path does not read would
// bring back exactly the bug this file exists for, and every oracle case would
// still pass until someone wrote to a frozen array.
//
// The other half is the refusals. `rtArrayElementWriteRefusal` distinguishes
// three levels and two kinds of write, which is six answers reached from two
// helpers in rt_prop.cpp; here they are asked directly.

#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/map.h"
#include "runtime/typed_array.h"
#include "runtime/object.h"
#include "runtime/heap.h"
#include "runtime/property_key.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

Value freeze(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectFreeze(0, 0, 1, argv));
}
Value seal(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectSeal(0, 0, 1, argv));
}
Value preventExtensions(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectPreventExtensions(0, 0, 1, argv));
}
bool isFrozen(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectIsFrozen(0, 0, 1, argv)).asBool();
}
bool isSealed(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectIsSealed(0, 0, 1, argv)).asBool();
}
bool isExtensible(Value v) {
    const uint64_t argv[1] = {v.rawBits()};
    return Value(rtObjectIsExtensible(0, 0, 1, argv)).asBool();
}

Value makeArray(uint32_t count) {
    Rooted<Value> arr{Value(bronze_create_array(count))};
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> v{Value::fromDouble(static_cast<double>(i) + 1.0)};
        arr.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, v);
    }
    return arr.get();
}

// A function object with nothing special about it: the singleton table interns
// on the code pointer, so one per test body is one object. Distinct return values
// prevent MSVC identical code folding (/O2) from merging their addresses.
uint64_t dummyCode(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_UNDEFINED_BITS;
}
uint64_t otherCode(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return BRONZE_ABI_NULL_BITS;
}

struct FatalGuard {
    FatalGuard(FatalHandler handler) { setFatalHandler(handler); }
    ~FatalGuard() { setFatalHandler(nullptr); }
};

struct ClearCell {
    ~ClearCell() { bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS; }
};

}  // namespace

TEST_CASE("an array's integrity level lands on its side property object") {
    ShadowStackFrame frame;
    Rooted<Value> arr{makeArray(2)};

    // Nothing recorded, and nothing allocated to record it in: an array that is
    // never frozen pays for none of this.
    CHECK(arr.get().asObject<ArrayHeader>()->properties.isUndefined());
    CHECK(rtIntegrityTable(arr.get()) == nullptr);
    CHECK(rtIsExtensible(arr.get()));
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Open);

    freeze(arr.get());

    // The bit is in the side object's dictionary — not on the ArrayHeader, and
    // not in the heap object header's flags word, which is the HeapKind
    // registry and stays exactly what it was.
    REQUIRE(arr.get().asObject<ArrayHeader>()->properties.isObject());
    CHECK(arr.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array);
    Dictionary* table = rtIntegrityTable(arr.get());
    REQUIRE(table != nullptr);
    CHECK_FALSE(table->extensible);
    CHECK(table->level == IntegrityLevel::Frozen);
    // The elements are untouched by all of it; freezing is about attributes.
    CHECK(arr.get().asObject<ArrayHeader>()->length == 2);
    CHECK(arr.get().asObject<ArrayHeader>()->getElem(0).asNumber() == 1.0);
}

TEST_CASE("the level only ever rises") {
    ShadowStackFrame frame;
    Rooted<Value> arr{makeArray(1)};

    preventExtensions(arr.get());
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Open);
    CHECK_FALSE(rtIsExtensible(arr.get()));

    seal(arr.get());
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Sealed);

    freeze(arr.get());
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Frozen);

    // Sealing a frozen array must not unfreeze it: the three operations remove
    // capabilities and none of them restores one.
    seal(arr.get());
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Frozen);
    preventExtensions(arr.get());
    CHECK(rtIntegrityLevel(arr.get()) == IntegrityLevel::Frozen);
}

TEST_CASE("an element write is refused for the reason the level gives") {
    ShadowStackFrame frame;

    SUBCASE("an ordinary array refuses nothing") {
        Rooted<Value> arr{makeArray(2)};
        CHECK(rtArrayElementWriteRefusal(arr.get(), 0) == SetRefusal::None);
        CHECK(rtArrayElementWriteRefusal(arr.get(), 2) == SetRefusal::None);
        CHECK(rtArrayElementsConfigurable(arr.get()));
    }
    SUBCASE("preventExtensions stops the append and nothing else") {
        Rooted<Value> arr{makeArray(2)};
        preventExtensions(arr.get());
        CHECK(rtArrayElementWriteRefusal(arr.get(), 0) == SetRefusal::None);
        CHECK(rtArrayElementWriteRefusal(arr.get(), 2) == SetRefusal::NotExtensible);
        CHECK(rtArrayElementsConfigurable(arr.get()));
    }
    SUBCASE("seal leaves the write and takes the delete") {
        Rooted<Value> arr{makeArray(2)};
        seal(arr.get());
        CHECK(rtArrayElementWriteRefusal(arr.get(), 0) == SetRefusal::None);
        CHECK(rtArrayElementWriteRefusal(arr.get(), 2) == SetRefusal::NotExtensible);
        CHECK_FALSE(rtArrayElementsConfigurable(arr.get()));
    }
    SUBCASE("freeze takes the write too") {
        Rooted<Value> arr{makeArray(2)};
        freeze(arr.get());
        CHECK(rtArrayElementWriteRefusal(arr.get(), 0) == SetRefusal::NotWritable);
        CHECK(rtArrayElementWriteRefusal(arr.get(), 2) == SetRefusal::NotExtensible);
        CHECK_FALSE(rtArrayElementsConfigurable(arr.get()));
    }
    SUBCASE("a HOLE inside the length is a create, not a write") {
        // `delete a[0]` takes index 0 out of the own keys, so writing it back
        // needs [[Extensible]] rather than writability — which is the whole
        // reason the refusal asks `hasElem` and not `index < length`.
        Rooted<Value> arr{makeArray(2)};
        arr.get().asObject<ArrayHeader>()->deleteElem(0);
        preventExtensions(arr.get());
        CHECK(rtArrayElementWriteRefusal(arr.get(), 0) == SetRefusal::NotExtensible);
        CHECK(rtArrayElementWriteRefusal(arr.get(), 1) == SetRefusal::None);
    }
}

TEST_CASE("a function's level reaches its statics and its prototype slot") {
    ShadowStackFrame frame;
    Rooted<Value> fn{rtNativeFunction(dummyCode, 0)};

    CHECK(rtFunctionPrototypeWritable(fn.get()));
    CHECK(rtIsExtensible(fn.get()));

    // A static, written the way `C.k = v` writes one.
    Rooted<Value> key{rtMakeString("tag")};
    Rooted<Value> val{rtMakeString("t")};
    rtEnsureFunctionProperties(fn);
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);

    freeze(fn.get());

    CHECK_FALSE(rtFunctionPrototypeWritable(fn.get()));
    Dictionary* table = rtIntegrityTable(fn.get());
    REQUIRE(table != nullptr);
    CHECK_FALSE(table->extensible);
    REQUIRE(table->entries.size() == 1);
    CHECK_FALSE(table->entries[0].writable);
    CHECK_FALSE(table->entries[0].configurable);
    CHECK(isFrozen(fn.get()));
    CHECK(isSealed(fn.get()));
    CHECK_FALSE(isExtensible(fn.get()));
}

TEST_CASE("a sealed function keeps its prototype writable") {
    ShadowStackFrame frame;
    Rooted<Value> fn{rtNativeFunction(otherCode, 0)};
    seal(fn.get());
    CHECK(rtFunctionPrototypeWritable(fn.get()));
    // …which is exactly why 7.3.15 answers false for the frozen question: the
    // function still has a writable own property.
    CHECK(isSealed(fn.get()));
    CHECK_FALSE(isFrozen(fn.get()));
}

TEST_CASE("a receiver bronze cannot record a level for is refused by name") {
    ShadowStackFrame frame;
    Rooted<Value> map{Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags))};

    // The predicates still answer, and their answers are correct BECAUSE the
    // mutators refuse: nothing can have made this Map non-extensible.
    CHECK(isExtensible(map.get()));
    CHECK_FALSE(isFrozen(map.get()));
    CHECK_FALSE(isSealed(map.get()));

    {
        FatalGuard guard([](const char* msg) { throw std::runtime_error(msg); });
        CHECK_THROWS_WITH_AS(freeze(map.get()),
                             doctest::Contains("Object.freeze on a Map"), std::runtime_error);
        CHECK_THROWS_WITH_AS(seal(map.get()), doctest::Contains("Object.seal on a Map"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(preventExtensions(map.get()),
                             doctest::Contains("Object.preventExtensions on a Map"),
                             std::runtime_error);
    }
}

TEST_CASE("freezing a typed array with elements is the TypeError 10.4.5.3 gives") {
    ShadowStackFrame frame;
    ClearCell guard;

    Rooted<Value> view{Value::fromObject(TypedArrayHeader::create(rtHeap(), ElementKind::Uint8, 2))};
    CHECK(isExtensible(view.get()));

    freeze(view.get());
    CHECK(rtExceptionPending());
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

    seal(view.get());
    CHECK(rtExceptionPending());
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

    // The view is untouched, and still says so.
    CHECK(isExtensible(view.get()));
    CHECK_FALSE(isFrozen(view.get()));
}

TEST_CASE("the arguments object carries `callee`; an ordinary array does not") {
    ShadowStackFrame frame;

    const uint64_t argv[1] = {Value::fromDouble(1.0).rawBits()};
    Rooted<Value> args{Value(bronze_arguments_object(1, argv, BRONZE_ABI_UNDEFINED_BITS, /*strict=*/true))};

    // The property is what tells an arguments object apart from every other
    // array, which is the whole reason the answer is a property: `[].callee`
    // has to stay `undefined`, and nothing else could distinguish the two.
    REQUIRE(args.get().asObject<ArrayHeader>()->properties.isObject());
    // The key is built BEFORE the side object is dereferenced: rtMakeString
    // allocates, and a raw ObjectHeader* taken across it names where the object
    // used to be (BRONZE_GC_STRESS=1 finds this every time).
    Rooted<Value> key{rtMakeString("callee")};
    PropertyInfo info;
    ObjectHeader* props = args.get().asObject<ArrayHeader>()->properties.asObject<ObjectHeader>();
    REQUIRE(props->shape != nullptr);
    REQUIRE(props->shape->lookupProperty(PropertyKey::fromValue(key.get()), info));
    // An accessor pair, and non-enumerable — 10.2.11 step 6. Non-enumerable is
    // what keeps every walk over an arguments object unchanged, since all of
    // them ask for own ENUMERABLE keys.
    CHECK(info.accessor);
    CHECK_FALSE(info.enumerable);

    Rooted<Value> plain{makeArray(1)};
    CHECK(plain.get().asObject<ArrayHeader>()->properties.isUndefined());
}

TEST_CASE("reading arguments.callee in strict mode throws TypeError") {
    ShadowStackFrame frame;

    const uint64_t argv[1] = {Value::fromDouble(1.0).rawBits()};
    Rooted<Value> args{Value(bronze_arguments_object(1, argv, BRONZE_ABI_UNDEFINED_BITS, /*strict=*/true))};
    Rooted<Value> key{rtMakeString("callee")};
    ObjectHeader* props = args.get().asObject<ArrayHeader>()->properties.asObject<ObjectHeader>();

    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
    Value res = props->getProp(rtHeap(), key, nullptr, args.slot_ptr());
    (void)res;
    CHECK(rtExceptionPending());
    bronze_tls_block_addr()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
}

// The conversion into dictionary mode is the one moment every attribute is
// COPIED rather than looked up, and the language cannot see it happen. An
// oracle case can only reach it through `preventExtensions` or a `delete`, and
// `freeze` and `seal` hide a copy that drops attributes because they re-stamp
// every entry afterwards. So the copy is held here, at the level it is made.
TEST_CASE("dictionary conversion keeps every attribute the shape node carried") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    Rooted<Value> self{
        Value::fromObject(ObjectHeader::create(heap, arena, Shape::createRoot(arena)))};
    Rooted<Value> key{Value::fromString(StringHeader::createFromUTF8(heap, "x"))};
    Rooted<Value> val{Value::fromDouble(1.0)};
    // `Object.defineProperty(o, 'x', {writable: false, configurable: false})`
    // on a fresh object: a shape node carries all four attributes, so this
    // stays out of dictionary mode.
    self.get().asObject<ObjectHeader>()->setProp(heap, arena, key, val, /*ic=*/nullptr,
                                                 /*enumerable=*/true, /*defineOwn=*/true,
                                                 /*receiver=*/nullptr, /*refused=*/nullptr,
                                                 /*writable=*/false, /*configurable=*/false);
    const PropertyKey stored = PropertyKey::fromValue(key.get());
    REQUIRE_FALSE(self.get().asObject<ObjectHeader>()->shape->isDictionary());
    PropertyInfo before;
    REQUIRE(self.get().asObject<ObjectHeader>()->shape->lookupProperty(stored, before));
    CHECK_FALSE(before.writable);
    CHECK_FALSE(before.configurable);
    CHECK(before.enumerable);

    ObjectHeader::toDictionary(arena, self);

    Dictionary* d = self.get().asObject<ObjectHeader>()->shape->dict;
    REQUIRE(d != nullptr);
    REQUIRE(d->entries.size() == 1);
    CHECK_FALSE(d->entries[0].writable);
    CHECK_FALSE(d->entries[0].configurable);
    CHECK(d->entries[0].enumerable);
    CHECK_FALSE(d->entries[0].accessor);

    // And the refusal follows from the entry, which is the reason the copy
    // matters: a `true` written here is a property the language says cannot be
    // written that a write then lands in.
    Rooted<Value> other{Value::fromDouble(2.0)};
    SetRefusal refusal = SetRefusal::None;
    self.set(Value::fromObject(self.get().asObject<ObjectHeader>()->setProp(
        heap, arena, key, other, /*ic=*/nullptr, /*enumerable=*/true, /*defineOwn=*/false,
        /*receiver=*/nullptr, &refusal)));
    CHECK(refusal == SetRefusal::NotWritable);
    PropertyInfo after;
    REQUIRE(self.get().asObject<ObjectHeader>()->shape->lookupProperty(stored, after));
    CHECK(self.get().asObject<ObjectHeader>()->getSlot(after.slot).asNumber() == 1.0);
}

// 10.1.9.2 step 2 for the two spellings that have a receiver distinct from the
// object the walk starts at. The oracle cases pin what a program sees;
// what they cannot show is that ONE function decides it, which is the property
// that keeps `Reflect.set` and `super.k = v` from drifting apart.
TEST_CASE("a set with a distinct receiver lands on the receiver") {
    ShadowStackFrame frame;

    Rooted<Value> holder{Value(bronze_create_object())};
    Rooted<Value> receiver{Value(bronze_create_object())};
    Rooted<Value> key{rtMakeString("k")};
    Rooted<Value> val{Value::fromDouble(5.0)};

    CHECK(rtOrdinarySetWithReceiver(holder, key, val, receiver) == SetRefusal::None);
    // The holder is where the walk STARTED; nothing may have been stored there.
    PropertyInfo onHolder;
    const PropertyKey stored = PropertyKey::fromValue(key.get());
    CHECK_FALSE(holder.get().asObject<ObjectHeader>()->shape->lookupProperty(stored, onHolder));
    PropertyInfo onReceiver;
    REQUIRE(receiver.get().asObject<ObjectHeader>()->shape->lookupProperty(stored, onReceiver));
    CHECK(receiver.get().asObject<ObjectHeader>()->getSlot(onReceiver.slot).asNumber() == 5.0);
}

TEST_CASE("a set with a distinct receiver reports the RECEIVER's refusal") {
    ShadowStackFrame frame;

    Rooted<Value> holder{Value(bronze_create_object())};
    Rooted<Value> key{rtMakeString("k")};
    Rooted<Value> val{Value::fromDouble(5.0)};

    // An existing non-writable property of the receiver: step 2.a.
    Rooted<Value> frozenReceiver{Value(bronze_create_object())};
    Rooted<Value> one{Value::fromDouble(1.0)};
    frozenReceiver.set(Value::fromObject(frozenReceiver.get().asObject<ObjectHeader>()->setProp(
        rtHeap(), rtArena(), key, one)));
    freeze(frozenReceiver.get());
    CHECK(rtOrdinarySetWithReceiver(holder, key, val, frozenReceiver) ==
          SetRefusal::NotWritable);

    // No such property, and the receiver is closed: step 2.c.ii through
    // 10.1.6.3 step 2.b. A different test, and the holder is extensible in
    // both — which is exactly what a version that asked the HOLDER would miss.
    Rooted<Value> closedReceiver{Value(bronze_create_object())};
    const uint64_t argv[1] = {closedReceiver.get().rawBits()};
    rtObjectPreventExtensions(0, 0, 1, argv);
    CHECK(rtIsExtensible(holder.get()));
    CHECK(rtOrdinarySetWithReceiver(holder, key, val, closedReceiver) ==
          SetRefusal::NotExtensible);
    PropertyInfo onHolder;
    CHECK_FALSE(holder.get().asObject<ObjectHeader>()->shape->lookupProperty(
        PropertyKey::fromValue(key.get()), onHolder));
}
