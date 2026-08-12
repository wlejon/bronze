// Object, array and function construction, the two class links, environment
// records, and the dynamic call path. Property access itself is rt_prop.cpp;
// typed arrays build themselves through ordinary constructor objects and so
// need nothing here.

#include <algorithm>
#include <charconv>
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
#include "runtime/object.h"
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
    proto->header.flags = 0;

    fn = fnVal.get().asObject<FunctionHeader>();  // create() may have moved it
    fn->prototype = Value::fromObject(proto);
    fn->instance_shape = rtNewRootShape(fn->prototype);
    rtInstallPrototypeConstructor(fnVal);
}

void rtEnsureFunctionProperties(Rooted<Value>& fnVal) {
    FunctionHeader* fn = fnVal.get().asObject<FunctionHeader>();
    if (fn->properties.isObject()) return;
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    props->header.flags = 0;
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
    ObjectHeader* obj = ObjectHeader::create(rtHeap(), rtArena(), rtPlainObjectShape());
    obj->header.flags = 0;
    return Value::fromObject(obj).rawBits();
}

// A GENERATOR OBJECT (ECMA-262 27.5.1): `bronze_create_object` with
// %GeneratorPrototype% in place of Object.prototype, and nothing else. The
// prototype is where 27.5.1.2's `[Symbol.iterator]() { return this; }` lives, so
// the object the generator desugaring fills in below is iterable without an own
// symbol-keyed property — which is what `Object.getOwnPropertySymbols` of one
// reports, and what a spec engine reports too.
uint64_t bronze_create_generator_object() {
    return rtNewIteratorObject(IteratorProto::Generator).rawBits();
}

uint64_t bronze_create_array(uint32_t length) {
    uint32_t cap = (length < 4) ? 4 : length;
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), cap);
    arr->header.flags = 1;
    arr->length = length;
    return Value::fromObject(arr).rawBits();
}

uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity, uint64_t envBits) {
    Rooted<Value> env{Value(envBits)};
    FunctionHeader* fn = FunctionHeader::create(rtHeap(), code, Value::fromUndefined(), arity);
    // Read the environment through the root only AFTER allocating: the
    // allocation above can collect, and a by-value copy taken before it would
    // point into dead from-space.
    fn->env_record = env.get();
    fn->header.flags = 2;
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
    if (!derivedVal.isObject() || derivedVal.asObject<HeapObjectHeader>()->flags != 2) {
        fatal("internal: class extends with a non-function derived class");
    }
    if (!baseVal.isObject() || baseVal.asObject<HeapObjectHeader>()->flags != 2) {
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

    Rooted<Value> derived{derivedVal};
    Rooted<Value> base{baseVal};
    rtEnsureFunctionPrototype(base);
    rtEnsureFunctionProperties(base);

    Rooted<Value> baseProto{base.get().asObject<FunctionHeader>()->prototype};
    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProto.get()));
    proto->header.flags = 0;
    Rooted<Value> protoRoot{Value::fromObject(proto)};

    Rooted<Value> baseProps{base.get().asObject<FunctionHeader>()->properties};
    ObjectHeader* props = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(baseProps.get()));
    props->header.flags = 0;

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
    Value fnVal(fnBits);
    if (!fnVal.isObject() || fnVal.asObject<HeapObjectHeader>()->flags != 2) {
        return rtThrowTypeError(std::string(valueKindName(fnVal)) + " is not a constructor")
            .rawBits();
    }

    // `new String(x)` and `new Boolean(x)` want a primitive WRAPPER object,
    // which bronze does not have. Left alone, the shape of this function makes
    // that silent in the worst way: the native returns a primitive, the rule
    // below discards any non-object return, and the program receives the empty
    // plain instance — `new String("ab").length` was `undefined`. Refusing by
    // name is the same call made for `Function`: a constructor that resolves
    // and hands back something that is not what was asked for is worse than one
    // that does not resolve.
    if (const char* wrapper = rtPrimitiveWrapperConstructorName(fnVal)) {
        fatal((std::string("`new ") + wrapper +
               "(...)` is unsupported: bronze has no primitive wrapper objects. Call " +
               wrapper + "(x) for the conversion, which is exact.")
                  .c_str());
    }

    Rooted<Value> fnRoot{fnVal};
    rtEnsureFunctionPrototype(fnRoot);

    FunctionHeader* fn = fnRoot.get().asObject<FunctionHeader>();
    ObjectHeader* instance = ObjectHeader::create(rtHeap(), rtArena(), fn->instance_shape);
    instance->header.flags = 0;

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

uint64_t bronze_object_keys(uint64_t objBits) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        fatal("Object.keys on a value that is not an object");
    }
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // An array's own keys are its indices, already in ascending order — the
    // ones it actually HAS: a hole left by `delete a[i]` is not an own
 // property, so the result is shorter than `length`.
    if (hdr->flags == 1) {
        Rooted<Value> src{objVal};
        uint32_t length = reinterpret_cast<ArrayHeader*>(hdr)->length;
        Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), length ? length : 4))};
        out.get().asObject<ArrayHeader>()->header.flags = 1;
        uint32_t at = 0;
        for (uint32_t i = 0; i < length; ++i) {
            if (!src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            char buf[16];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), i);
            Rooted<Value> key{Value::fromString(
                StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)))};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        // Then the named ones — the indices come first because they are
        // integer-like keys, and own-enumerable order puts those ahead of the
        // rest. Only a match array has any.
        if (src.get().asObject<ArrayHeader>()->properties.isObject()) {
            Rooted<Value> props{src.get().asObject<ArrayHeader>()->properties};
            const std::vector<StringHeader*> named =
                rtOwnStringKeysOrdered(props.get().asObject<ObjectHeader>());
            for (StringHeader* k : named) {
                Rooted<Value> key{rtCopyKeyToHeap(k)};
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
            }
        }
        return out.get().rawBits();
    }
    if (hdr->flags != 0) {
        fatal("Object.keys is only supported on plain objects and arrays");
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
    out.get().asObject<ArrayHeader>()->header.flags = 1;

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
    EnvHeader* env = resolveEnv(envBits, depth);
    if (index >= env->slotCount()) {
        fatal("environment slot index out of range (lowering bug)");
    }
    env->slotsData()[index] = Value(valBits);
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc,
                             const uint64_t* argvBits) {
    Value calleeVal(calleeBits);
    if (!calleeVal.isObject() || calleeVal.asObject<HeapObjectHeader>()->flags != 2) {
        return rtThrowTypeError(std::string(valueKindName(calleeVal)) + " is not a function")
            .rawBits();
    }
    auto* fn = calleeVal.asObject<FunctionHeader>();
    // argvBits already points into the caller's GC root frame, so it is rooted
    // exactly as long as the call needs it — copying it into a vector would
    // build an *unrooted* duplicate and cost a malloc per call.
    Value* argv = reinterpret_cast<Value*>(const_cast<uint64_t*>(argvBits));
    return fn->call(Value(thisBits), argc, argv).rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
