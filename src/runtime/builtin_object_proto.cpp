// `Object.prototype` (ECMA-262 20.1.3) — the intrinsic every plain object
// inherits from, and the six members bronze answers on it.
//
// It is a real object on the real chain, found by the ordinary prototype walk —
// not a table consulted beside it, which is what every other builtin receiver
// in bronze still is. The difference is the whole point: a method here can be
// held, compared, passed to `.call`, and replaced, and `Object.getPrototypeOf({})`
// has something true to answer.
//
// Its own translation unit, and the seam is the receiver. Every function in
// builtin_object.cpp takes its subject as an ARGUMENT — `Object.keys(o)` is a
// static that could have been a free function — while every function here takes
// it as `this`, off a chain a program can reach and modify. That is why the two
// disagree about what a bad receiver means: a static raises the TypeError its
// clause names, and a method here can only have been reached THROUGH a
// receiver, so a kind bronze cannot walk is a refusal rather than a throw.
//
// The two files still build one pair of intrinsics, because 20.1.2.1 and
// 20.1.3.1 make the namespace and the prototype each other's property — so
// `ensureObjectIntrinsics` stays in builtin_object.cpp and reaches the table
// below through `rtInstallObjectProtoMethods`, exactly as `String.prototype`'s
// members reach the object builtin_wrappers.cpp allocates.
//
// Every member is defined NON-ENUMERABLE, per 20.1.3. That is not tidiness:
// `for-in` walks the prototype chain, so an enumerable member here would appear
// in every for-in over every object in the program. `Object.keys`, spread and
// `JSON.stringify` ask for own enumerable keys and so cannot see it either,
// which is why this object could be introduced under a suite of pinned
// expectations without moving one of them.

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Steps 1 and 2 of every member here except `toString`: `null` and `undefined`
// have no ToObject, and the TypeError that says so is the one thing these
// clauses agree on before they diverge.
bool requireNonNullish(Value self, const char* method) {
    if (!self.isNull() && !self.isUndefined()) return true;
    rtThrowTypeError(std::string("Object.prototype.") + method + " called on null or undefined");
    return false;
}

// ---- Own properties, kind by kind ------------------------------------------
//
// `hasOwnProperty` (20.1.3.2) and `propertyIsEnumerable` (20.1.3.4) are one
// question asked twice — does the RECEIVER ITSELF define this key, and is that
// definition enumerable — so they are one function here. Written twice they
// would come to disagree about a kind, and then `propertyIsEnumerable`
// answering false where `hasOwnProperty` answered true would mean "own and
// non-enumerable" on some receivers and "this one has no arm" on others.
//
// It is NOT `in`'s dispatch (rt_operator.cpp) with the chain walk removed, and
// the difference is the point: `in` asks own-OR-INHERITED and answers a
// shapeless receiver out of its member table, because that table stands in for
// the prototype object bronze has not built. Every entry in such a table is
// therefore an INHERITED member, and none of them may be visible from here.
// `'push' in arr` is true and `arr.hasOwnProperty('push')` is false, and that
// is not a divergence between two copies of one answer — it is the two
// questions giving their own.
//
// A receiver whose own keys bronze genuinely cannot enumerate is refused by
// name rather than reported absent. There is exactly one: a String exotic
// OBJECT, whose 10.4.3.4 index properties are synthesised on the property path
// and live in no shape (rt_object.cpp carries the reasoning).
static_assert(HeapKind::Count == 12,
              "a HeapKind was added or removed: give the own-property switch below an arm for "
              "it. `hasOwnProperty` and `propertyIsEnumerable` are reachable from EVERY "
              "receiver now that the chain runs past the member tables, so a kind with no arm "
              "is a receiver those two cannot answer about.");

bool ownProperty(Rooted<Value>& self, Value keyVal, bool& enumerable) {
    enumerable = false;
    // ToPropertyKey (7.1.19) FIRST: it runs ToString, which allocates, so no
    // header below may be read across it. What it hands back is arena-interned
    // and immortal, which is what lets the string be read again after the
    // allocations further down.
    PropertyKey name = rtInternPropertyKey(keyVal);
    const std::string key = name.isString() ? rtAsciiChars(name.string()) : std::string();
    uint32_t index = 0;

    if (!self.get().isObject()) {
        // Step 2 is ToObject(this). bronze builds no box for a number, a
        // boolean or a symbol — and needs none: 21.1, 20.3 and 20.4 give those
        // wrappers no own property, so the answer is false whatever the key was.
        // A STRING is the one primitive whose box has own properties, and
        // 10.4.3.4 and 10.4.3.5 make them exactly its `length` and its indices.
        if (!self.get().isString() || !name.isString()) return false;
        if (key == "length") return true;  // 10.4.3.4: non-enumerable
        if (!rtIsIntegerLikeKey(key, index)) return false;
        if (index >= self.get().asString<StringHeader>()->getLength()) return false;
        enumerable = true;  // 10.4.3.5 gives each index [[Enumerable]]: true
        return true;
    }

    switch (self.get().asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Plain: {
            rtCheckStringExoticOwnKeys(self.get(), "testing");
            auto* obj = self.get().asObject<ObjectHeader>();
            PropertyInfo info;
            if (!obj->shape || !obj->shape->lookupProperty(name, info)) return false;
            enumerable = info.enumerable;
            return true;
        }
        case HeapKind::Function: {
            // Three storage places, and 10.2 gives each its attributes.
            // `prototype` (10.2.11) and the `length`/`name` pair (10.2.10,
            // 10.2.9) are all non-enumerable and live outside the statics
            // object; a static or an assigned property carries whatever
            // attribute the shape recorded for it.
            //
            // `prototype` answers true for every function, which is the answer
            // `in` already gives: bronze materialises the slot on demand for
            // any function, so an arrow — which 10.2.11 gives no `prototype`
            // at all — is over-reported by both spellings together rather than
            // by one of them.
            if (key == "prototype") return true;
            if ((key == "length" || key == "name") &&
                self.get().asObject<FunctionHeader>()->name != nullptr) {
                return true;
            }
            Value props = self.get().asObject<FunctionHeader>()->properties;
            if (!props.isObject()) return false;
            // The statics object's OWN shape and not its chain: `extends` links
            // it to the base class's statics, and a static a derived class
            // INHERITS is not an own property of the derived constructor.
            PropertyInfo info;
            auto* holder = props.asObject<ObjectHeader>();
            if (!holder->shape || !holder->shape->lookupProperty(name, info)) return false;
            enumerable = info.enumerable;
            return true;
        }
        case HeapKind::Array: {
            // ArrayCreate's `length` (10.4.2.2) is own, writable and
            // non-enumerable; an element is own and enumerable, because
            // CreateDataProperty defines it so. A HOLE is a key that is not
            // there at all, which is the whole difference `delete a[1]` makes.
            if (key == "length") return true;
            if (name.isString() && rtIsIntegerLikeKey(key, index)) {
                if (!self.get().asObject<ArrayHeader>()->hasElem(index)) return false;
                enumerable = true;
                return true;
            }
            // A match array's `index`, `input` and `groups` (22.2.7.2), which
            // are the only NAMED properties an array in bronze can carry.
            Value props = self.get().asObject<ArrayHeader>()->properties;
            if (!props.isObject()) return false;
            PropertyInfo info;
            auto* holder = props.asObject<ObjectHeader>();
            if (!holder->shape || !holder->shape->lookupProperty(name, info)) return false;
            enumerable = info.enumerable;
            return true;
        }
        case HeapKind::TypedArray: {
            // 10.4.5: an integer index within the length is the ONLY own
            // property a typed array has. `length`, `buffer`, `byteLength`,
            // `byteOffset` and `BYTES_PER_ELEMENT` all read like own properties
            // on the property path and are not: 23.2.3 makes the first four
            // accessors on `%TypedArray%.prototype` and 23.2.6.2 puts
            // `BYTES_PER_ELEMENT` on the constructor's prototype.
            if (!name.isString() || !rtIsIntegerLikeKey(key, index)) return false;
            if (index >= self.get().asObject<TypedArrayHeader>()->length) return false;
            enumerable = true;
            return true;
        }
        case HeapKind::RegExp:
            // 22.2.3.1 RegExpAlloc defines exactly one own property, and
            // non-enumerably. Everything else 22.2.6 gives a RegExp — `source`,
            // `flags`, `global` and the rest — is an accessor on the prototype,
            // however much bronze's header-backed answers look like own data.
            return key == "lastIndex";
        case HeapKind::Map:
        case HeapKind::Set:
        case HeapKind::ArrayBuffer:
        case HeapKind::DataView:
            // 24.1.3, 24.2.3, 25.1.6 and 25.3.4 put every member on a
            // prototype. These four carry internal slots and no own property at
            // all — `size` and `byteLength` included, which are accessors.
            return false;
        case HeapKind::ModuleNamespace: {
            // 10.4.6.1: an export is own, writable and ENUMERABLE, and
            // `@@toStringTag` is the one own key that is not an export.
            if (name.isSymbol()) {
                Value tag;
                return rtModuleNamespaceOwnSymbol(self.get(), name.toValue(), tag);
            }
            if (!rtModuleNamespaceHasExport(self.get(), name.string())) return false;
            enumerable = true;
            return true;
        }
        case HeapKind::Iterator:
        case HeapKind::Env:
            // Not JS values: nothing hands a program one, so reaching this is a
            // lowering bug rather than something a program did.
            fatal("internal: an own-property test on an environment or iteration record");
        default:
            fatal((std::string("internal: an own-property test on ") +
                   rtObjectKindName(self.get()) + ", a heap kind this switch has no arm for")
                      .c_str());
    }
}

uint64_t objectProtoHasOwnProperty(uint64_t, uint64_t thisBits, uint32_t argc,
                                   const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireNonNullish(self.get(), "hasOwnProperty")) {
        return Value::fromUndefined().rawBits();
    }
    bool enumerable = false;
    return Value::fromBool(ownProperty(self, args[0], enumerable)).rawBits();
}

// 20.1.3.4. Own AND enumerable — a name that is only inherited answers false
// here where `in` answers true, and a class method (15.7.14 defines it
// non-enumerable) answers false where `hasOwnProperty` answers true.
uint64_t objectProtoPropertyIsEnumerable(uint64_t, uint64_t thisBits, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireNonNullish(self.get(), "propertyIsEnumerable")) {
        return Value::fromUndefined().rawBits();
    }
    bool enumerable = false;
    const bool own = ownProperty(self, args[0], enumerable);
    return Value::fromBool(own && enumerable).rawBits();
}

// 20.1.3.3. Walks the ARGUMENT's chain looking for the receiver, so it answers
// about ancestry rather than about identity: an object is not its own
// prototype, and the walk starts one link up for that reason.
//
// The receiver may be ANY value — it is compared by identity and never read.
// A PRIMITIVE one is false by construction: step 2's ToObject would make a
// fresh box, and a box nothing else has ever seen is in no chain.
//
// The walk over the argument is where bronze's missing intrinsics would show,
// and they do not stop it. Above a shapeless object the chain is
// `<Kind>.prototype` and then `Object.prototype`, and bronze never hands the
// first of those to a program — so it cannot be the receiver, and the whole
// remaining question is whether the receiver is `Object.prototype`. That is how
// `Object.prototype.isPrototypeOf([])` reaches its `true` without an
// `Array.prototype` existing to walk through, where before this it answered a
// silent `false`. A module namespace is the one object with no chain at all
// (10.4.6.1 fixes [[Prototype]] at null).
uint64_t objectProtoIsPrototypeOf(uint64_t, uint64_t thisBits, uint32_t argc,
                                  const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireNonNullish(self.get(), "isPrototypeOf")) {
        return Value::fromUndefined().rawBits();
    }
    if (!args[0].isObject() || !self.get().isObject()) {
        return Value::fromBool(false).rawBits();
    }
    // Before any raw pointer is taken: the first call builds the intrinsic.
    const uint64_t objectProto = rtObjectPrototype().rawBits();
    const uint64_t target = self.get().rawBits();

    Rooted<Value> walker{args[0]};
    for (uint32_t depth = 0; depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        const uint16_t kind = walker.get().asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::ModuleNamespace) return Value::fromBool(false).rawBits();
        if (kind != HeapKind::Plain) {
            return Value::fromBool(target == objectProto).rawBits();
        }
        ObjectHeader* next = walker.get().asObject<ObjectHeader>()->protoAncestor(1);
        if (!next) return Value::fromBool(false).rawBits();
        const Value nextVal = Value::fromObject(next);
        if (nextVal.rawBits() == target) return Value::fromBool(true).rawBits();
        walker.set(nextVal);
    }
    fatal("prototype chain too deep (a cycle?)");
}

// 20.1.3.7 ToObject(this), which for an object is the object — whatever kind it
// is, since an identity function needs nothing from its receiver.
//
// Answering with the receiver is what makes `'' + {}` "[object Object]" rather
// than a TypeError: 7.1.1.1 calls this first under hint default, gets something
// that is not a primitive, and carries on to `toString`. A `valueOf` that
// answered a primitive here would stop that search, which is exactly what a
// program's own override is for.
//
// A PRIMITIVE is where ToObject has work to do, and the box is what bronze does
// not build. Refused by name rather than answered with the primitive itself,
// which would make `x.valueOf() === x` true where the language says the wrapper
// makes it false. Reachable only through `.call`: every primitive that has a
// prototype of its own answers `valueOf` from it first.
uint64_t objectProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireNonNullish(self, "valueOf")) return Value::fromUndefined().rawBits();
    if (self.isObject()) return self.rawBits();
    fatal("unsupported: Object.prototype.valueOf on a primitive receiver (20.1.3.7 is "
          "ToObject, and bronze does not build the wrapper object it would return)");
}

// 20.1.3.6 steps 4 through 14: the BUILTIN TAG, chosen from what the receiver
// is rather than from anything it carries.
//
// It is a fixed list and an ordered one. `Array` comes before `Arguments` and
// `Arguments` before `Function` because an arguments object is an array in
// bronze (10.2.11 is stood in for by an ordinary array with a `callee`
// accessor) and the specification's steps are tried in that order too — step 4
// asks IsArray, step 5 asks for [[ParameterMap]]. Reversing them would tag
// every arguments object "Array".
//
// The clause's remaining entries are internal slots, and bronze answers each
// from the nearest thing it has to one:
//
//   - [[ErrorData]] is `rtIsErrorInstance`, a walk to `Error.prototype`. bronze
//     builds every error from one of five constructors whose prototypes all
//     chain to it, so the walk is the brand.
//   - [[BooleanData]], [[StringData]] and [[NumberData]] are the wrappers'
//     internal slots, read through the same accessors `valueOf` uses.
//   - [[DateValue]] cannot occur: bronze has no `Date`, so no receiver can have
//     one, and a branch for it would name a kind nothing can produce.
const char* builtinTag(Value self) {
    if (self.isNumber()) return "Number";   // step 9, via the box that is not built
    if (self.isString()) return "String";   // step 10
    if (self.isBool()) return "Boolean";    // step 8
    // A symbol reaches none of steps 4-14, so its tag is step 14's "Object" and
    // the real answer comes from step 15: `Symbol.prototype[@@toStringTag]` is
    // the string "Symbol" (20.4.3.6), found by the ordinary walk.
    if (!self.isObject()) return "Object";
    switch (self.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Array:
            return rtIsArgumentsObject(self) ? "Arguments" : "Array";
        case HeapKind::Function:
            return "Function";
        case HeapKind::RegExp:
            return "RegExp";
        default:
            break;
    }
    if (rtIsErrorInstance(self)) return "Error";
    if (Value data; rtStringWrapperData(self, data)) return "String";
    if (Value data; rtBooleanWrapperData(self, data)) return "Boolean";
    if (Value data; rtNumberWrapperData(self, data)) return "Number";
    // Everything else — a plain object, a Map, a Set, a typed array, a module
    // namespace — is step 14's "Object", and reads as something else only if
    // step 15's @@toStringTag says so.
    return "Object";
}

// 20.1.3.6 Object.prototype.toString.
//
// Its receiver rule is the one thing here that is not the ordinary one: steps 1
// and 2 answer for `undefined` and `null` BEFORE step 3's ToObject, which is
// why those two have an answer at all where every other member of this file
// raises for them. So this does not go through `requireProtoReceiver`.
//
// Nor does it refuse a receiver bronze cannot walk. The other members of
// 20.1.3 need the receiver's own property table and refuse a kind that has
// none; this one needs a tag, and every kind has one — which is exactly why
// `Object.prototype.toString.call(x)` is the type probe real code uses it as,
// and why making it die on a Map would be worse than useless.
uint64_t objectProtoToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (self.isUndefined()) return rtMakeString("[object Undefined]").rawBits();
    if (self.isNull()) return rtMakeString("[object Null]").rawBits();

    // Step 4 onward wants O = ToObject(this). bronze does not build the box:
    // every question below is about what the value IS, and a wrapper's answer
    // to each is the wrapped primitive's — `builtinTag` names the two places
    // that would have differed, and neither does.
    std::string tag = builtinTag(self);

    // Steps 15-17. An ordinary property GET, so a user-installed
    // `[Symbol.toStringTag]` — own or inherited — is found by the ordinary
    // prototype walk and WINS over the tag above. A non-string one is ignored
    // rather than coerced: step 17 keeps the builtin tag unless step 16 found a
    // String, which is what makes `{ [Symbol.toStringTag]: 42 }` read
    // "[object Object]" and not "[object 42]".
    //
    // For a receiver with no shape the walk has nowhere to go, and the answer
    // comes from rt_prop.cpp's `toStringTagOf` — the switch over heap kinds
    // that stands in for the `Map.prototype` and `%TypedArray%.prototype`
    // objects bronze does not have. The order is what makes that sound: this
    // read runs first and unconditionally, so anything a program installed is
    // consulted before any stand-in can be.
    Rooted<Value> receiver{self};
    Rooted<Value> key{Value::fromSymbol(rtSymbolToStringTag())};
    Rooted<Value> found{
        Value(bronze_elem_get(receiver.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (found.get().isString()) tag = rtUtf8Chars(found.get().asString<StringHeader>());

    return rtMakeString("[object " + tag + "]").rawBits();
}

// 20.1.3.5 Object.prototype.toLocaleString: `Invoke(O, "toString")` with no
// arguments, and nothing else. No locale is consulted — the name is a hook the
// subclasses of 21.1.3.4, 23.1.3.32 and friends override, and the base
// definition is a forwarding call — so this breaks no determinism rule.
//
// It reads `toString` off the RECEIVER rather than calling the function above,
// because that is the observable difference the clause exists for: an object
// with its own `toString` gets its own, and that is the whole point of
// `toLocaleString` being specified as an Invoke.
uint64_t objectProtoToLocaleString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> key{rtMakeString("toString")};
    Rooted<Value> method{Value(bronze_elem_get(self.get().rawBits(), key.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (!method.get().isObject() ||
        method.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Object.prototype.toLocaleString called on a receiver whose "
                                "`toString` is not a function")
            .rawBits();
    }
    return bronze_dynamic_call(method.get().rawBits(), self.get().rawBits(), 0, nullptr);
}

const NativeMethod kObjectProtoMethods[] = {
    {"hasOwnProperty", objectProtoHasOwnProperty, 1},
    {"isPrototypeOf", objectProtoIsPrototypeOf, 1},
    {"propertyIsEnumerable", objectProtoPropertyIsEnumerable, 1},
    {"toLocaleString", objectProtoToLocaleString, 0},
    {"toString", objectProtoToString, 0},
    {"valueOf", objectProtoValueOf, 0},
};

// 20.1.3 members bronze has not built, diagnosed by name on a plain object's
// full-chain miss. `__defineGetter__` and its three siblings are Annex B
// (B.2.2) and are deliberately absent from this list as well as from bronze:
// nothing in it is a name a program should be reaching for.
const char* const kObjectProtoUnimplemented[] = {
    "__proto__",
};

}  // namespace

void rtDefineToStringTag(Rooted<Value>& obj, const char* tag) {
    Rooted<Value> key{Value::fromSymbol(rtSymbolToStringTag())};
    Rooted<Value> val{rtMakeString(tag)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true);
}

void rtInstallObjectProtoMethods(Rooted<Value>& proto) {
    // `rtDefineMethods` is a DefineOwnProperty with `enumerable: false`, which
    // is what 20.1.3 says every one of these is — and not an assignment, so a
    // member here cannot be swallowed by a setter anything installed first.
    rtDefineMethods(proto, kObjectProtoMethods, std::size(kObjectProtoMethods));
}

void rtObjectProtoCheckMissingMember(const std::string& key) {
    rtCheckUnimplementedMember("Object.prototype", kObjectProtoUnimplemented,
                               std::size(kObjectProtoUnimplemented), key);
}

// The rest of the chain, for a receiver whose members bronze answers from a C
// table beside it rather than from a prototype object.
//
// Every object in the language inherits from this one, directly or through a
// prototype in between. A receiver with no shape cannot be walked, so before
// this step the search simply ENDED — and `hasOwnProperty`, `valueOf`,
// `isPrototypeOf` and `propertyIsEnumerable` read `undefined` on a function, an
// array, a Map, a Set and a RegExp. That is the silent fallback the house rules
// rank below a refusal, and making this object real is what turned it from a
// missing feature into a hole: `f.toString` was already diagnosed by name from
// `Function.prototype`'s table one link BELOW here, while `f.valueOf` — one
// link ABOVE — answered `undefined`.
//
// Skipping the intermediate prototype is exact rather than an approximation,
// and the reason is a property of those tables rather than luck. A name
// ECMA-262 puts on `Array.prototype`, `Function.prototype`, `Map.prototype`,
// `Set.prototype`, `RegExp.prototype`, `%TypedArray%.prototype`,
// `ArrayBuffer.prototype` or `DataView.prototype` is either answered by that
// receiver's table or refused by name from it — which is why
// `Array.prototype.toString` and `%TypedArray%.prototype.toLocaleString` are on
// their unimplemented lists — so nothing that would SHADOW a member of this
// object can reach this step. What arrives here is what the intermediate does
// not define, which is what an ordinary walk would have brought here anyway.
//
// The receiver is threaded through so that an accessor found here would run
// against the value the program wrote rather than against this object. No
// member of `Object.prototype` is one today; passing it keeps that a fact about
// the members rather than an assumption of the walk.
Value rtObjectProtoMember(Rooted<Value>& receiver, const std::string& key) {
    Rooted<Value> proto{rtObjectPrototype()};
    Rooted<Value> keyStr{rtMakeString(key)};
    const Value found = proto.get().asObject<ObjectHeader>()->getProp(
        rtHeap(), keyStr, /*ic=*/nullptr, receiver.slot_ptr());
    // A miss here is the END of the chain — 20.1.3 fixes this object's own
    // [[Prototype]] at null — so it is also where a 20.1.3 member bronze has
    // not built is named, exactly as it is for a plain object.
    if (found.isUndefined()) rtObjectProtoCheckMissingMember(key);
    return found;
}

// The same step for `in`, which asks whether the member is THERE rather than
// what it is. One level and no walk: this object's [[Prototype]] is null, so
// its own properties are the whole of what the chain has left to offer.
bool rtObjectProtoHasMember(const std::string& key) {
    Rooted<Value> proto{rtObjectPrototype()};
    Rooted<Value> keyStr{rtMakeString(key)};
    auto* obj = proto.get().asObject<ObjectHeader>();
    uint32_t slot = 0;
    if (obj->shape && obj->shape->lookupProperty(keyStr.get().asString<StringHeader>(), slot)) {
        return true;
    }
    rtObjectProtoCheckMissingMember(key);
    return false;
}

}  // namespace bronze::runtime
