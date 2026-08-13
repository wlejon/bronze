// The `Symbol` constructor object and `Symbol.prototype`: the two objects a
// program names, and the members of 20.4.2 and 20.4.3 hanging off them.
//
// `Symbol` is a FUNCTION object rather than a namespace object because
// `Symbol("tag")` is a call — 20.4.1.1 defines it as a constructor that refuses
// `new`. That shape also decides where its unimplemented members are diagnosed:
// they reach the function property path in rt_prop.cpp, not a namespace
// object's.
//
// `Symbol.prototype` is an ORDINARY object, which is the one thing separating
// it from the three intrinsics in builtin_wrappers.cpp: 20.4.3 says it "is not
// a Symbol instance and does not have a [[SymbolData]] internal slot", where
// 21.1.3 and 22.1.3 say the opposite of theirs. So there is no brand to carry
// and no wrapper to allocate, and a symbol reaches its members by the ordinary
// prototype walk with the primitive as the receiver.
//
// The value model this hands out lives in symbol.cpp; here is only what a
// program can hold.

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// 20.4.1.1. `Symbol()` and `Symbol(undefined)` are the same call — step 2 only
// runs ToString when the argument is not undefined — so the two produce symbols
// with no description at all, which `.description` reports as `undefined`
// rather than as "".
uint64_t symbolCall(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (args[0].isUndefined()) return rtMakeSymbol(Value::fromUndefined()).rawBits();
    if (args[0].isSymbol()) {
        // ToString(symbol) is the TypeError this type exists for, and it fires
        // for a DESCRIPTION as much as anywhere else.
        return rtThrowTypeError("Cannot convert a Symbol value to a string").rawBits();
    }
    Rooted<Value> desc{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtMakeSymbol(desc.get()).rawBits();
}

// 20.4.2.1 Symbol.for: the registry, which is the whole observable difference
// between it and `Symbol()` — the same string gives the same symbol back, where
// `Symbol()` never repeats itself.
uint64_t symbolForCall(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (args[0].isSymbol()) {
        return rtThrowTypeError("Cannot convert a Symbol value to a string").rawBits();
    }
    Rooted<Value> key{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtSymbolFor(key).rawBits();
}

// 20.4.2.6 Symbol.keyFor, the reverse. `undefined` for a symbol the registry
// never made — including one whose description equals a registered key, since
// the registry is searched by identity.
uint64_t symbolKeyForCall(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isSymbol()) {
        return rtThrowTypeError("Symbol.keyFor requires a symbol").rawBits();
    }
    return rtSymbolKeyFor(args[0]).rawBits();
}

// Real members of `Symbol` that bronze has not built. Same rule as every other
// table in rt_members.cpp: membership is ECMA-262's "does this exist?", never
// "have we got round to it?".
//
// `iterator` and `toStringTag` are NOT here because they are BUILT: they are
// the well-known symbols `rtSymbolIterator()` and `rtSymbolToStringTag()`
// (runtime/symbol.h), read by every iteration bronze opens and by 20.1.3.6's
// tag lookup. The other eleven are names ECMA-262 defines and bronze has not,
// so `Symbol.toPrimitive` is a diagnosed missing member rather than
// `undefined`.
const char* const kSymbolUnimplemented[] = {
    "asyncIterator", "hasInstance", "isConcatSpreadable", "match",       "matchAll",
    "replace",       "search",      "species",            "split",       "toPrimitive",
    "unscopables",
};

Value g_symbolFunction = Value::fromUndefined();
Value g_symbolPrototype = Value::fromUndefined();

struct SymbolStatic {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const SymbolStatic kSymbolStatics[] = {
    {"for", symbolForCall, 1},
    {"keyFor", symbolKeyForCall, 1},
};

// ---- Symbol.prototype (20.4.3) ---------------------------------------------

// 20.4.3.4 thisSymbolValue. Only the PRIMITIVE half exists: 20.4.3 gives this
// prototype no [[SymbolData]] slot of its own, and bronze builds no Symbol
// wrapper object — `new Symbol()` is the TypeError 20.4.1.1 makes it, and
// `Object(sym)` is a construct bronze refuses by name — so there is nothing
// else that could carry one.
bool thisSymbol(Value self, const char* method, Value& out) {
    if (!self.isSymbol()) {
        rtThrowTypeError(std::string("Symbol.prototype.") + method +
                         " called on an incompatible receiver");
        return false;
    }
    out = self;
    return true;
}

uint64_t symbolProtoToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self;
    if (!thisSymbol(Value(thisBits), "toString", self)) {
        return Value::fromUndefined().rawBits();
    }
    return rtMakeString(rtSymbolDescriptiveString(self)).rawBits();
}

uint64_t symbolProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self;
    if (!thisSymbol(Value(thisBits), "valueOf", self)) {
        return Value::fromUndefined().rawBits();
    }
    return self.rawBits();
}

// 20.4.3.2 is `get Symbol.prototype.description`, an ACCESSOR and not a data
// property — which is the whole reason `Symbol.prototype` needed a real object
// before it could be right: a table consulted beside the value cannot express
// the difference, and `Object.getOwnPropertyDescriptor(Symbol.prototype,
// "description")` is how a program asks.
uint64_t symbolProtoDescription(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self;
    if (!thisSymbol(Value(thisBits), "description", self)) {
        return Value::fromUndefined().rawBits();
    }
    const StringHeader* desc = self.asSymbol<SymbolHeader>()->description;
    // `undefined`, not "", for a symbol made without one — the clause reads
    // [[Description]] straight out and that field is genuinely absent.
    if (!desc) return Value::fromUndefined().rawBits();
    return rtCopyKeyToHeap(desc).rawBits();
}

const NativeMethod kSymbolProtoMethods[] = {
    {"toString", symbolProtoToString, 0},
    {"valueOf", symbolProtoValueOf, 0},
};

// 20.4.3, built on first use like every other intrinsic and PUBLISHED before it
// is decorated, because `constructor` asks for the function object and building
// that asks for this. A permanent root, not a plain static: the collector moves
// it and the installs below allocate.
void ensureSymbolPrototype() {
    if (g_symbolPrototype.isObject()) return;
    Rooted<Value> objectProto{rtObjectPrototype()};
    Rooted<Value> proto{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(objectProto.get())))};
    proto.get().asObject<ObjectHeader>()->header.flags = HeapKind::Plain;
    g_symbolPrototype = proto.get();
    rtHeap().add_permanent_root(&g_symbolPrototype);

    rtDefineMethods(proto, kSymbolProtoMethods, std::size(kSymbolProtoMethods));
    {
        Rooted<Value> key{rtMakeString("description")};
        Rooted<Value> getter{rtNativeFunction(symbolProtoDescription, 0)};
        Rooted<Value> setter{Value::fromUndefined()};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                     /*enumerable=*/false);
    }
    // 20.4.3.6: the tag is an ordinary own property of this object, and a
    // string. Making it real rather than a stand-in in `toStringTagOf` is what
    // lets `Symbol.prototype[Symbol.toStringTag]` be read off the object a
    // program holds — the same move `Object.prototype.toString`'s Math and JSON
    // tags made.
    {
        Rooted<Value> key{Value::fromSymbol(rtSymbolToStringTag())};
        Rooted<Value> tag{rtMakeString("Symbol")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, tag, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    {
        Rooted<Value> key{rtMakeString("constructor")};
        Rooted<Value> ctor{rtSymbolFunction()};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
}

}  // namespace

Value rtSymbolFunction() {
    if (g_symbolFunction.isObject()) return g_symbolFunction;
    Rooted<Value> fn{rtNativeFunction(symbolCall, 1)};
    rtEnsureFunctionProperties(fn);
    // Published before the prototype is asked for, because `Symbol.prototype`'s
    // `constructor` asks for THIS object back and the two would otherwise
    // recurse without end.
    g_symbolFunction = fn.get();
    rtHeap().add_permanent_root(&g_symbolFunction);
    // 20.4.2.9, and it must be set here rather than left to
    // `rtEnsureFunctionPrototype`, which mints a fresh empty object for a
    // function that has none — that object is what `Symbol.prototype` would
    // read, and a method installed on it would be found by nothing.
    //
    // `instance_shape` is set with it because that function's guard tests BOTH,
    // and a prototype without one is re-minted on the next read. Nothing ever
    // builds an object from the shape: 20.4.1.1 makes `new Symbol()` a
    // TypeError, so this constructor has no instances.
    {
        Rooted<Value> proto{rtSymbolPrototype()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    // NON-ENUMERABLE, which 20.4.2 makes all sixteen own properties of `Symbol`
    // and bronze had wrong: `Object.keys(Symbol)` reported four names and
    // `for (k in Symbol)` visited them. `writable` and `configurable` stay true
    // — a shape transition carries neither, and dictionary mode is the only
    // storage that does (cases/blocked/intrinsic_property_attributes).
    for (const SymbolStatic& s : kSymbolStatics) {
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> val{rtNativeFunction(s.code, s.arity)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    // 20.4.2.5 `Symbol.iterator` and 20.4.2.14 `Symbol.toStringTag`: the
    // well-known symbols themselves, as ordinary properties of this object.
    // Properties and not compile-time constants, because that is what ECMA-262
    // makes them — `[Symbol.iterator]` is a member expression, and evaluating
    // it is what makes a program that rebinds `Symbol` get its own answer.
    {
        Rooted<Value> key{rtMakeString("iterator")};
        Rooted<Value> val{rtIteratorKey()};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    {
        Rooted<Value> key{rtMakeString("toStringTag")};
        Rooted<Value> val{Value::fromSymbol(rtSymbolToStringTag())};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    return g_symbolFunction;
}

Value rtSymbolPrototype() {
    ensureSymbolPrototype();
    return g_symbolPrototype;
}

void rtSymbolCheckMissingMember(Value fn, const std::string& key) {
    if (!g_symbolFunction.isObject() || fn.rawBits() != g_symbolFunction.rawBits()) return;
    rtCheckUnimplementedMember("Symbol", kSymbolUnimplemented, std::size(kSymbolUnimplemented),
                               key);
}

}  // namespace bronze::runtime
