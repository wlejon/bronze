// The primitive wrapper objects, and the three intrinsic prototypes they and
// their primitives share: `String.prototype` (22.1.3), `Boolean.prototype`
// (20.3.3), `Number.prototype` (21.1.3), and the exotic objects that carry each
// wrapped primitive — the String exotic object (10.4.3), the Boolean object and
// the Number object.
//
// These are REAL objects on the real chain, in the sense builtin_object.cpp's
// `Object.prototype` is one: a program can hold `String.prototype`, compare it,
// pass `String.prototype.indexOf` to `.call`, and reach it from a primitive
// through the ordinary prototype walk. What a string's members were before is
// what an array's still are — a table consulted BESIDE the value by the
// property path, with no holder anything can reach — and that is exactly why
// `"abc"[0]` could not work: an index that misses has to FALL THROUGH to an
// ordinary lookup, and there was no ordinary lookup to fall through to.
//
// Every member here is defined NON-ENUMERABLE, per the first line of 22.1.3 and
// 20.3.3. That is what made it safe to slide these under a suite of pinned
// expectations: `for-in` walks the prototype chain, so one enumerable member
// would have appeared in every for-in over every string in every program.
//
// The index properties are the deliberate exception and the one place this
// differs from `Object.prototype`: 10.4.3.4 StringCreate makes them ENUMERABLE,
// and non-writable and non-configurable with it. That is a fact about the
// wrapped characters rather than about a member table, so they are answered
// from the [[StringData]] slot on the property path and never stored.

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The one internal slot a wrapper carries: [[StringData]], [[BooleanData]] or
// [[NumberData]] (ECMA-262 6.1.7.2). It is a real field on the object rather
// than a property under a reserved name, so it is invisible to `Object.keys`,
// to `for-in`, to `getOwnPropertyNames` AND to `getOwnPropertySymbols`.
namespace WrapperSlot {
enum : uint32_t { Data, kCount };
}

// The BRAND, and it is the slot count paired with the slot's TYPE rather than
// the (prototype, count) pair an iterator object uses. The reason is that all
// three prototypes are themselves wrappers — 22.1.3 makes `String.prototype` a
// String exotic object with [[StringData]] "", and 21.1.3 makes
// `Number.prototype` a Number object with [[NumberData]] +0𝔽 — so their own
// prototype is `Object.prototype` and a prototype test would exclude the very
// objects the test most needs to include. The slot's type separates them, and
// it can do that job here because a wrapper's slot holds a primitive of a known
// type where an iterator's holds whatever it is iterating.
//
// An object forged with `Object.create(String.prototype)` was not allocated
// with the slot, answers a count of 0, and is correctly not a wrapper: reading
// the slot off it would read past its end.
bool wrapperData(Value v, Value& out) {
    if (!v.isObject()) return false;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return false;
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (obj->internalSlotCount() != WrapperSlot::kCount) return false;
    out = obj->internalSlot(WrapperSlot::Data);
    return true;
}

// ---- Boolean.prototype (20.3.3) --------------------------------------------

// 20.3.3.3 thisBooleanValue, which is the whole of `Boolean.prototype`: both
// members are it plus a formatting step.
uint64_t booleanProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value out;
    if (!rtThisBooleanValue(Value(thisBits), out)) {
        return rtThrowTypeError(
                   "Boolean.prototype.valueOf called on a value that is not a boolean")
            .rawBits();
    }
    return out.rawBits();
}

uint64_t booleanProtoToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value out;
    if (!rtThisBooleanValue(Value(thisBits), out)) {
        return rtThrowTypeError(
                   "Boolean.prototype.toString called on a value that is not a boolean")
            .rawBits();
    }
    return rtMakeString(out.asBool() ? "true" : "false").rawBits();
}

const NativeMethod kBooleanProtoMethods[] = {
    {"toString", booleanProtoToString, 0},
    {"valueOf", booleanProtoValueOf, 0},
};

// ---- the intrinsics --------------------------------------------------------

Value g_stringPrototype = Value::fromUndefined();
Value g_booleanPrototype = Value::fromUndefined();
Value g_numberPrototype = Value::fromUndefined();
Shape* g_stringWrapperShape = nullptr;
Shape* g_booleanWrapperShape = nullptr;
Shape* g_numberWrapperShape = nullptr;
// `valueOf`, arena-interned once, so the ToPrimitive guard below can walk a
// chain without allocating — which is what lets the shortcut be usable from
// `console.log`'s inspect walk, whose whole contract is that it cannot move
// the heap.
StringHeader* g_valueOfKey = nullptr;
// The two `valueOf` function objects as they were installed, so the guard can
// ask whether the one a lookup finds today is still the builtin. Read back off
// the finished prototypes rather than named here, because the string half is
// installed by builtin_string.cpp and this file must not hold a second opinion
// about which function that is.
Value g_pristineStringValueOf = Value::fromUndefined();
Value g_pristineBooleanValueOf = Value::fromUndefined();
Value g_pristineNumberValueOf = Value::fromUndefined();

// An OWN data property by arena key, with no allocation and no accessor call.
// `undefined` for a name the object does not carry.
Value readOwn(Value obj, StringHeader* key) {
    auto* self = obj.asObject<ObjectHeader>();
    PropertyInfo info;
    if (!self->shape || !self->shape->lookupProperty(key, info) || info.accessor) {
        return Value::fromUndefined();
    }
    return self->getSlot(info.slot);
}

// An object with the wrapper's internal slot, holding `data`. `create`
// allocates, so the payload is read back through its root afterwards.
Value newWrapper(Shape* shape, Rooted<Value>& data) {
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(), shape,
                                                              WrapperSlot::kCount);
    obj->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    obj->setInternalSlot(WrapperSlot::Data, data.get());
    return Value::fromObject(obj);
}

// All three prototypes, built by one initializer.
//
// They are wrappers of their own kind, which is what 22.1.3, 20.3.3 and 21.1.3
// say they are — `String.prototype` is a String exotic object with
// [[StringData]] "", so `String.prototype.length` is 0 rather than absent, and
// `Number.prototype` is a Number object with [[NumberData]] +0𝔽, so
// `Number.prototype + 1` is 1 — and their [[Prototype]] is `Object.prototype`
// rather than each other's. The chain a primitive string walks is therefore
// String.prototype, Object.prototype, null, exactly as a spec engine's is.
//
// They share ONE root shape, and it is not the one `{}` literals start from,
// for the reason a namespace object has its own: a site reading `"".indexOf`
// must not share a transition tree with every object literal in the program.
void ensureWrapperIntrinsics() {
    if (g_stringWrapperShape) return;
    // Reentrancy, and the same shape `ensureObjectIntrinsics` uses for
    // `Object.prototype.constructor`: decorating these objects asks for the two
    // constructor function objects, and building one of THOSE asks for the
    // prototype it records — so the two are published half-built and decorated
    // afterwards, which turns the cycle into a pair of ordinary assignments.
    // The nested call sees an object here and returns.
    if (g_stringPrototype.isObject()) return;

    Rooted<Value> objectProto{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(objectProto.get());

    Rooted<Value> emptyString{rtMakeString("")};
    Rooted<Value> stringProto{newWrapper(protoShape, emptyString)};
    Rooted<Value> falseValue{Value::fromBool(false)};
    Rooted<Value> booleanProto{newWrapper(protoShape, falseValue)};
    // 21.1.3: `Number.prototype` is itself a Number object, and its
    // [[NumberData]] is +0𝔽 — which is what makes `Number.prototype + 1` be 1
    // rather than NaN, and what puts it in `builtinTag`'s [[NumberData]] arm.
    Rooted<Value> zero{Value::fromDouble(0.0)};
    Rooted<Value> numberProto{newWrapper(protoShape, zero)};

    // Published before anything is installed on them, for the reentrancy above.
    // Permanent roots rather than plain statics: the collector moves these
    // objects, and the installs below allocate.
    g_stringPrototype = stringProto.get();
    g_booleanPrototype = booleanProto.get();
    g_numberPrototype = numberProto.get();
    rtHeap().add_permanent_root(&g_stringPrototype);
    rtHeap().add_permanent_root(&g_booleanPrototype);
    rtHeap().add_permanent_root(&g_numberPrototype);

    rtInstallStringMethods(stringProto);
    rtInstallStringPatternMethods(stringProto);
    // 22.1.3.36's `[Symbol.iterator]`, the one symbol-keyed member of the
    // object — installed with the same define-own, non-enumerable terms as
    // every string method (builtin_string_iterator.cpp).
    rtInstallStringIterator(stringProto);
    rtDefineMethods(booleanProto, kBooleanProtoMethods, std::size(kBooleanProtoMethods));
    rtInstallNumberMethods(numberProto);

    // 22.1.3.2 and 20.3.3.1, non-enumerable like everything else here. This is
    // the object `"abc".constructor` answers, and it is the same function
    // object the bare name `String` resolves to, because both come from the
    // code-pointer intern table.
    {
        Rooted<Value> key{rtMakeString("constructor")};
        Rooted<Value> ctor{rtStringConstructorObject()};
        stringProto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor,
                                                            nullptr, /*enumerable=*/false,
                                                            /*defineOwn=*/true);
    }
    {
        Rooted<Value> key{rtMakeString("constructor")};
        Rooted<Value> ctor{rtBooleanConstructorObject()};
        booleanProto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor,
                                                             nullptr, /*enumerable=*/false,
                                                             /*defineOwn=*/true);
    }
    // 21.1.3.1, and the reason `Number` had to stop being a namespace object:
    // this back-pointer has to be the same object the bare name resolves to,
    // and a namespace object is not a constructor for `new Number(1)` to
    // intercept.
    {
        Rooted<Value> key{rtMakeString("constructor")};
        Rooted<Value> ctor{rtNumberConstructorObject()};
        numberProto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor,
                                                            nullptr, /*enumerable=*/false,
                                                            /*defineOwn=*/true);
    }

    {
        Rooted<Value> name{rtMakeString("valueOf")};
        g_valueOfKey = StringHeader::internToArena(rtArena(), name.get().asString<StringHeader>());
        g_pristineStringValueOf = readOwn(stringProto.get(), g_valueOfKey);
        g_pristineBooleanValueOf = readOwn(booleanProto.get(), g_valueOfKey);
        g_pristineNumberValueOf = readOwn(numberProto.get(), g_valueOfKey);
    }

    rtHeap().add_permanent_root(&g_pristineStringValueOf);
    rtHeap().add_permanent_root(&g_pristineBooleanValueOf);
    rtHeap().add_permanent_root(&g_pristineNumberValueOf);
    // Last, because it is what the guard at the top of this function tests: a
    // wrapper cannot be allocated until there is a shape naming its prototype,
    // and nothing above allocates one.
    g_stringWrapperShape = rtNewRootShape(g_stringPrototype);
    g_booleanWrapperShape = rtNewRootShape(g_booleanPrototype);
    g_numberWrapperShape = rtNewRootShape(g_numberPrototype);
}

// The `valueOf` an ordinary lookup on this object would find, WITHOUT
// allocating: the key is the arena string interned above, and the walk is the
// chain walk with the accessor case excluded rather than called.
Value resolvedValueOf(Value obj) {
    ObjectHeader* holder = obj.asObject<ObjectHeader>();
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth && holder; ++depth) {
        PropertyInfo info;
        if (holder->shape && holder->shape->lookupProperty(g_valueOfKey, info)) {
            return info.accessor ? Value::fromUndefined() : holder->getSlot(info.slot);
        }
        holder = holder->protoAncestor(1);
    }
    return Value::fromUndefined();
}

}  // namespace

Value rtStringPrototype() {
    ensureWrapperIntrinsics();
    return g_stringPrototype;
}

Value rtBooleanPrototype() {
    ensureWrapperIntrinsics();
    return g_booleanPrototype;
}

Value rtNumberPrototype() {
    ensureWrapperIntrinsics();
    return g_numberPrototype;
}

void rtDefineMethods(Rooted<Value>& proto, const NativeMethod* methods, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Rooted<Value> key{rtMakeString(methods[i].name)};
        Rooted<Value> val{rtNativeFunction(methods[i].code, methods[i].arity)};
        // `defineOwn`, because this is DefineOwnProperty and not an assignment,
        // and `enumerable: false`, because that is what 22.1.3 and 20.3.3 say
        // every one of these is — see the file header for why that is the load-
        // bearing half.
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
}

Value rtMakeStringWrapper(Rooted<Value>& str) {
    ensureWrapperIntrinsics();
    return newWrapper(g_stringWrapperShape, str);
}

Value rtMakeBooleanWrapper(bool value) {
    ensureWrapperIntrinsics();
    Rooted<Value> data{Value::fromBool(value)};
    return newWrapper(g_booleanWrapperShape, data);
}

Value rtMakeNumberWrapper(double value) {
    ensureWrapperIntrinsics();
    Rooted<Value> data{Value::fromDouble(value)};
    return newWrapper(g_numberWrapperShape, data);
}

bool rtStringWrapperData(Value v, Value& out) {
    Value data;
    if (!wrapperData(v, data) || !data.isString()) return false;
    out = data;
    return true;
}

bool rtBooleanWrapperData(Value v, Value& out) {
    Value data;
    if (!wrapperData(v, data) || !data.isBool()) return false;
    out = data;
    return true;
}

bool rtNumberWrapperData(Value v, Value& out) {
    Value data;
    if (!wrapperData(v, data) || !data.isNumber()) return false;
    out = data;
    return true;
}

bool rtThisStringValue(Value self, Value& out) {
    if (self.isString()) {
        out = self;
        return true;
    }
    return rtStringWrapperData(self, out);
}

bool rtThisBooleanValue(Value self, Value& out) {
    if (self.isBool()) {
        out = self;
        return true;
    }
    return rtBooleanWrapperData(self, out);
}

bool rtThisNumberValue(Value self, Value& out) {
    if (self.isNumber()) {
        out = self;
        return true;
    }
    return rtNumberWrapperData(self, out);
}

Value rtStringCharAsString(Value str, uint32_t index) {
    StringHeader* s = str.asString<StringHeader>();
    if (index >= s->getLength()) return Value::fromUndefined();
    // ONE code unit, not one code point: 6.1.4 makes a String value a sequence
    // of UTF-16 code units, so indexing the second half of a surrogate pair
    // answers that half. `codePointAt` is the member that pairs them, and the
    // difference between the two is the whole reason both exist.
    const std::vector<uint16_t> one{s->charCodeAt(index)};
    return rtStringFromUnits(one);
}

bool rtWrapperPrimitive(Value v, Value& out) {
    Value data;
    if (!wrapperData(v, data)) return false;
    if (!data.isString() && !data.isBool() && !data.isNumber()) return false;
    const char* kind = data.isString() ? "String" : (data.isBool() ? "Boolean" : "Number");
    // This is not 7.1.1 ToPrimitive, which is built and lives in rt_convert.cpp.
    // It is the SHORTCUT the sites that cannot run user code take instead: for a
    // PRISTINE wrapper the answer OrdinaryToPrimitive would produce is exactly
    // this slot, because the `valueOf` it would call is the builtin installed
    // above. So the answer is available without the algorithm — and only while
    // that stays true, which is what the check below asks. A `valueOf` the
    // program has replaced is refused by name rather than silently ignored,
    // because ignoring it is a wrong answer where changing this one was the
    // whole point of overriding it.
    //
    // No conversion a PROGRAM spells comes through here any more: `+`,
    // `String(x)`, ToNumber, the relational operators, `==` and a computed
    // property key all run the real algorithm, which calls the override. What
    // is left is `console.log` and `JSON.stringify`, which have their own
    // algorithms and must not run user code at all — so a wrapper whose
    // `valueOf` the program replaced is named there rather than silently
    // printed from its slot.
    const Value pristine = data.isString()  ? g_pristineStringValueOf
                           : data.isBool() ? g_pristineBooleanValueOf
                                           : g_pristineNumberValueOf;
    if (resolvedValueOf(v).rawBits() != pristine.rawBits()) {
        fatal((std::string("unsupported: ToPrimitive of a ") + kind +
               " object whose `valueOf` is not the builtin (7.1.1 OrdinaryToPrimitive calls it, "
               "and bronze answers from the internal slot instead, which an override would "
               "change)")
                  .c_str());
    }
    out = data;
    return true;
}

bool rtStringDataOwnProperty(Value str, const std::string& key, StringOwnProperty& out) {
    // 10.4.3.4 StringCreate step 5 defines `length` as an own data property,
    // non-writable and non-configurable and NOT enumerable — the one own
    // property of a String that is not an index.
    if (key == "length") {
        out.value = Value::fromDouble(str.asString<StringHeader>()->getLength());
        out.enumerable = false;
        return true;
    }
    // 10.4.3.5 StringGetOwnProperty: a CANONICAL numeric string below the
    // length names one code unit, and anything else — "01", "1x", an index past
    // the end — is not an own property at all and falls through to the ordinary
    // lookup, which is what the caller does when this answers false.
    uint32_t index = 0;
    if (!rtIsIntegerLikeKey(key, index)) return false;
    if (index >= str.asString<StringHeader>()->getLength()) return false;
    // Rooted before the character below allocates; nothing above it does.
    Rooted<Value> self{str};
    out.value = rtStringCharAsString(self.get(), index);
    out.enumerable = true;
    return true;
}

bool rtStringDataHasOwnKey(Value str, const std::string& key) {
    if (key == "length") return true;
    uint32_t index = 0;
    return rtIsIntegerLikeKey(key, index) && index < str.asString<StringHeader>()->getLength();
}

bool rtStringExoticOwnProperty(Value obj, const std::string& key, Value& out) {
    Value data;
    if (!rtStringWrapperData(obj, data)) return false;
    StringOwnProperty own;
    if (!rtStringDataOwnProperty(data, key, own)) return false;
    out = own.value;
    return true;
}

void rtCheckStringExoticOwnKeys(Value, const char*) {}

bool rtConstructPrimitiveWrapper(Value fn, uint32_t argc, const uint64_t* argv, Value& out) {
    const char* name = rtPrimitiveWrapperConstructorName(fn);
    if (!name) return false;
    RootedArgs args(argc, argv);
    if (std::string_view(name) == "Boolean") {
        // 20.3.1.1: ToBoolean of the argument, in a [[BooleanData]] slot. The
        // object is TRUTHY whatever is in that slot, which is 7.1.2 ToBoolean
        // of an Object and the single most cited reason not to write this.
        out = rtMakeBooleanWrapper(bronze_truthy(args[0].rawBits()));
        return true;
    }
    if (std::string_view(name) == "Number") {
        // 21.1.1.1 with NewTarget present: no argument at all is +0𝔽, and
        // anything else is ToNumeric — which for a SYMBOL is the TypeError
        // 6.1.5.1 names.
        if (args.count() == 0) {
            out = rtMakeNumberWrapper(0.0);
            return true;
        }
        out = rtNumberValueOfArgument(args[0]);
        if (rtExceptionPending()) {
            out = Value::fromUndefined();
            return true;
        }
        out = rtMakeNumberWrapper(out.asNumber());
        return true;
    }
    // 22.1.1.1 with NewTarget present: no argument at all is the empty string,
    // and a SYMBOL is put through ToString — which throws — rather than through
    // SymbolDescriptiveString. That shortcut is step 2 and belongs to the
    // `new`-less form alone, so `new String(sym)` is the TypeError the language
    // says it is where `String(sym)` is the description.
    Rooted<Value> str{args.count() == 0 ? rtMakeString("") : rtValueToString(args[0])};
    if (rtExceptionPending()) {
        out = Value::fromUndefined();
        return true;
    }
    out = rtMakeStringWrapper(str);
    return true;
}

}  // namespace bronze::runtime
