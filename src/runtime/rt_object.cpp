// Object, array and function construction, the two class links, environment
// records, and the dynamic call path. Property access itself is rt_prop.cpp;
// typed arrays build themselves through ordinary constructor objects and so
// need nothing here.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/promise.h"
#include "runtime/regexp.h"
#include "runtime/builtin_object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

// The kind of a value, named. Diagnostics print this rather than raw bits:
// "attempted to call undefined" bisects to a construct, `fff6000000000000`
// bisects to nothing.
static const char* valueKindName(Value v) {
    if (v.isNumber() || v.isInt32()) return "a number";
    if (v.isString()) return "a string";
    if (v.isBool()) return "a boolean";
    if (v.isNull()) return "null";
    if (v.isUndefined()) return "undefined";
    if (v.isHole()) return "the internal hole sentinel";
    if (v.isSymbol()) return "a symbol";
    if (v.isObject()) {
        switch (v.asObject<HeapObjectHeader>()->flags) {
            case 1: return "an array";
            case 2: return "a function";
            case TypedArrayHeader::kFlags:
                return v.asObject<TypedArrayHeader>()->kindName();
            case ArrayBufferHeader::kFlags: return "an ArrayBuffer";
            case DataViewHeader::kFlags: return "a DataView";
            default: return "an object";
        }
    }
    return "a value of an unknown kind";
}

static std::string_view latin1View(const StringHeader* s) {
    return std::string_view(s->latin1Data(), s->getLength());
}

// ECMA-262 10.2.5 MakeConstructor step 6: the object a constructor hands its
// instances carries a back-pointer to the constructor. Non-enumerable,
// because a `for-in` over an instance must not visit it and `Object.keys` on
// the prototype must not report it (the attributes 10.2.5 spells out).
//
// It is the link `new this.constructor()` reads — the prototype-style way to
// clone an object without naming its class, and three.js's `Box3.clone`,
// `BufferAttribute` and `AnimationUtils` all do exactly that. Without it the
// expression is `undefined is not a constructor`, which is a hard error but a
// wrong one: the language says the property is there.
//
// Assigning `Foo.prototype = {...}` afterwards drops it, which is also what
// the language says: the object the program supplied has whatever it has.
static void rtInstallPrototypeConstructor(Rooted<Value>& fnVal) {
    Rooted<Value> protoRoot{fnVal.get().asObject<FunctionHeader>()->prototype};
    Rooted<Value> key{rtMakeString("constructor")};
    // A DEFINITION: a base prototype's `constructor` must not be found and
    // written through when a derived prototype defines its own.
    protoRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, fnVal,
                                                      /*ic=*/nullptr, /*enumerable=*/false,
                                                      /*defineOwn=*/true);
}

// A function's `.prototype` serves both as a constructor's instance prototype
// and as the target of `Foo.prototype.m =...`, and those must be the same
// object, so both go through here.
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->prototype.isObject() && fn->instance_shape) return;

    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    proto->header.flags = HeapKind::Plain;

    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->prototype = Value::fromObject(proto);
    fn->instance_shape = rtNewRootShape(fn->prototype);
    rtInstallPrototypeConstructor(fnVal);
}

void rtEnsureFunctionProperties(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->properties.isObject()) return;
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    props->header.flags = HeapKind::Plain;
    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->properties = Value::fromObject(props);
}

// ECMA-262 6.1.7.1 OwnPropertyKeys, in one place because six callers need the
// same answer and a second copy of the integer-first split would be a second
// chance to get `"10"` before `"2"` wrong. Enumerable-only by default because
// most callers — `Object.keys`, object spread, object rest and `for-in` — are
// defined over own enumerable keys, and a class method is not one.
//
// The three groups are the specification's, and the ORDER between them is
// pinned rather than incidental: integer-like keys ascending, then the
// remaining string keys in creation order, then the symbol keys in creation
// order. Symbols last is a rule about the answer and not about how they are
// stored — a symbol-keyed property sits in the transition chain wherever it was
// added, and this is where 6.1.7.1's grouping is applied. Nothing here consults
// a hash table, so the order is a function of the program and not of an address.
std::vector<PropertyKey> rtOwnKeysOrdered(const ObjectHeader* obj, bool enumerableOnly) {
    // A String exotic object's own keys begin with the index properties
    // 10.4.3.4 synthesised from the wrapped characters, and bronze answers
    // those on the property path only. Minting a key for one here would have to
    // allocate — an arena key is interned from a heap string — in the middle of
    // a walk whose whole contract is that it does not, and this function is
    // handed a RAW header that an allocation would move. So it is refused by
    // name; reporting a String object as having no indices is the wrong answer
    // rather than the missing one.
    rtCheckStringExoticOwnKeys(Value::fromObject(obj), "enumerating");
    // Shape keys are arena-interned and immortal, so collecting them up front
    // is safe across whatever the caller allocates while walking them.
    Shape* shape = obj->shape;
    std::vector<PropertyKey> inserted =
        shape ? shape->ownKeysInInsertionOrder(enumerableOnly) : std::vector<PropertyKey>{};

    std::vector<std::pair<uint32_t, PropertyKey>> intKeys;
    std::vector<PropertyKey> strKeys;
    std::vector<PropertyKey> symKeys;
    for (PropertyKey k : inserted) {
        if (k.isSymbol()) {
            symKeys.push_back(k);
            continue;
        }
        uint32_t idx = 0;
        StringHeader* s = k.string();
        if (s->isLatin1() && rtIsIntegerLikeKey(latin1View(s), idx)) {
            intKeys.emplace_back(idx, k);
        } else {
            strKeys.push_back(k);
        }
    }
    std::sort(intKeys.begin(), intKeys.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<PropertyKey> ordered;
    ordered.reserve(intKeys.size() + strKeys.size() + symKeys.size());
    for (const auto& [idx, name] : intKeys) ordered.push_back(name);
    for (PropertyKey name : strKeys) ordered.push_back(name);
    for (PropertyKey name : symKeys) ordered.push_back(name);
    return ordered;
}

std::vector<StringHeader*> rtOwnStringKeysOrdered(const ObjectHeader* obj, bool enumerableOnly) {
    std::vector<StringHeader*> names;
    for (PropertyKey k : rtOwnKeysOrdered(obj, enumerableOnly)) {
        // Symbols are last, so this could stop at the first one — it does not,
        // because "the order happens to make the loop right" is how an
        // ordering change becomes a silently missing key.
        if (StringHeader* s = k.string()) names.push_back(s);
    }
    return names;
}

// An array index as the STRING that names it — the spelling ToPropertyKey gives
// it, and the only one an own-key answer may contain. `std::to_chars` rather
// than a stream for the reason every number that reaches output uses it:
// deterministic bytes with no locale in the path.
static Value indexName(uint32_t index) {
    char buf[16];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), index);
    return Value::fromString(
        StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)));
}

Value rtStringOwnKeyNames(Value strVal, bool enumerableOnly) {
    Rooted<Value> self{strVal};
    const uint32_t length = self.get().asString<StringHeader>()->getLength();
    const uint32_t total = enumerableOnly ? length : length + 1;
    Rooted<Value> out{Value(bronze_create_array(total))};
    for (uint32_t i = 0; i < length; ++i) {
        Rooted<Value> key{indexName(i)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, key);
    }
    if (!enumerableOnly) {
        Rooted<Value> key{rtMakeString("length")};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), length, key);
    }
    return out.get();
}

Value rtCopyKeyToHeap(const StringHeader* key) {
    if (key->isLatin1()) {
        return Value::fromString(
            StringHeader::createLatin1(rtHeap(), key->latin1Data(), key->getLength()));
    }
    return Value::fromString(
        StringHeader::createUTF16(rtHeap(), key->utf16Data(), key->getLength()));
}

extern "C" {

uint64_t bronze_create_object() {
    recordHelperCall("bronze_create_object");
    ObjectHeader* obj = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    obj->header.flags = HeapKind::Plain;
    return Value::fromObject(obj).rawBits();
}

uint64_t bronze_create_array(uint32_t length) {
    recordHelperCall("bronze_create_array");
    uint32_t cap = (length < 4) ? 4 : length;
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), cap);
    arr->length = length;
    return Value::fromObject(arr).rawBits();
}

uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity, uint32_t length,
                                uint32_t nameKey, uint64_t envBits) {
    recordHelperCall("bronze_create_function");
    Rooted<Value> env{Value(envBits)};
    FunctionHeader* fn = FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity);
    // Read the environment through the root only AFTER allocating: the
    // allocation above can collect, and a by-value copy taken before it would
    // point into dead from-space.
    fn->env_record = env.get();
    fn->header.flags = HeapKind::Function;
    // The key headers are arena-interned and immortal (rt_state.cpp), so this
    // is a pointer copy and not an allocation — which is what keeps a closure
    // created in a loop as cheap as it was before it carried a name.
    rtSetFunctionNameAndLength(fn, nameKey, length);
    return Value::fromObject(fn).rawBits();
}

// `class D extends B` — the two prototype links a class sets up, and the only
// runtime concept classes add. Instances: D.prototype's proto is B.prototype,
// so an inherited method is found by the ordinary chain walk. Statics: D's
// own-property object's proto is B's, so `D.staticOfB()` resolves the same way.
//
// D.prototype is REPLACED rather than mutated, because the prototype lives on
// the shape — which is also why this must run before any method is stored on
// it.
void bronze_class_extends(uint64_t derivedBits, uint64_t baseBits) {
    Value derivedVal(derivedBits);
    Value baseVal(baseBits);
    if (!derivedVal.isObject() ||
        derivedVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        fatal("internal: class extends with a non-function derived class");
    }
    if (!baseVal.isObject() || baseVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        fatal("a class can only extend another class or a constructor function");
    }
    // A native intrinsic cannot be a base, and saying so is the point. The
    // derived constructor builds an ordinary plain object and forwards to the
    // base, but `Array`'s body ignores the receiver and returns an array of its
    // own — so `new Sub()` would be a plain object that `Array.isArray` and
    // `instanceof Array` both call false while the program believes it made an
    // array. Refusing is loud where subclassing it is a silent wrong answer.
    if (const char* intrinsic = rtIntrinsicConstructorName(baseVal)) {
        fatal((std::string("extending the native constructor `") + intrinsic +
               "` is unsupported (its instances are built by the runtime, so a "
               "subclass would not be one)")
                  .c_str());
    }
    // `Promise` is refused for a reason of its own, and it is load-bearing
    // rather than defensive: the whole capability story in promise.h — no
    // @@species, no NewPromiseCapability over an arbitrary constructor, `then`
    // minting a plain intrinsic promise — rests on every promise in the
    // program BEING the intrinsic. A subclass's instances are exactly the
    // receiver that story has no answer for, and silently handing them
    // intrinsic capabilities would be a wrong answer rather than a missing one.
    if (rtIsPromiseConstructor(baseVal)) {
        fatal("extending `Promise` is unsupported (bronze's promises are the intrinsic and "
              "only the intrinsic: there is no @@species, so a subclass's `then` could not "
              "produce a subclass)");
    }

    Rooted<Value> derived{derivedVal};
    Rooted<Value> base{baseVal};
    rtEnsureFunctionPrototype(base);
    rtEnsureFunctionProperties(base);

    Rooted<Value> baseProto{base.get().asObject<FunctionHeader>()->prototype};
    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProto.get()));
    proto->header.flags = HeapKind::Plain;
    Rooted<Value> protoRoot{Value::fromObject(proto)};

    Rooted<Value> baseProps{base.get().asObject<FunctionHeader>()->properties};
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProps.get()));
    props->header.flags = HeapKind::Plain;

    FunctionHeader* fn = derived.get().asObject<FunctionHeader>();
    fn->prototype = protoRoot.get();
    fn->properties = Value::fromObject(props);
    fn->instance_shape = rtNewRootShape(protoRoot.get());
    // The replacement prototype needs its own back-pointer, or `D.prototype`
    // would inherit the BASE's and `new this.constructor()` on a derived
    // instance would build a base instance.
    rtInstallPrototypeConstructor(derived);
}

uint64_t bronze_construct(uint64_t fnBits, uint32_t argc, const uint64_t* argvBits) {
    recordCallSite("bronze_construct", fnBits);
    Value fnVal(fnBits);
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError(std::string(valueKindName(fnVal)) + " is not a constructor")
            .rawBits();
    }

    // A BOUND function constructs its TARGET (10.4.1.2): the bound arguments
    // are prepended, [[BoundThis]] is IGNORED — `new` supplies the receiver —
    // and the instance's prototype is the target's, which is why this unwraps
    // BEFORE the ordinary path below could materialise a prototype on the
    // bound function itself. One layer per recursion, so `f.bind(a).bind(b)`
    // flattens through the chain exactly as 10.4.1.2's delegation does.
    if (Value target, boundThis, boundArgs;
        rtBoundFunctionState(fnVal, target, boundThis, boundArgs)) {
        (void)boundThis;
        Rooted<Value> targetRoot{target};
        Rooted<Value> argsRoot{boundArgs};
        const uint32_t bound = argsRoot.get().asObject<ArrayHeader>()->length;
        // The combined block is a RootedBlock because the recursion allocates
        // before it reads its block — the exact contract the comment further
        // down names, satisfied the way bronze_construct_spread satisfies it.
        RootedBlock block(bound + argc);
        for (uint32_t i = 0; i < bound; ++i) {
            block.set(i, argsRoot.get().asObject<ArrayHeader>()->getElem(i));
        }
        for (uint32_t i = 0; i < argc; ++i) block.set(bound + i, Value(argvBits[i]));
        return bronze_construct(targetRoot.get().rawBits(), bound + argc, block.data());
    }

    // `new String(x)` and `new Boolean(x)` build the wrapper INSTEAD of the
    // ordinary instance, and cannot be left to run their bodies below: a native
    // constructor cannot see NewTarget through the uniform calling convention,
    // so its body returns the primitive, and the rule at the foot of this
    // function discards any non-object return in favour of the plain instance.
    // The program would receive `{}` — `new String("ab").length` read
    // `undefined` — which is why this was a named refusal before there was an
    // exotic object to hand back.
    if (Value wrapper; rtConstructPrimitiveWrapper(fnVal, argc, argvBits, wrapper)) {
        return wrapper.rawBits();
    }

    Rooted<Value> fnRoot{fnVal};
    rtEnsureFunctionPrototype(fnRoot);

    FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
    ObjectHeader* instance = ObjectHeader::create(rtHeap(), rtArena(), fn->instance_shape);
    instance->header.flags = HeapKind::Plain;

    Rooted<Value> self{Value::fromObject(instance)};
    // This helper is the one place in the runtime that ALLOCATES before it
    // reads its argument block — `rtEnsureFunctionPrototype` and the instance
    // above — so the block must already be rooted when it arrives, and "argv
    // points into the caller's frame" is not the reason, and on its own it
    // would not be enough. Two callers, and each satisfies it its own way:
    // generated code puts the block in its GC root frame, and
    // `bronze_construct_spread` builds a `RootedBlock`. A third caller has to
    // do one or the other.
    fn = fnRoot.get().asObject<FunctionHeader>();
    Value result = fn->call(self.get(), argc,
                            const_cast<Value*>(reinterpret_cast<const Value*>(argvBits)));

    // JS: a constructor returning an object replaces the instance; any other
    // return value (including undefined) is ignored.
    return result.isObject() ? result.rawBits() : self.get().rawBits();
}

// ECMA-262 20.1.2.17 Object.keys: own ENUMERABLE STRING keys, which is 7.3.23
// EnumerableOwnProperties with key-of-type-String.
//
// Every receiver gets an answer here or is named, and the difference between
// those two is not how much bronze has built — it is whether the answer is
// COMPLETE. A Map, a Set, a RegExp, an ArrayBuffer and a DataView have no own
// enumerable string-keyed property at all, so the empty array is derivable
// rather than a gap dressed up as a result; a typed array's indices ARE own
// enumerable properties (10.4.5.3), so it answers those and not `[]`. The old
// message said which receivers were "supported", which names bronze's coverage
// where the reader needs the receiver's storage.
uint64_t bronze_object_keys(uint64_t objBits) {
    recordHelperCall("bronze_object_keys");
    Value objVal(objBits);
    // Step 1 is ToObject, whose only two failures are these (7.1.18). Thrown
    // rather than fatal: the language names this TypeError, so a `catch` may
    // hold it.
    if (objVal.isNull() || objVal.isUndefined()) {
        return rtThrowTypeError("Object.keys called on a value that is not an object").rawBits();
    }
    // ToObject("ab") is a String exotic object whose own keys are the indices
    // and `length` (10.4.3.3) — and only the indices are enumerable, so those
    // are the answer. Computed from the characters rather than from a box built
    // to be read once and thrown away, which is the arrangement
    // `Object.getPrototypeOf` of a primitive already uses.
    if (objVal.isString()) return rtStringOwnKeyNames(objVal, /*enumerableOnly=*/true).rawBits();
    // A number, a boolean and a symbol box to an object with no own property of
    // any kind, so the empty answer needs no box either — which is what lets
    // `Object.keys(5)` answer at all, since bronze has no Number.prototype for
    // one to point at.
    if (!objVal.isObject()) return bronze_create_array(0);

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // An array's own keys are its indices, already in ascending order — the
    // ones it actually HAS: a hole left by `delete a[i]` is not an own
 // property, so the result is shorter than `length`.
    if (hdr->flags == HeapKind::Array) {
        Rooted<Value> src{objVal};
        uint32_t length = reinterpret_cast<ArrayHeader*>(hdr)->length;
        Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), length ? length : 4))};
        uint32_t at = 0;
        for (uint32_t i = 0; i < length; ++i) {
            if (!src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            Rooted<Value> key{indexName(i)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        // Then the named ones — the indices come first because they are
        // integer-like keys, and own-enumerable order puts those ahead of the
        // rest. The keys are arena-interned and immortal, so the vector
        // survives the allocations the copy below makes.
        for (StringHeader* k : rtArrayOwnNamedKeys(src.get())) {
            Rooted<Value> key{rtCopyKeyToHeap(k)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    // A typed array's integer-indexed elements are own enumerable properties
    // (10.4.5.3 [[DefineOwnProperty]] gives one `enumerable: true`), so this is
    // the one receiver below whose answer is not empty — and the reason the
    // whole group could not be answered with `[]` and a comment. There are no
    // holes: 23.2.5.1 allocates every element, so the keys are exactly
    // `0..length-1`. `length`, `buffer` and `byteOffset` are accessors on
    // %TypedArray%.prototype and own properties of nothing.
    if (hdr->flags == TypedArrayHeader::kFlags) {
        const uint32_t length = reinterpret_cast<TypedArrayHeader*>(hdr)->length;
        Rooted<Value> out{Value(bronze_create_array(length))};
        for (uint32_t i = 0; i < length; ++i) {
            Rooted<Value> key{indexName(i)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), i, key);
        }
        return out.get().rawBits();
    }
    // A module namespace: 10.4.6.2's export names, SORTED by code unit, and
    // every one of them enumerable (10.4.6.5), so `Object.keys` and
    // `getOwnPropertyNames` report the same list. The sort happened once at
    // construction — this only copies it out, which is what makes the order a
    // function of the export names rather than of any table walked here.
    if (hdr->flags == ModuleNamespaceHeader::kFlags) {
        Rooted<Value> src{objVal};
        const std::vector<StringHeader*> names = rtModuleNamespaceKeys(src.get());
        Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(names.size())))};
        uint32_t at = 0;
        for (StringHeader* name : names) {
            Rooted<Value> key{rtCopyKeyToHeap(name)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    // A function's own keys are `length`, `name` and `prototype` — every one of
    // them non-enumerable (10.2.4, 20.2.4) — plus the statics it was assigned,
    // which are the only group this member reports. Those live in the side
    // object, so the answer is COMPLETE, and that is what separates this from
    // `Object.getOwnPropertyNames` of the same function: that member wants the
    // non-enumerable three, and bronze stores none of them.
    if (hdr->flags == HeapKind::Function) {
        Value props = objVal.asObject<FunctionHeader>()->properties;
        if (!props.isObject()) return bronze_create_array(0);
        Rooted<Value> propsRoot{props};
        const std::vector<StringHeader*> named =
            rtOwnStringKeysOrdered(propsRoot.get().asObject<ObjectHeader>());
        Rooted<Value> out{Value(bronze_create_array(static_cast<uint32_t>(named.size())))};
        uint32_t at = 0;
        for (StringHeader* k : named) {
            Rooted<Value> key{rtCopyKeyToHeap(k)};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }
    if (hdr->flags == MapHeader::kMapFlags || hdr->flags == MapHeader::kSetFlags ||
        hdr->flags == MapHeader::kWeakMapFlags || hdr->flags == MapHeader::kWeakSetFlags ||
        hdr->flags == RegExpHeader::kFlags || hdr->flags == ArrayBufferHeader::kFlags ||
        hdr->flags == DataViewHeader::kFlags) {
        // None of these has an own enumerable string-keyed property, and that
        // is a fact about the LANGUAGE rather than about bronze's storage: a
        // Map's and a Set's entries are internal slots reached by `get`/`add`,
        // a RegExp's `lastIndex` is an own property but non-enumerable
        // (22.2.6.9), and an ArrayBuffer's and a DataView's `byteLength` and
        // friends are accessors on their prototypes. So `[]` is the complete
        // answer, and refusing it named bronze's coverage instead.
        return bronze_create_array(0);
    }
    if (hdr->flags != HeapKind::Plain) {
        // An iteration record and an environment record are the remainder, and
        // nothing hands a program either — so reaching here is a lowering bug
        // rather than something a program did.
        fatal("internal: Object.keys on an object kind no program can hold");
    }

    // `Object.keys` is own ENUMERABLE STRING keys (20.1.2.17 -> 7.3.23 with
    // key-of-type-String), so the symbol half of the own keys never reaches
    // here — which is how a symbol-keyed property becomes invisible to
    // `Object.keys`, `Object.entries` and `JSON.stringify` at once, by BEING a
    // symbol rather than by any rule about its spelling.
    const std::vector<StringHeader*> ordered =
        rtOwnStringKeysOrdered(reinterpret_cast<ObjectHeader*>(hdr));

    const uint32_t total = static_cast<uint32_t>(ordered.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), total ? total : 4))};

    uint32_t at = 0;
    for (StringHeader* name : ordered) {
        // Copy the immortal arena string into the heap: the result array holds
        // ordinary JS strings, not pointers into the shape arena.
        Rooted<Value> key{rtCopyKeyToHeap(name)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
    }

    return out.get().rawBits();
}

// ---- Environment records: `depth` parent hops, then `index` ----

uint64_t bronze_env_create(uint64_t parentBits, uint32_t slotCount) {
    recordHelperCall("bronze_env_create");
    Rooted<Value> parent{Value(parentBits)};
    return Value::fromObject(EnvHeader::create(rtHeap(), parent, slotCount)).rawBits();
}

static EnvHeader* resolveEnv(uint64_t envBits, uint32_t depth) {
    Value envVal(envBits);
    if (!envVal.isObject()) {
        fatal("environment access on a value that is not an environment record");
    }
    auto* env = envVal.asObject<EnvHeader>();
    if (env->header.flags != EnvHeader::kFlags) {
        fatal("environment access on a value that is not an environment record");
    }
    return env->ancestor(depth);
}

uint64_t bronze_env_get(uint64_t envBits, uint32_t depth, uint32_t index) {
    recordHelperCall("bronze_env_get");
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    return env->slotsData()[index].rawBits();
}

static_assert(Value::fromUninitialized().rawBits() == BRONZE_ABI_UNINITIALIZED_BITS,
              "BRONZE_ABI_UNINITIALIZED_BITS in bronze_abi.h has drifted from the "
              "uninitialized-binding singleton");

// The same read, for a slot holding a `let`, `const` or `class` binding.
// ECMA-262 9.1.1.1.6 GetBindingValue: "if the binding for N in envRec is an
// uninitialized binding, throw a ReferenceError". The marker is the only thing
// that distinguishes the two states, and it can never be a program's value, so
// one compare against it IS the check.
//
// `keyIndex` is the module's interned name, so the message can say which
// binding without the backend carrying a string — the same arrangement
// `bronze_reference_error` uses. Returns `undefined` on the raising path for
// the reason every raise helper does: the result lands in a caller's GC root
// slot before the pending cell is tested.
uint64_t bronze_env_get_tdz(uint64_t envBits, uint32_t depth, uint32_t index,
                            uint32_t keyIndex) {
    recordHelperCall("bronze_env_get_tdz");
    const uint64_t bits = bronze_env_get(envBits, depth, index);
    if (bits != BRONZE_ABI_UNINITIALIZED_BITS) return bits;
    // A binding name reaching here can be a FLATTENED one — the module linker
    // renames a non-entry file's module scope to `modN.local`, and it can do
    // that safely precisely because a JavaScript identifier contains no dot. So
    // the text after the last dot is the name the program wrote, and a name
    // with no dot is already it. The message says what the source says;
    // `mod3.aLate` names a compilation strategy the programmer did not choose.
    const std::string& key = rtKeyString(keyIndex);
    const size_t dot = key.rfind('.');
    const std::string shown = dot == std::string::npos ? key : key.substr(dot + 1);
    return rtThrowReferenceError("Cannot access '" + shown + "' before initialization").rawBits();
}

void bronze_env_set(uint64_t envBits, uint32_t depth, uint32_t index, uint64_t valBits) {
    recordHelperCall("bronze_env_set");
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    env->slotsData()[index] = Value(valBits);
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc,
                             const uint64_t* argvBits) {
    recordCallSite("bronze_dynamic_call", calleeBits);
    Value calleeVal(calleeBits);
    if (!calleeVal.isObject()) {
        return rtThrowTypeError(std::string(valueKindName(calleeVal)) + " is not a function")
            .rawBits();
    }
    auto* hdr = calleeVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) {
        return rtThrowTypeError(std::string(valueKindName(calleeVal)) + " is not a function")
            .rawBits();
    }
    auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
    if (fn->arity == 0 || argc >= fn->arity) {
        return fn->code(fn->env_record.rawBits(), thisBits, argc, argvBits);
    }
    // argvBits already points into the caller's GC root frame, so it is rooted
    // exactly as long as the call needs it — copying it into a vector would
    // build an *unrooted* duplicate and cost a malloc per call.
    Value* argv = reinterpret_cast<Value*>(const_cast<uint64_t*>(argvBits));
    return fn->call(Value(thisBits), argc, argv).rawBits();
}

}  // extern "C"

static Value g_globalThisObject = Value::fromUndefined();

// The names 19.1–19.4 put on the global object that bronze provides. The
// SAME set lowering admits as provided globals (lower_util.cpp), minus
// `globalThis` itself — which is written as a self-reference below — because
// `Math` and `globalThis.Math` are one binding and must not drift.
static const char* const kGlobalObjectNames[] = {
    "Math", "Object", "Number", "JSON", "Array", "String", "Boolean", "Symbol", "RegExp",
    "Promise", "Map", "Set", "WeakMap", "WeakSet", "Error", "TypeError", "AggregateError",
    "RangeError", "SyntaxError", "ReferenceError", "URIError", "isNaN", "isFinite", "parseInt",
    "parseFloat", "ArrayBuffer", "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array",
    "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "DataView",
    "Function", "Proxy", "Reflect", "Date", "encodeURI", "encodeURIComponent", "decodeURI",
    "decodeURIComponent",
};

Value rtGlobalThisObject() {
    if (g_globalThisObject.isUndefined()) {
        Rooted<Value> glob{Value::fromObject(
            ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))};
        glob.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        // 9.3.4: builtins on the global object are writable and configurable
        // but NOT enumerable, which is what keeps a `for-in` over globalThis
        // from visiting Math.
        for (const char* name : kGlobalObjectNames) {
            Value resolved = Value::fromUndefined();
            if (!rtResolveBuiltinGlobal(name, resolved)) {
                fatal((std::string("internal: global object population lists '") + name +
                       "', a name the builtin ladder cannot resolve")
                          .c_str());
            }
            Rooted<Value> key{rtMakeString(name)};
            Rooted<Value> val{resolved};
            glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                         /*enumerable=*/false,
                                                         /*defineOwn=*/true);
        }
        // Host globals, as they stand when the object is FIRST read. A host
        // registers before `bronze_main` runs and this object cannot exist
        // before that, so the snapshot is the registry — a host that
        // re-registers a name afterwards updates free-name reads (which scan
        // the registry live) but not this object, an edge no current host
        // exercises.
        for (const auto& entry : rtHostGlobalEntries()) {
            Rooted<Value> key{rtMakeString(entry.first)};
            Rooted<Value> val{entry.second};
            glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                         /*enumerable=*/false,
                                                         /*defineOwn=*/true);
        }
        Rooted<Value> key{rtMakeString("globalThis")};
        glob.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, glob, nullptr,
                                                     /*enumerable=*/false, /*defineOwn=*/true);
        g_globalThisObject = glob.get();
        rtHeap().add_permanent_root(&g_globalThisObject);
    }
    return g_globalThisObject;
}

// The read half of "a property of the global object is a global binding":
// `bronze_global_get` asks this after the builtin ladder and the host
// registry both miss, so `globalThis.navigator = {...}` really does create
// the global that a later free `navigator` reads. Presence is the SHAPE's
// answer and not "the value is not undefined", because a global assigned
// `undefined` is a defined global and not a ReferenceError.
bool rtGlobalThisOwnLookup(const std::string& name, Value& out) {
    if (!g_globalThisObject.isObject()) return false;
    Rooted<Value> key{rtMakeString(name)};
    PropertyKey pkey = rtInternPropertyKey(key.get());
    auto* obj = g_globalThisObject.asObject<ObjectHeader>();
    uint32_t slot = 0;
    if (!obj->shape || !obj->shape->lookupProperty(pkey, slot)) return false;
    out = g_globalThisObject.asObject<ObjectHeader>()->getProp(rtHeap(), key);
    return true;
}

static uint64_t reflectApply(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 3) {
        return rtThrowTypeError("Reflect.apply requires at least 3 arguments").rawBits();
    }
    Value target(argv[0]);
    Value thisArg(argv[1]);
    Value argsList(argv[2]);
    if (!target.isObject() || target.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return rtThrowTypeError("Reflect.apply: target must be a function").rawBits();
    }
    if (!argsList.isObject()) {
        return rtThrowTypeError("Reflect.apply: arguments must be an object").rawBits();
    }
    if (argsList.asObject<HeapObjectHeader>()->flags != HeapKind::Array) {
        // 28.1.1 step 2 is CreateListFromArrayLike, which walks `length` and
        // indices off ANY object. bronze reads a real array's elements only,
        // and an array-like silently called with zero arguments would be the
        // silent wrong answer this refusal exists to prevent.
        fatal("unsupported: Reflect.apply with an argument list that is not an Array "
              "(CreateListFromArrayLike over an array-like is not built)");
    }
    uint32_t count = argsList.asObject<ArrayHeader>()->length;
    std::vector<Value> argVec(count);
    for (uint32_t i = 0; i < count; ++i) {
        argVec[i] = argsList.asObject<ArrayHeader>()->getElem(i);
    }
    return target.asObject<FunctionHeader>()->call(thisArg, count, argVec.data()).rawBits();
}

static uint64_t reflectGet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) return BRONZE_ABI_UNDEFINED_BITS;
    // 28.1.6 step 4: with a receiver, a GETTER found on the target runs
    // against the receiver instead. bronze's read path would run it against
    // the target, so a distinct receiver is refused rather than misanswered.
    if (argc > 2 && argv[2] != argv[0]) {
        fatal("unsupported: Reflect.get with a receiver distinct from the target");
    }
    return bronze_elem_get(argv[0], argv[1]);
}

static uint64_t reflectSet(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 3) return Value::fromBool(false).rawBits();
    if (argc > 3 && argv[3] != argv[0]) {
        fatal("unsupported: Reflect.set with a receiver distinct from the target");
    }
    bronze_elem_set(argv[0], argv[1], argv[2], /*strict=*/false);
    return Value::fromBool(true).rawBits();
}

static uint64_t reflectHas(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc < 2) return Value::fromBool(false).rawBits();
    return Value::fromBool(bronze_has_property(argv[1], argv[0])).rawBits();
}

static uint64_t reflectOwnKeys(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc == 0) return rtThrowTypeError("Reflect.ownKeys called on non-object").rawBits();
    const uint64_t call[1] = {argv[0]};
    return rtObjectGetOwnPropertyNames(0, 0, 1, call);
}

static uint64_t reflectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return objectGetPrototypeOf(0, 0, argc, argv);
}

static uint64_t reflectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return objectSetPrototypeOf(0, 0, argc, argv);
}

static uint64_t reflectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return rtObjectGetOwnPropertyDescriptor(0, 0, argc, argv);
}

static uint64_t reflectDefineProperty(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return rtObjectDefineProperty(0, 0, argc, argv);
}

static Value g_reflectNamespace = Value::fromUndefined();

Value rtReflectNamespace() {
    if (g_reflectNamespace.isUndefined()) {
        Rooted<Value> ns{Value::fromObject(
            ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(Value::fromNull())))};
        ns.get().asObject<HeapObjectHeader>()->flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        g_reflectNamespace = ns.get();
        rtHeap().add_permanent_root(&g_reflectNamespace);

        const NativeMethod methods[] = {
            {"apply", reflectApply, 3},
            {"get", reflectGet, 2},
            {"set", reflectSet, 3},
            {"has", reflectHas, 2},
            {"ownKeys", reflectOwnKeys, 1},
            {"getPrototypeOf", reflectGetPrototypeOf, 1},
            {"setPrototypeOf", reflectSetPrototypeOf, 2},
            {"getOwnPropertyDescriptor", reflectGetOwnPropertyDescriptor, 2},
            {"defineProperty", reflectDefineProperty, 3},
        };
        rtDefineMethods(ns, methods, std::size(methods));
    }
    return g_reflectNamespace;
}

// 21.4.4.2.1: milliseconds since the epoch, from the real clock. The
// determinism rule is about the COMPILER's output, not a compiled program's:
// a program that reads the clock reads the clock, exactly as it would under
// node, and an oracle case that printed this value would be wrong for a
// reason no fixed constant could fix.
static uint64_t dateNow(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return Value::fromDouble(static_cast<double>(ms)).rawBits();
}

// A Date OBJECT carries [[DateValue]] and some forty prototype methods, none
// of which bronze has built — so constructing one is refused by name rather
// than handed back as something that would misanswer every method call.
static uint64_t dateConstructor(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    fatal("unsupported: the Date constructor (Date.now() is built; a Date object and its "
          "prototype methods are not)");
}

static Value g_dateConstructor = Value::fromUndefined();

Value rtDateConstructor() {
    if (g_dateConstructor.isUndefined()) {
        Rooted<Value> ctor{rtNativeFunction(dateConstructor, 7)};
        rtEnsureFunctionProperties(ctor);
        Rooted<Value> props{ctor.get().asObject<FunctionHeader>()->properties};
        Rooted<Value> key{rtMakeString("now")};
        Rooted<Value> nowFn{rtNativeFunction(dateNow, 0)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, nowFn, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
        g_dateConstructor = ctor.get();
        rtHeap().add_permanent_root(&g_dateConstructor);
    }
    return g_dateConstructor;
}

}  // namespace bronze::runtime
