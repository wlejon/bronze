// The `Object` namespace: whole-object questions — its keys, its prototype, its
// identity, the four copies, and the integrity levels' entry points.
//
// `Object` is a value here, rather than a name the compiler recognises at a
// call. Recognising `Object.keys(...)` at the CALL made every other member a
// compile error, which was
// the right answer while `keys` was the only member; it does not survive a
// second one, because `Object.assign` and `Object.defineProperty` would each
// need their own IL op and their own arity check in lowering. So `Object` joins
// `Math` as an ordinary namespace object resolved by name, and `Object.keys`
// keeps its instruction as a fast path over the SAME C++ function — one
// implementation, so the two spellings cannot drift.
//
// The four members whose subject is a property DESCRIPTOR are in
// builtin_object_descriptor.cpp; builtin_object.h names that seam and what
// crosses it. Nothing in this file moves an object out of its shape chain,
// which is the observable difference between the two halves.
//
// Every function here takes its subject as an ARGUMENT. The ones that take it
// as `this` are `Object.prototype`'s and live in builtin_object_proto.cpp,
// which is the seam: a static raises the TypeError its clause names for a
// receiver it will not take, while a method can only have been REACHED through
// its receiver, so a kind bronze cannot walk is a refusal there rather than a
// throw. `ensureObjectIntrinsics` at the foot of this file still builds both
// objects, because each is a property of the other.

#include "runtime/builtin_object.h"

#include <iterator>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/namespace.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// ToPropertyKey (7.1.19) into the immortal form a DictEntry can hold. Interning
// is not an optimization here: a DictEntry outlives every collection, so a heap
// key would dangle.
//
// A SYMBOL is returned as it stands and never converted: it already lives in
// the arena, and running ToString on one is the TypeError this whole type
// exists to raise. That is also what lets `Object.defineProperty`,
// `getOwnPropertyDescriptor` and `hasOwn` take a symbol key without a branch of
// their own.
PropertyKey rtInternPropertyKey(Value keyVal) {
    if (keyVal.isSymbol()) return PropertyKey::fromValue(keyVal);
    Rooted<Value> str{rtValueToString(keyVal)};
    return PropertyKey::forString(
        StringHeader::internToArena(rtArena(), str.get().asString<StringHeader>()));
}

// Own-property existence, shared by `Object.prototype.hasOwnProperty` and
// `Object.hasOwn` — 20.1.3.2 and 20.1.2.13 are the same operation with the
// receiver in a different position, and writing it twice is how the two would
// come to disagree about a dictionary-mode object.
//
// The key is interned BEFORE the object is read: `rtInternPropertyKey` runs
// ToString, which allocates, so an ObjectHeader* taken across it would be stale.
bool rtHasOwnPropertyNamed(Rooted<Value>& self, Value key) {
    // The characters first (10.4.3.3), then the shape: a String exotic object
    // has both, and `String.prototype` is the one every program meets — its
    // [[StringData]] is "" and every string method is a property of its shape.
    // A miss in the characters is therefore not the answer, only the first
    // half of it (10.4.3.1 step 3 hands the rest to OrdinaryGetOwnProperty).
    if (Value data; rtStringWrapperData(self.get(), data)) {
        if (!key.isSymbol()) {
            const std::string keyStr = rtObjectKeyTextOf(key);
            if (rtExceptionPending()) return false;
            if (rtStringDataHasOwnKey(data, keyStr)) return true;
        }
        // A PRIMITIVE string has no shape to walk, so its characters really
        // were the whole answer.
        if (!self.get().isObject()) return false;
    }
    PropertyKey name = rtInternPropertyKey(key);
    auto* obj = self.get().asObject<ObjectHeader>();
    uint32_t slot = 0;
    return obj->shape && obj->shape->lookupProperty(name, slot);
}

bool rtObjectIsPlain(Value v) {
    // The byte-store family opens with an ObjectHeader (typed_array.h), so
    // for every question about a SHAPE it is a plain object. A typed array's
    // elements are the one thing outside its shape, and the own-key walks
    // below list them by hand (`ObjectOwnKeys::TypedArray`).
    return v.isObject() && HeapKind::carriesShape(v.asObject<HeapObjectHeader>()->flags);
}

namespace {

bool isPlainObject(Value v) { return rtObjectIsPlain(v); }

// Why bronze cannot describe or redefine this receiver's own properties. One
// sentence per storage story, because "unsupported" without the reason is a
// reader's dead end.
const char* propertyStoreReason(Value v) {
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case HeapKind::Function:
            return "its own keys come from three places — a `prototype` slot, a `length` and a "
                   "`name` in the header, and a side object of statics — and only the last is a "
                   "shape a descriptor could be written to";
        default:
            return "it keeps no property table, so there is nothing here to describe";
    }
}

// The receiver of an `Object` member that needs a property TABLE — one it can
// describe, redefine, or copy into.
//
// `isPlainObject` is the wrong predicate to gate these on, and the wrongness is
// not a matter of degree. It answers "does this keep its properties in a
// shape"; the first step of every clause below asks whether the value is an
// OBJECT, and an array is an object. So each of these told a program its array
// "is not an object" — a false statement about the receiver, and the kind that
// sends a reader looking for the wrong bug. It is the same mistake the
// integrity levels made: a predicate answering a different question than the
// step asks.
//
// Three answers, and the middle one is the fix. A plain object proceeds. A
// PRIMITIVE gets the TypeError the clause specifies, and the message is now
// true of what it was actually given. Any other object is refused BY NAME,
// saying what it is and what about its storage bronze cannot reach —
// `getOwnPropertyNames`'s precedent, which named the kinds before any of the
// rest of them did.
}  // namespace

[[noreturn]] void refuseObjectKind(Value v, const char* member) {
    fatal((std::string("unsupported: Object.") + member + " on " + rtObjectKindName(v) + " (" +
           propertyStoreReason(v) + ")")
              .c_str());
}

bool rtObjectRequirePropertyTable(Value v, const char* member) {
    if (isPlainObject(v)) return true;
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function) return true;
    // An array too: its elements and `length` are described and redefined by
    // their own rules (10.4.2.1, builtin_object_descriptor.cpp's array arm)
    // and its named properties live in a side object that is an ordinary
    // shape.
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Array) return true;
    // And a proxy: its [[DefineOwnProperty]] is 10.5.6, the `defineProperty`
    // trap, which the apply reaches as one more receiver kind
    // (builtin_object_define.cpp).
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) return true;
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::TypedArray) return true;
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::RegExp) return true;
    if (!v.isObject()) {
        rtThrowTypeError(std::string("Object.") + member +
                         " called on a value that is not an object");
        return false;
    }
    rtThrowTypeError(std::string("unsupported: Object.") + member + " on " +
                     rtObjectKindName(v) + " (" + propertyStoreReason(v) + ")");
    return false;
}

ObjectOwnKeys rtObjectOwnKeysOf(Value v, const char* member) {
    if (v.isNull() || v.isUndefined()) {
        // The same sentence the other refusals use, and it is true of exactly
        // these two: 7.1.18's only failures are the two values that are not
        // objects and have no box.
        rtThrowTypeError(std::string("Object.") + member +
                         " called on a value that is not an object");
        return ObjectOwnKeys::Threw;
    }
    if (v.isString()) return ObjectOwnKeys::StringChars;
    if (Value data; rtStringWrapperData(v, data)) return ObjectOwnKeys::StringChars;
    if (!v.isObject()) return ObjectOwnKeys::None;
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::TypedArray) {
        return ObjectOwnKeys::TypedArray;
    }
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::RegExp) return ObjectOwnKeys::RegExp;
    if (isPlainObject(v)) return ObjectOwnKeys::Shape;
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
        return ObjectOwnKeys::Function;
    }
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::Array) return ObjectOwnKeys::Array;
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) return ObjectOwnKeys::Proxy;
    if (rtIsModuleNamespace(v)) return ObjectOwnKeys::Namespace;
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::ArrayBuffer ||
        v.asObject<HeapObjectHeader>()->flags == HeapKind::DataView) {
        return ObjectOwnKeys::None;
    }
    rtThrowTypeError(std::string("unsupported: Object.") + member + " on " +
                     rtObjectKindName(v) + " (" + propertyStoreReason(v) + ")");
    return ObjectOwnKeys::None;
}

std::string rtObjectKeyTextOf(Value keyVal) {
    Rooted<Value> str{rtValueToString(keyVal)};
    if (rtExceptionPending()) return std::string();
    return rtUtf8Chars(str.get().asString<StringHeader>());
}

// The six integrity-level members — `freeze`, `seal`, `preventExtensions` and
// their predicates — are in integrity.cpp. They left this file when they
// stopped being about plain objects: what each of them DOES is decide where the
// receiver keeps [[Extensible]], and that question belongs beside every path
// that reads the answer back (an array's element write, a function's
// `prototype`), not beside `Object.keys`.

// 20.1.2.12 Object.getPrototypeOf.
//
// A plain object answers from its shape, and `Object.prototype` is what the
// chain of a `{}` ends at — so `null` here means what the language means by it:
// `Object.create(null)`, and `Object.prototype` itself. The two used to be
// indistinguishable, which is why this was a named error.
//
// An array and a function still are: their members are answered by the
// property path rather than found on a prototype object, so there is no
// `Array.prototype` to return and `null` would be a lie about a chain that
// really does have methods on it.
//
// The [[Prototype]] of a receiver that keeps no shape — an array and a
// function, whose prototypes are FIXED by 23.1.6.1 and 20.2.3 and so need no
// storage to be known. False for every other such kind, whose prototype is an
// intrinsic bronze builds no object for.
//
// One function because two operations must agree about it: `getPrototypeOf`
// answers with it, and `setPrototypeOf` compares against it to decide whether a
// write changes anything (10.1.2 step 2).
bool rtShapelessPrototypeOf(Value obj, Value& out) {
    if (!obj.isObject()) return false;
    const uint16_t kind = obj.asObject<HeapObjectHeader>()->flags;
    // A SUBCLASS instance stores its [[Prototype]] after all — on the box
    // holding its ordinary half (runtime/native_base.h) — so it is asked
    // FIRST, ahead of the fixed intrinsic answer below. Without this line
    // `Object.getPrototypeOf(new (class extends Array)())` would report
    // %Array.prototype% and skip the subclass's, which is the one link the
    // program can see.
    if (Value proto = rtExoticSubclassPrototype(obj); proto.isObject() || proto.isNull()) {
        out = proto;
        return true;
    }
    if (kind == HeapKind::Array) {
        out = rtArrayPrototypeObject();
        return true;
    }
    if (kind == HeapKind::RegExp) {
        out = rtRegExpPrototypeObject();
        return true;
    }
    if (kind != HeapKind::Function) return false;
    // A DERIVED CLASS stores its own [[Prototype]] — the base constructor,
    // 15.7.14 step 6 — so it is asked first, on the same terms as the exotic
    // subclass above: a stored link is the one the program can see, and the
    // fixed intrinsic below is only what a function that never got one has.
    if (Value stored = obj.asObject<FunctionHeader>()->parent; !stored.isUndefined()) {
        out = stored;
        return true;
    }
    // A generator, an async function and an async generator do not inherit
    // from %Function.prototype% at all: 27.3.4, 27.7.4 and 27.4.4 put each
    // one's own kind prototype in between. Asked before the ordinary answer
    // below, and never for an ordinary function, which answers `undefined`
    // here and falls through.
    if (Value kindProto = rtFunctionKindPrototype(obj); !kindProto.isUndefined()) {
        out = kindProto;
        return true;
    }
    // %Function.prototype% is itself a function object (20.2.3), and its own
    // [[Prototype]] is Object.prototype. Without this line the general rule
    // below would answer with the object being asked about, and a chain walk
    // over it would never terminate.
    out = bronze_strict_eq(obj.rawBits(), rtFunctionPrototypeObject().rawBits())
              ? rtObjectPrototype()
              : rtFunctionPrototypeObject();
    return true;
}

// Step 1 of 20.1.2.12 is ToObject, which is the whole reason a PRIMITIVE has an
// answer here at all: `Object.getPrototypeOf("x")` is String.prototype, and
// only `null` and `undefined` are the TypeError (7.1.18). All four primitive
// kinds with an intrinsic now answer from it, and the wrapper ToObject would
// build is still not built — [[GetPrototypeOf]] of one is the intrinsic
// whatever it wraps, so building it would allocate an object to read a constant
// off it.
uint64_t objectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        if (args[0].isNull() || args[0].isUndefined()) {
            return rtThrowTypeError("Object.getPrototypeOf called on " +
                                    std::string(args[0].isNull() ? "null" : "undefined"))
                .rawBits();
        }
        if (args[0].isString()) return rtStringPrototype().rawBits();
        if (args[0].isBool()) return rtBooleanPrototype().rawBits();
        if (args[0].isNumber()) return rtNumberPrototype().rawBits();
        if (args[0].isSymbol()) return rtSymbolPrototype().rawBits();
        // 10.4.6.1 fixes a module namespace's [[Prototype]] at null, and it is
        // immutable — so this is the language's own answer and not the "no
        // prototype object exists" that an array's `null` would have been.
        if (rtIsModuleNamespace(args[0])) return Value::fromNull().rawBits();
        // An array's and a function's [[Prototype]] is not stored anywhere —
        // neither header carries a shape — but it is not unknown either: it is
        // the intrinsic, fixed by 23.1.6.1 and 20.2.3, and bronze builds both
        // objects. So the answer is the intrinsic, and what an array's chain
        // does NOT do (walk through it — an array's members are answered beside
        // the value) is a separate fact, already refused by name where a
        // program could act on it: decorating Array.prototype.
        if (Value proto; rtShapelessPrototypeOf(args[0], proto)) return proto.rawBits();
        if (args[0].isObject() &&
            args[0].asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 10.5.1 [[GetPrototypeOf]]: the `getPrototypeOf` trap, or the
            // target's prototype. A revoked proxy throws here like everywhere.
            return rtProxyGetPrototypeOf(args[0]).rawBits();
        }
        // What is left is a kind whose prototype IS an intrinsic bronze has
        // never built as an object: `RegExp.prototype` is answered by the
        // property path from a C table beside the value, so there is no object
        // to hand back and `null` would be a lie about a chain that works.
        if (args[0].isObject() &&
            args[0].asObject<HeapObjectHeader>()->flags == HeapKind::RegExp) {
            return rtRegExpPrototypeObject().rawBits();
        }
        return rtThrowTypeError(std::string("unsupported: Object.getPrototypeOf of ") +
                                rtObjectKindName(args[0]))
            .rawBits();
    }
    Shape* shape = args[0].asObject<ObjectHeader>()->shape;
    const Value proto = shape ? shape->prototypeValue() : Value::fromUndefined();
    if (proto.isObject() || proto.isNull()) return proto.rawBits();
    // A plain object whose root shape carries `undefined` rather than a
    // prototype: every route to one now names either an object or null, so this
    // is a bronze bug and not a program's doing.
    fatal("internal: a plain object whose root shape names no prototype");
}

// A function's own keys, for the one question that does not need them written
// down: does this key name one? LISTING them is what needs somewhere to put
// them, and testing for one does not.
//
// The three that are not in the statics table: `prototype` (10.2.4, in its own
// slot and materialised lazily), and `length` and `name` (10.2.10, 10.2.9, in
// the header). `prototype` is reported for every function, which is bronze's
// existing answer to `'prototype' in f` — an arrow function has none in
// ECMA-262, and that divergence is the property path's and not this member's.
bool functionHasOwnKey(Rooted<Value>& fnVal, Value key) {
    if (!key.isSymbol()) {
        const std::string text = rtObjectKeyTextOf(key);
        if (rtExceptionPending()) return false;
        if (text == "prototype") return true;
        if ((text == "length" || text == "name") &&
            fnVal.get().asObject<FunctionHeader>()->name != nullptr) {
            return true;
        }
    }
    // Everything else — a `static` member, a symbol-keyed one — is in the side
    // object, which is an ordinary shape and answers with the ordinary walk.
    Value props = fnVal.get().asObject<FunctionHeader>()->properties;
    if (!props.isObject()) return false;
    Rooted<Value> propsRoot{props};
    return rtHasOwnPropertyNamed(propsRoot, key);
}

// 20.1.2.13 Object.hasOwn(O, P) — `hasOwnProperty` with the receiver moved into
// the argument list, and the reason the method form is not the idiom: the
// method can be shadowed by an own property of the object being asked about.
uint64_t objectHasOwn(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    switch (rtObjectOwnKeysOf(args[0], "hasOwn")) {
        case ObjectOwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::None:
            return Value::fromBool(false).rawBits();
        case ObjectOwnKeys::Array:
        case ObjectOwnKeys::TypedArray:
        case ObjectOwnKeys::RegExp:
        case ObjectOwnKeys::Proxy: {
            // The one [[GetOwnProperty]] `hasOwnProperty` asks
            // (builtin_object_proto.cpp): an element, `length`, a RegExp's
            // `lastIndex`, or a named or symbol-keyed property of the side
            // object — or, for a proxy, the `getOwnPropertyDescriptor` trap
            // (10.5.5). Asked there rather than answered here so the two
            // spellings cannot drift.
            Rooted<Value> self{args[0]};
            bool enumerable = false;
            const bool own = rtOwnPropertyOf(self, args[1], enumerable);
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            return Value::fromBool(own).rawBits();
        }
        case ObjectOwnKeys::Function: {
            Rooted<Value> fn{args[0]};
            return Value::fromBool(functionHasOwnKey(fn, args[1])).rawBits();
        }
        case ObjectOwnKeys::StringChars: {
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                if (rtExceptionPending()) return Value::fromUndefined().rawBits();
                // args[0] re-read through RootedArgs: `rtObjectKeyTextOf` allocates.
                Value data = args[0];
                if (!data.isString()) rtStringWrapperData(args[0], data);
                if (rtStringDataHasOwnKey(data, key)) return Value::fromBool(true).rawBits();
            }
            // A PRIMITIVE string's own keys are the indices and `length` and
            // nothing else (10.4.3.3), so the miss is the answer. A String
            // exotic OBJECT also has a shape — `String.prototype` keeps every
            // string method in one — and 10.4.3.1 step 3 hands the key that the
            // characters did not answer to OrdinaryGetOwnProperty, which is the
            // walk below.
            if (!args[0].isObject()) return Value::fromBool(false).rawBits();
            break;
        }
        case ObjectOwnKeys::Namespace: {
            // The exports are the complete list of own keys (10.4.6.2), and
            // "is this an own property" is the same question 10.4.6.5 answers —
            // asked through the same helper, so the two cannot drift.
            Value ignored;
            return Value::fromBool(rtModuleNamespaceOwnProperty(args[0], args[1], ignored))
                .rawBits();
        }
        case ObjectOwnKeys::Shape:
            break;
    }
    Rooted<Value> self{args[0]};
    return Value::fromBool(rtHasOwnPropertyNamed(self, args[1])).rawBits();
}

// 20.1.2.14 Object.is — SameValue (7.2.11), which differs from `===` in exactly
// two places and exists for them: NaN is the same value as itself, and +0 is
// not the same value as -0.
uint64_t objectIs(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Value a = args[0];
    const Value b = args[1];
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber();
        const double y = b.asNumber();
        if (x != x && y != y) return Value::fromBool(true).rawBits();  // both NaN
        // Zeroes differ by sign only, which compares equal as doubles; the bit
        // patterns are what SameValue distinguishes, so they are what is
        // compared. Not a raw rawBits() compare for every pair of numbers,
        // because a NaN has many encodings and two of them are the same value.
        if (x == 0.0 && y == 0.0) {
            return Value::fromBool(std::signbit(x) == std::signbit(y)).rawBits();
        }
        return Value::fromBool(x == y).rawBits();
    }
    // Every other kind: SameValue is SameValueNonNumber (7.2.12), which agrees
    // with strict equality everywhere it is defined — the two clauses differ
    // only over numbers, which the branch above has already taken.
    return Value::fromBool(bronze_strict_eq(a.rawBits(), b.rawBits())).rawBits();
}

// Does `proto` name the prototype this receiver already has? Asked only of the
// kinds that carry no shape, so that a [[SetPrototypeOf]] which changes nothing
// can succeed the way 10.1.2 step 2 says it does, without anything having to be
// stored. False for a kind whose prototype bronze cannot name at all — the
// caller refuses those, which is the honest answer for "I cannot tell".
bool rtSamePrototypeAsCurrent(Value obj, Value proto) {
    Value current;
    if (!rtShapelessPrototypeOf(obj, current)) return false;
    return bronze_strict_eq(proto.rawBits(), current.rawBits());
}

// 20.1.2.21 step 4, in one place: every false [[SetPrototypeOf]] answers —
// a non-extensible object (10.1.2.1 step 4), a chain that would loop (step
// 8.c.ii), a proxy trap that refused (10.5.2 step 9) — reach it, and none of
// them is reported differently for the storage the receiver keeps its
// prototype in.
uint64_t refuseInextensiblePrototype() {
    return rtThrowTypeError("Cannot set the prototype: the object is not extensible, the "
                            "chain would be cyclic, or a proxy trap refused")
        .rawBits();
}

bool rtObjectSetPrototypeOfOrdinary(Rooted<Value>& self, Rooted<Value>& proto) {
    if (!isPlainObject(self.get())) {
        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 10.5.2 [[SetPrototypeOf]]: the `setPrototypeOf` trap, or the
            // target's own, checked against the target's extensibility.
            return rtProxySetPrototypeOf(self.get(), proto.get());
        }
        // 10.1.2 OrdinarySetPrototypeOf step 2: a write of the prototype the
        // object ALREADY has succeeds and changes nothing. That is the whole
        // of `Object.setPrototypeOf(arr, Array.prototype)`, a defensive idiom
        // real code writes, and answering it needs no storage — only the
        // getter above, which now knows what an array's and a function's
        // prototype is.
        if (rtSamePrototypeAsCurrent(self.get(), proto.get())) return true;
        if (!rtIsExtensible(self.get())) return false;

        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            // Check for cycle (10.1.2 step 8)
            Value link = proto.get();
            for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
                if (link.rawBits() == self.get().rawBits()) return false;
                if (link.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                    Value next = link.asObject<FunctionHeader>()->parent;
                    link = next.isUndefined() ? rtFunctionPrototypeObject() : next;
                } else if (isPlainObject(link)) {
                    Shape* shape = link.asObject<ObjectHeader>()->shape;
                    link = shape ? shape->prototypeValue() : Value::fromNull();
                } else {
                    break;
                }
            }

            self.get().asObject<FunctionHeader>()->parent = proto.get();

            // Link statics/properties prototype chain so derived constructor inherits base statics
            rtEnsureFunctionProperties(self);
            Rooted<Value> selfProps{self.get().asObject<FunctionHeader>()->properties};

            if (proto.get().isObject() &&
                proto.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                rtEnsureFunctionProperties(proto);
                Rooted<Value> protoProps{proto.get().asObject<FunctionHeader>()->properties};
                Shape* newRoot = rtRootShapeForPrototype(protoProps.get());
                ObjectHeader::setPrototype(rtArena(), selfProps, newRoot);
            } else if (proto.get().isObject() || proto.get().isNull()) {
                Shape* newRoot = rtRootShapeForPrototype(proto.get());
                ObjectHeader::setPrototype(rtArena(), selfProps, newRoot);
            }
            return true;
        }

        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
            // Check for cycle (10.1.2 step 8)
            Value link = proto.get();
            for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
                if (link.rawBits() == self.get().rawBits()) return false;
                Value next;
                if (rtShapelessPrototypeOf(link, next)) {
                    link = next;
                } else if (isPlainObject(link)) {
                    Shape* shape = link.asObject<ObjectHeader>()->shape;
                    link = shape ? shape->prototypeValue() : Value::fromNull();
                } else {
                    break;
                }
            }

            if (proto.get().isObject()) {
                if (ObjectHeader* protoObj = proto.get().asObject<ObjectHeader>(); protoObj->shape) {
                    protoObj->shape->used_as_prototype = true;
                }
            }

            Shape* newRoot = rtRootShapeForPrototype(proto.get());
            if (!self.get().asObject<ArrayHeader>()->properties.isObject()) {
                ObjectHeader* box = ObjectHeader::create(rtHeap(), rtArena(), newRoot);
                box->header.flags = HeapKind::Plain;
                self.get().asObject<ArrayHeader>()->properties = Value::fromObject(box);
            } else {
                Rooted<Value> props{self.get().asObject<ArrayHeader>()->properties};
                ObjectHeader::setPrototype(rtArena(), props, newRoot);
            }
            self.get().asObject<ArrayHeader>()->reserved |= ArrayHeader::kHasCustomPrototype;
            return true;
        }

        return false;
    }
    // 10.1.2.1 OrdinarySetPrototypeOf steps 2 to 4, in that order. Step 2's
    // SameValue comes FIRST, so a write of the prototype the object already has
    // succeeds on a non-extensible object too — it stores nothing, and there is
    // nothing for [[Extensible]] to forbid. Any other write needs the object to
    // be extensible, and step 4's `false` is what 20.1.2.21 step 4 turns into a
    // TypeError. Without this a `preventExtensions`'d object's chain could be
    // replaced outright: the write landed, and `Object.isExtensible` went on
    // reporting false about an object whose prototype had just moved.
    {
        const uint64_t call[1] = {self.get().rawBits()};
        const Value current(objectGetPrototypeOf(0, 0, 1, call));
        if (bronze_strict_eq(proto.get().rawBits(), current.rawBits())) return true;
    }
    if (!rtIsExtensible(self.get())) return false;
    // Step 8: a chain that would loop back to the object is refused, walked
    // over ordinary links only — step 8.c.i stops the walk at a link whose
    // [[GetPrototypeOf]] is not the ordinary one, a proxy being the case.
    {
        Value link = proto.get();
        for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
            if (link.rawBits() == self.get().rawBits()) return false;
            if (!isPlainObject(link)) break;
            Shape* shape = link.asObject<ObjectHeader>()->shape;
            link = shape ? shape->prototypeValue() : Value::fromNull();
        }
    }
    Shape* newRoot = rtRootShapeForPrototype(proto.get());
    ObjectHeader::setPrototype(rtArena(), self, newRoot);
    return true;
}

// 20.1.2.21 Object.setPrototypeOf. Returns the object, so it composes.
uint64_t objectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[1].isObject() && !args[1].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    if (args[0].isNull() || args[0].isUndefined()) {
        return rtThrowTypeError("Object.setPrototypeOf called on null or undefined").rawBits();
    }
    if (!args[0].isObject()) return args[0].rawBits();  // 20.1.2.21 step 3
    Rooted<Value> self{args[0]};
    Rooted<Value> proto{args[1]};
    const bool ok = rtObjectSetPrototypeOfOrdinary(self, proto);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    if (!ok) return refuseInextensiblePrototype();
    return self.get().rawBits();
}

// 20.1.2.2 Object.create. `null` really means no prototype, and every walk
// over a prototype chain already stops at a shape whose prototype is not an
// object — so an object with no prototype needs no special case anywhere,
// which is the point of putting the prototype on the shape.
uint64_t objectCreate(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isObject() && !args[0].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    // A PROXY may be the prototype: every walk that misses stops at the link
    // and hands the rest of the operation to the proxy's internal method with
    // the receiver it had (object.cpp's read and write walks, rt_operator.cpp's
    // `in`, rt_enumerate.cpp's for-in), which is what 10.1.8.1 step 3 and its
    // siblings say happens at a link whose [[Get]] is not the ordinary one.
    if (args[0].isObject() && !isPlainObject(args[0]) &&
        args[0].asObject<HeapObjectHeader>()->flags != HeapKind::Proxy) {
        // THROWN rather than fatal, and the difference is who the message is
        // for. It is still bronze's gap and not the program's — 20.1.2.2 admits
        // any object — but the receiver is a value the program was holding, so
        // a reflective walk that reaches one (`Object.create(SomeClass)`,
        // `Object.create(arr)`) must be able to carry on or report it, not take
        // the process down. The reason stays in the message.
        return rtThrowTypeError(
                   std::string("Object.create with ") + rtObjectKindName(args[0]) +
                   " as the prototype is unsupported (a prototype is walked by every read "
                   "that misses, and this kind answers its members from a table beside the "
                   "value rather than from a shape a walk can step through; only a plain "
                   "object may be one)")
            .rawBits();
    }
    Rooted<Value> proto{args[0]};
    Rooted<Value> out{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(proto.get())))};
    out.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    if (!args[1].isUndefined()) {
        Rooted<Value> descriptors{args[1]};
        rtObjectDefineFromDescriptors(out, descriptors);
    }
    return out.get().rawBits();
}

// 20.1.2.10 Object.getOwnPropertyNames: own string keys in the same order
// `keys` reports, MINUS the enumerable filter — which is the only difference,
// and is one argument to one walk rather than a second walk.
//
// Exported across the seam (builtin_object.h) because 20.1.2.9 step 2 is a loop
// over exactly this list, and `getOwnPropertyDescriptors` is on the other side
// of it.
uint64_t rtObjectGetOwnPropertyNames(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    switch (rtObjectOwnKeysOf(args[0], "getOwnPropertyNames")) {
        case ObjectOwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::None:
            return bronze_create_array(0);
        case ObjectOwnKeys::Array:
            // 10.4.2's own order — the indices ascending, `length`, then the
            // named properties — and `length` is here where `Object.keys`
            // drops it, for the same reason as the string arm below.
            return rtArrayOwnKeyNames(args[0], /*enumerableOnly=*/false).rawBits();
        case ObjectOwnKeys::StringChars:
            // 10.4.3.3's own order — the indices ascending and THEN `length` —
            // and `length` is here where `Object.keys` drops it, because this
            // member is OwnPropertyKeys without the enumerable filter.
            return rtStringOwnKeyNames(args[0], /*enumerableOnly=*/false).rawBits();
        case ObjectOwnKeys::Namespace:
            // Every export is enumerable (10.4.6.5), so dropping the filter
            // changes nothing and this is the same list `Object.keys` gives —
            // answered by the same function, so the two cannot drift.
            return bronze_object_keys(args[0].rawBits());
        case ObjectOwnKeys::Proxy: {
            // 10.5.11 [[OwnPropertyKeys]] — the `ownKeys` trap's list, in the
            // trap's order — with 20.1.2.10's string filter over it. The trap
            // runs once; its symbols are `getOwnPropertySymbols`' half.
            Rooted<Value> proxy{args[0]};
            Rooted<Value> keys{rtProxyOwnKeys(proxy.get())};
            if (rtExceptionPending()) return Value::fromUndefined().rawBits();
            Rooted<Value> out{Value(bronze_create_array(0))};
            uint32_t at = 0;
            const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
            for (uint32_t i = 0; i < count; ++i) {
                Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
                if (!key.get().isString()) continue;
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
            }
            return out.get().rawBits();
        }
        case ObjectOwnKeys::Function: {
            // 10.2.4 and 10.2.9/10.2.10: an ordinary function's own properties
            // are `length`, `name` and — where its syntax gave it one —
            // `prototype`, none of them enumerable, and THEN whatever the
            // program assigned as a static. All three come first because
            // 6.1.7.1 orders string keys by creation and
            // OrdinaryFunctionCreate makes them before any assignment can run.
            //
            // `length` and `name` are reported when the header carries them —
            // every function bronze compiled and every native built through a
            // named `rtNativeFunction` — and not for the nameless form, whose
            // reads answer "" and 0 without there being a property to list.
            Rooted<Value> fn{args[0]};
            // A global constructor's statics are answered BESIDE the function
            // from a C table until something needs them written down — which a
            // listing does. The installer is idempotent and writes the same
            // interned objects the read path hands out, so materialising here
            // changes what is listed and nothing about what is read.
            rtInstallGlobalConstructorStatics(fn);
            Rooted<Value> out{Value(bronze_create_array(0))};
            uint32_t at = 0;
            const FunctionHeader* header = fn.get().asObject<FunctionHeader>();
            const bool named = header->name != nullptr;
            const bool hasPrototype = header->hasPrototypeProperty();
            for (const char* builtinName : {"length", "name", "prototype"}) {
                const bool present = builtinName[0] == 'p' ? hasPrototype : named;
                if (!present) continue;
                Rooted<Value> key{rtMakeString(builtinName)};
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
            }
            Value props = fn.get().asObject<FunctionHeader>()->properties;
            if (props.isObject()) {
                Rooted<Value> propsRoot{props};
                const uint64_t call[1] = {propsRoot.get().rawBits()};
                Rooted<Value> statics{Value(rtObjectGetOwnPropertyNames(0, 0, 1, call))};
                const uint32_t count = statics.get().asObject<ArrayHeader>()->length;
                for (uint32_t i = 0; i < count; ++i) {
                    Rooted<Value> key{statics.get().asObject<ArrayHeader>()->getElem(i)};
                    out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
                }
            }
            return out.get().rawBits();
        }
        case ObjectOwnKeys::TypedArray:
        case ObjectOwnKeys::RegExp:
        case ObjectOwnKeys::Shape:
            break;
    }
    Rooted<Value> self{args[0]};
    // 20.1.2.10 is the STRING half of OwnPropertyKeys — the symbol half is
    // `getOwnPropertySymbols`, and the two are separate functions in the
    // language precisely so that neither ever reports the other's keys.
    const std::vector<StringHeader*> ordered =
        rtOwnStringKeysOrdered(self.get().asObject<ObjectHeader>(), /*enumerableOnly=*/false);
    // 10.4.5.7: a typed array's elements come first, every index within the
    // length ascending, ahead of the string keys its shape holds.
    const uint16_t kind = self.get().asObject<HeapObjectHeader>()->flags;
    const uint32_t elements =
        kind == HeapKind::TypedArray ? self.get().asObject<TypedArrayHeader>()->length : 0;
    // 22.2.3.2: `lastIndex` is defined by RegExpAlloc before the program can
    // write a key, so by 6.1.7.1's creation order it is listed first.
    const bool lastIndex = kind == HeapKind::RegExp;
    Rooted<Value> out{Value(bronze_create_array(elements + (lastIndex ? 1u : 0u) +
                                                static_cast<uint32_t>(ordered.size())))};
    uint32_t at = 0;
    for (uint32_t i = 0; i < elements; ++i) {
        Rooted<Value> key{rtMakeString(std::to_string(i))};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    if (lastIndex) {
        Rooted<Value> key{rtMakeString("lastIndex")};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    for (StringHeader* name : ordered) {
        Rooted<Value> key{rtKeyAsValue(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }
    return out.get().rawBits();
}

namespace {

using NamespaceFn = NativeMethod;

const NamespaceFn kObjectFunctions[] = {
    {"keys", objectKeys, 1, 1},
    {"values", objectValues, 1, 1},
    {"entries", objectEntries, 1, 1},
    // Padded to 0 (variadic) and 20.1.2.1's length 2: the one row where the
    // two columns differ most visibly.
    {"assign", objectAssign, 0, 2},
    {"fromEntries", objectFromEntries, 1, 1},
    {"defineProperty", rtObjectDefineProperty, 3, 3},
    {"getOwnPropertyDescriptor", rtObjectGetOwnPropertyDescriptor, 2, 2},
    {"defineProperties", rtObjectDefineProperties, 2, 2},
    {"freeze", rtObjectFreeze, 1, 1},
    {"isFrozen", rtObjectIsFrozen, 1, 1},
    {"seal", rtObjectSeal, 1, 1},
    {"isSealed", rtObjectIsSealed, 1, 1},
    {"preventExtensions", rtObjectPreventExtensions, 1, 1},
    {"isExtensible", rtObjectIsExtensible, 1, 1},
    {"create", objectCreate, 2, 2},
    {"getPrototypeOf", objectGetPrototypeOf, 1, 1},
    {"setPrototypeOf", objectSetPrototypeOf, 2, 2},
    {"getOwnPropertyNames", rtObjectGetOwnPropertyNames, 1, 1},
    {"getOwnPropertyDescriptors", rtObjectGetOwnPropertyDescriptors, 1, 1},
    {"hasOwn", objectHasOwn, 2, 2},
    {"is", objectIs, 2, 2},
    {"getOwnPropertySymbols", objectGetOwnPropertySymbols, 1, 1},
    // 20.1.2.13, whose body is 7.3.35 shared with `Map.groupBy`
    // (builtin_group_by.cpp).
    {"groupBy", rtObjectGroupBy, 2, 2},
};

// Real members of `Object` that bronze has not built. `groupBy` was the last
// entry and left when 20.1.2.13 landed, so the list is empty — spelled as a
// named empty rather than deleted, because the check it feeds is what turns
// the next member of 20.1.2 bronze has not built from a silent `undefined`
// into a refusal by name.
const char* const* const kObjectUnimplemented = nullptr;
constexpr size_t kObjectUnimplementedCount = 0;

thread_local Value g_objectNamespace = Value::fromUndefined();
thread_local Value g_objectPrototype = Value::fromUndefined();

// Both intrinsics, built together.
//
// They reference each other — `Object.prototype` is a property of the
// constructor and `Object.prototype.constructor` is the constructor — so neither
// can be built by an accessor that lazily builds the other: whichever ran first
// would re-enter the second, which would re-enter the first. So the two globals
// are PUBLISHED AS SOON AS EACH OBJECT EXISTS and the decoration follows, which
// makes the cycle a pair of ordinary assignments.
//
// Publishing early is not tidiness. `Object` is a function object now, and a
// function's statics live in a side object built from `rtPlainObjectShape()` —
// which names %Object.prototype% and so calls back into here. With the
// assignment at the END of this function that call re-entered, found the guard
// clear, and recursed until the stack ran out: every program that so much as
// mentioned `Object` died at startup. The guard reads `g_objectPrototype`, so
// that one is set the moment its object exists.
//
// `constructor` matters more than it looks. Without it `({}).constructor` is
// `undefined`, which is a silent wrong answer, and one that would appear or
// disappear depending on whether the program had mentioned `Object` anywhere.
void ensureObjectIntrinsics() {
    if (g_objectPrototype.isObject()) return;

    // Object.prototype's own prototype is NULL, and it is the one object in the
    // program for which that is true by definition rather than by request
    // (20.1.3: "the value of [[Prototype]] is null"). It must not come from
    // `rtPlainObjectShape`, which is about to name THIS object as its
    // prototype — that would be the chain closing on itself.
    Rooted<Value> proto{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(Value::fromNull())))};
    proto.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    g_objectPrototype = proto.get();
    rtHeap().add_permanent_root(&g_objectPrototype);

    // 20.1.1 makes `Object` a CONSTRUCTOR, not a namespace: it is callable, it
    // is `new`able, and its statics are the own properties of a function object.
    // So it is a function singleton interned on the body's code pointer — the
    // same mechanism every other global constructor uses (builtin_constructors.cpp
    // says why one distinct C function per constructor is load-bearing) — and
    // its statics go in the side object every function keeps them in. 20.1.1
    // gives it the name "Object" and length 1.
    Rooted<Value> ns{rtNativeFunction(objectConstructorBody, 0, "Object", 1)};
    {
        FunctionHeader* live = ns.get().asObject<FunctionHeader>();
        // 20.1.2.1: `Object.prototype` is this function's `prototype` slot, and
        // filling it here is what stops the first `new Object()` from minting a
        // fresh empty object through `rtEnsureFunctionPrototype` — the same trap
        // `Array` names in builtin_constructors.cpp. The instance shape is left
        // to that function, which builds one from a prototype already present.
        live->prototype = proto.get();
        // 20.1.2.1: `{ [[Writable]]: false, [[Enumerable]]: false,
        // [[Configurable]]: false }`. Set on `Object` alone in this chunk and
        // not on the rest of `kCtors`, whose clauses say the same thing —
        // `Array.prototype = x` still lands, and that is a known gap rather
        // than a decision this line makes.
        live->prototype_readonly = true;
    }
    g_objectNamespace = ns.get();
    rtHeap().add_permanent_root(&g_objectNamespace);
    rtEnsureFunctionProperties(ns);

    for (const NamespaceFn& fn : kObjectFunctions) {
        Rooted<Value> key{rtMakeString(fn.name)};
        Rooted<Value> val{rtNativeFunction(fn.code, fn.arity, fn.name, fn.length)};
        Rooted<Value> holder{ns.get().asObject<FunctionHeader>()->properties};
        holder.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    // The prototype's own members come from builtin_object_proto.cpp, which owns
    // every function whose subject is `this` rather than an argument. They are
    // installed from here because 20.1.2.1 and 20.1.3.1 make these two objects
    // each other's property, so one initializer has to hold both.
    rtInstallObjectProtoMethods(proto);

    // One cross-reference to write: `Object.prototype` is the function's own
    // slot, filled above, and 20.1.3.1's `constructor` is the other direction.
    {
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ns, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }

    // Both globals were published above, each as soon as its object existed.
}

}  // namespace

// The symbol half, across the seam. `Reflect.ownKeys` is defined over BOTH
// halves of 6.1.7.1 and lives in rt_reflect.cpp, so the one function that
// knows where a receiver keeps its symbol keys is reached rather than copied.
uint64_t rtObjectGetOwnPropertySymbols(uint64_t a, uint64_t b, uint32_t argc,
                                       const uint64_t* argv) {
    return objectGetOwnPropertySymbols(a, b, argc, argv);
}

Value rtObjectNamespace() {
    ensureObjectIntrinsics();
    return g_objectNamespace;
}

Value rtObjectPrototype() {
    ensureObjectIntrinsics();
    return g_objectPrototype;
}

bool rtObjectCheckMissingMember(Value obj, const std::string& key) {
    if (!g_objectNamespace.isObject() || obj.rawBits() != g_objectNamespace.rawBits()) return false;
    rtCheckUnimplementedMember("Object", kObjectUnimplemented, kObjectUnimplementedCount, key);
    return true;
}

}  // namespace bronze::runtime

extern "C" uint64_t bronze_get_prototype_of(uint64_t objBits) {
    uint64_t argv[1] = {objBits};
    return bronze::runtime::objectGetPrototypeOf(0, 0, 1, argv);
}
