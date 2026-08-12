// The `Symbol` constructor object: what the identifier `Symbol` denotes, and
// the statics hanging off it.
//
// It is a FUNCTION object rather than a namespace object because `Symbol("tag")`
// is a call — 20.4.1.1 defines it as a constructor that refuses `new`. That
// shape also decides where its unimplemented members are diagnosed: they reach
// the function property path in rt_prop.cpp, not a namespace object's.
//
// The value model this hands out lives in symbol.cpp; here is only the object a
// program names.

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
// `iterator` is NOT here and is not implemented either — it is the string
// `"@@iterator"` (runtime/iterator.h), which is a divergence pinned the other
// way round in cases/iterator_protocol.js rather than a missing name. Promoting
// it to a real symbol is a migration of its own: the `@@` prefix is what makes
// three separate mechanisms work, only one of which is about iterators.
const char* const kSymbolUnimplemented[] = {
    "asyncIterator", "hasInstance", "isConcatSpreadable", "match",       "matchAll",
    "replace",       "search",      "species",            "split",       "toPrimitive",
    "toStringTag",   "unscopables",
};

Value g_symbolFunction = Value::fromUndefined();

struct SymbolStatic {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const SymbolStatic kSymbolStatics[] = {
    {"for", symbolForCall, 1},
    {"keyFor", symbolKeyForCall, 1},
};

}  // namespace

Value rtSymbolFunction() {
    if (g_symbolFunction.isObject()) return g_symbolFunction;
    Rooted<Value> fn{Value(bronze_function_singleton(symbolCall, 1))};
    rtEnsureFunctionProperties(fn);
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    for (const SymbolStatic& s : kSymbolStatics) {
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> val{Value(bronze_function_singleton(s.code, s.arity))};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    }
    // `Symbol.iterator` is still the STRING `"@@iterator"`, unchanged and
    // deliberately so. Landing the symbol primitive does not migrate it: the
    // `@@` prefix is load-bearing in three places — the iterator hook, the
    // generator desugaring's self-property, and the internal slots a Map's and
    // a string's iterators keep — and moving one of them without the others
    // would break the other two silently.
    Rooted<Value> key{rtMakeString("iterator")};
    Rooted<Value> val{Value::fromString(rtIteratorKey())};
    props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
    g_symbolFunction = fn.get();
    rtHeap().add_permanent_root(&g_symbolFunction);
    return g_symbolFunction;
}

void rtSymbolCheckMissingMember(Value fn, const std::string& key) {
    if (!g_symbolFunction.isObject() || fn.rawBits() != g_symbolFunction.rawBits()) return;
    rtCheckUnimplementedMember("Symbol", kSymbolUnimplemented, std::size(kSymbolUnimplemented),
                               key);
}

}  // namespace bronze::runtime
