#include "opt/wpo/builtins.h"

#include <initializer_list>

#include "opt/wpo/value.h"

namespace bronze::wpo {

namespace {

constexpr uint16_t kStrP = kStr;
constexpr uint16_t kNumP = kNumber;
constexpr uint16_t kBoolP = kBool;

}  // namespace

void BuiltinCatalog::add(BuiltinDef def) {
    const uint32_t index = static_cast<uint32_t>(defs_.size());
    byPath_.emplace(def.path, index);
    defs_.push_back(std::move(def));
}

uint32_t BuiltinCatalog::find(const std::string& path) const {
    auto it = byPath_.find(path);
    return it == byPath_.end() ? UINT32_MAX : it->second;
}

BuiltinCatalog::BuiltinCatalog() {
    // An object entry: a namespace, a constructor or a prototype.
    auto object = [&](const std::string& path, const std::string& proto, bool complete,
                      Effect call = Effect::None, Effect construct = Effect::None,
                      const std::string& instanceProto = {}, uint16_t prims = 0) {
        BuiltinDef def;
        def.path = path;
        def.proto = proto;
        def.complete = complete;
        def.call = call;
        def.construct = construct;
        def.instanceProto = instanceProto;
        def.prims = prims;
        add(std::move(def));
    };
    // A function member. Its own [[Prototype]] is Function.prototype.
    auto fn = [&](const std::string& path, Effect call, uint16_t prims = 0) {
        BuiltinDef def;
        def.path = path;
        def.call = call;
        def.prims = prims;
        def.proto = "Function.prototype";
        add(std::move(def));
    };
    auto fns = [&](const std::string& owner, std::initializer_list<const char*> names, Effect call,
                   uint16_t prims = 0) {
        for (const char* name : names) fn(owner.empty() ? std::string(name) : owner + "." + name, call, prims);
    };
    // A primitive-valued member (`Math.PI`, `Symbol.iterator`, a getter
    // whose answer is a number, like `Map.prototype.size`).
    auto prim = [&](const std::string& path, uint16_t prims) {
        BuiltinDef def;
        def.path = path;
        def.isObject = false;
        def.prims = prims;
        add(std::move(def));
    };
    auto prims = [&](const std::string& owner, std::initializer_list<const char*> names, uint16_t bits) {
        for (const char* name : names) prim(owner + "." + name, bits);
    };

    // ---- Object ----
    object("Object", "Function.prototype", false, Effect::ObjectCtor, Effect::ObjectCtor,
           "Object.prototype");
    object("Object.prototype", "", true);
    fn("Object.assign", Effect::ObjectAssign);
    fn("Object.create", Effect::ObjectCreate);
    fn("Object.defineProperty", Effect::ObjectDefineProperty);
    fns("Object", {"defineProperties", "fromEntries", "getOwnPropertyDescriptor",
                   "getOwnPropertyDescriptors", "getOwnPropertySymbols", "setPrototypeOf",
                   "groupBy"},
        Effect::Escape);
    fn("Object.entries", Effect::ObjectEntries);
    fns("Object", {"freeze", "preventExtensions", "seal"}, Effect::FreezeArg0);
    fn("Object.keys", Effect::ObjectEnumKeys);
    fn("Object.getOwnPropertyNames", Effect::ObjectKeys);
    fn("Object.values", Effect::ObjectValues);
    fn("Object.getPrototypeOf", Effect::ObjectGetProto);
    fns("Object", {"is", "isExtensible", "isFrozen", "isSealed", "hasOwn"}, Effect::Prim, kBoolP);
    fn("Object.prototype.constructor", Effect::ObjectCtor);
    fns("Object.prototype", {"hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable"},
        Effect::Prim, kBoolP);
    fns("Object.prototype", {"toLocaleString", "toString"}, Effect::Prim, kStrP);
    fn("Object.prototype.valueOf", Effect::ReturnThis);
    fns("Object.prototype", {"__defineGetter__", "__defineSetter__", "__lookupGetter__",
                             "__lookupSetter__"},
        Effect::Escape);

    // ---- Function ----
    object("Function", "Function.prototype", false, Effect::Escape, Effect::Escape,
           "Function.prototype");
    object("Function.prototype", "Object.prototype", true, Effect::NoOp);
    fn("Function.prototype.apply", Effect::FnApply);
    fn("Function.prototype.bind", Effect::FnBind);
    fn("Function.prototype.call", Effect::FnCall);
    fn("Function.prototype.toString", Effect::Prim, kStrP);
    fns("Function.prototype", {"caller", "arguments"}, Effect::Escape);
    fn("Function.prototype.constructor", Effect::Escape);
    prim("Function.prototype.length", kNumP);
    prim("Function.prototype.name", kStrP);

    // ---- Array ----
    object("Array", "Function.prototype", false, Effect::ArrayCtor, Effect::ArrayCtor,
           "Array.prototype");
    object("Array.prototype", "Object.prototype", true);
    fn("Array.from", Effect::ArrayFrom);
    fn("Array.of", Effect::ArrayOf);
    fn("Array.isArray", Effect::Prim, kBoolP);
    const std::string ap = "Array.prototype";
    fn(ap + ".constructor", Effect::ArrayCtor);
    prim(ap + ".length", kNumP);
    fn(ap + ".at", Effect::ArrayAt);
    fn(ap + ".concat", Effect::ArrayConcat);
    fns(ap, {"copyWithin", "reverse"}, Effect::ReturnThis);
    fn(ap + ".entries", Effect::ArrayEntries);
    fn(ap + ".keys", Effect::ArrayKeys);
    fn(ap + ".values", Effect::ArrayValues);
    fns(ap, {"every", "some", "includes"}, Effect::ArrayPredicate, kBoolP);
    fns(ap, {"findIndex", "findLastIndex"}, Effect::ArrayPredicate, kNumP);
    fns(ap, {"indexOf", "lastIndexOf"}, Effect::Prim, kNumP);
    fns(ap, {"join", "toLocaleString", "toString"}, Effect::Prim, kStrP);
    fn(ap + ".fill", Effect::ArrayFill);
    fn(ap + ".filter", Effect::ArrayFilter);
    fns(ap, {"find", "findLast"}, Effect::ArrayFind);
    fn(ap + ".flat", Effect::ArrayFlat);
    fns(ap, {"flatMap", "toSorted", "toSpliced", "with"}, Effect::Escape);
    fn(ap + ".forEach", Effect::ArrayForEach);
    fn(ap + ".map", Effect::ArrayMap);
    fns(ap, {"pop", "shift"}, Effect::ArrayPop);
    fns(ap, {"push", "unshift"}, Effect::ArrayPush);
    fns(ap, {"reduce", "reduceRight"}, Effect::ArrayReduce);
    fns(ap, {"slice", "toReversed"}, Effect::ArraySlice);
    fn(ap + ".sort", Effect::ArraySort);
    fn(ap + ".splice", Effect::ArraySplice);

    // ---- typed arrays: one shared prototype stands for %TypedArray%.prototype
    // and each view's own, whose only own keys are `constructor` and
    // BYTES_PER_ELEMENT. ----
    object("TypedArray", "Function.prototype", false, Effect::TypedCtor, Effect::TypedCtor,
           "TypedArray.prototype");
    object("TypedArray.prototype", "Object.prototype", true);
    const std::string tp = "TypedArray.prototype";
    fn(tp + ".constructor", Effect::TypedCtor);
    prims(tp, {"length", "byteLength", "byteOffset", "BYTES_PER_ELEMENT"}, kNumP);
    fns(tp, {"at"}, Effect::Prim, kNumP | kUndef);
    fns(tp, {"copyWithin", "reverse", "fill", "sort"}, Effect::ReturnThis);
    fns(tp, {"includes"}, Effect::Prim, kBoolP);
    fns(tp, {"every", "some"}, Effect::ArrayPredicate, kBoolP);
    fns(tp, {"findIndex", "findLastIndex"}, Effect::ArrayPredicate, kNumP);
    fns(tp, {"find", "findLast"}, Effect::ArrayPredicate, kNumP | kUndef);
    fns(tp, {"indexOf", "lastIndexOf"}, Effect::Prim, kNumP);
    fns(tp, {"join", "toLocaleString", "toString"}, Effect::Prim, kStrP);
    fns(tp, {"filter", "map", "slice", "subarray", "toReversed", "toSorted", "with"},
        Effect::TypedSubarray);
    fn(tp + ".forEach", Effect::ArrayForEach);
    fns(tp, {"reduce", "reduceRight"}, Effect::ArrayReduce);
    fn(tp + ".set", Effect::TypedSet);
    fns(tp, {"entries", "keys", "values"}, Effect::ArrayValues);
    fn(tp + ".buffer", Effect::Escape);
    for (const char* view : {"Float32Array", "Float64Array", "Int8Array", "Int16Array", "Int32Array",
                             "Uint8Array", "Uint8ClampedArray", "Uint16Array", "Uint32Array",
                             "BigInt64Array", "BigUint64Array"}) {
        const std::string name = view;
        object(name, "TypedArray", false, Effect::Escape, Effect::TypedCtor, tp);
        fns(name, {"from", "of"}, Effect::TypedCtor);
        prim(name + ".BYTES_PER_ELEMENT", kNumP);
        object(name + ".prototype", "Object.prototype", false);
    }
    object("ArrayBuffer", "Function.prototype", false, Effect::Escape, Effect::NewOther,
           "ArrayBuffer.prototype");
    object("ArrayBuffer.prototype", "Object.prototype", false);
    fn("ArrayBuffer.isView", Effect::Prim, kBoolP);
    prim("ArrayBuffer.prototype.byteLength", kNumP);
    object("DataView", "Function.prototype", false, Effect::Escape, Effect::NewOther,
           "DataView.prototype");
    object("DataView.prototype", "Object.prototype", false);

    // ---- String / Number / Boolean / Symbol ----
    object("String", "Function.prototype", false, Effect::Prim, Effect::NewOther,
           "String.prototype", kStrP);
    fns("String", {"fromCharCode", "fromCodePoint", "raw"}, Effect::Prim, kStrP);
    object("String.prototype", "Object.prototype", true);
    const std::string sp = "String.prototype";
    fn(sp + ".constructor", Effect::Prim, kStrP);
    prim(sp + ".length", kNumP);
    fns(sp, {"at", "charAt", "concat", "normalize", "padEnd", "padStart", "repeat", "slice",
             "substring", "substr", "toLowerCase", "toUpperCase", "toLocaleLowerCase",
             "toLocaleUpperCase", "toString", "trim", "trimStart", "trimEnd", "trimLeft",
             "trimRight", "valueOf", "toWellFormed", "anchor", "big", "blink", "bold",
             "fixed", "fontcolor", "fontsize", "italics", "link", "small", "strike", "sub",
             "sup"},
        Effect::Prim, kStrP | kUndef);
    fns(sp, {"charCodeAt", "codePointAt", "indexOf", "lastIndexOf", "localeCompare", "search"},
        Effect::Prim, kNumP | kUndef);
    fns(sp, {"endsWith", "includes", "startsWith", "isWellFormed"}, Effect::Prim, kBoolP);
    fns(sp, {"replace", "replaceAll"}, Effect::StrReplace, kStrP);
    fns(sp, {"split", "match"}, Effect::StrArray);
    fn(sp + ".matchAll", Effect::Escape);

    object("Number", "Function.prototype", false, Effect::Prim, Effect::NewOther,
           "Number.prototype", kNumP);
    fns("Number", {"isFinite", "isInteger", "isNaN", "isSafeInteger"}, Effect::Prim, kBoolP);
    fns("Number", {"parseFloat", "parseInt"}, Effect::Prim, kNumP);
    prims("Number", {"EPSILON", "MAX_SAFE_INTEGER", "MAX_VALUE", "MIN_SAFE_INTEGER", "MIN_VALUE",
                     "NaN", "NEGATIVE_INFINITY", "POSITIVE_INFINITY"},
          kNumP);
    object("Number.prototype", "Object.prototype", true);
    fn("Number.prototype.constructor", Effect::Prim, kNumP);
    fns("Number.prototype", {"toExponential", "toFixed", "toLocaleString", "toPrecision",
                             "toString"},
        Effect::Prim, kStrP);
    fn("Number.prototype.valueOf", Effect::Prim, kNumP);

    object("Boolean", "Function.prototype", false, Effect::Prim, Effect::NewOther,
           "Boolean.prototype", kBoolP);
    object("Boolean.prototype", "Object.prototype", true);
    fn("Boolean.prototype.constructor", Effect::Prim, kBoolP);
    fn("Boolean.prototype.toString", Effect::Prim, kStrP);
    fn("Boolean.prototype.valueOf", Effect::Prim, kBoolP);

    object("Symbol", "Function.prototype", false, Effect::SymbolFn, Effect::None);
    prims("Symbol", {"asyncIterator", "hasInstance", "isConcatSpreadable", "iterator", "match",
                     "matchAll", "replace", "search", "species", "split", "toPrimitive",
                     "toStringTag", "unscopables"},
          kSym);
    fn("Symbol.for", Effect::SymbolFn);
    fn("Symbol.keyFor", Effect::Prim, kStrP | kUndef);
    object("Symbol.prototype", "Object.prototype", true);
    fn("Symbol.prototype.constructor", Effect::SymbolFn);
    fn("Symbol.prototype.toString", Effect::Prim, kStrP);
    fn("Symbol.prototype.valueOf", Effect::Prim, kSym);
    prim("Symbol.prototype.description", kStrP | kUndef);

    object("BigInt", "Function.prototype", false, Effect::Prim, Effect::None, "", kBigInt);

    // ---- Math / JSON ----
    object("Math", "Object.prototype", true);
    fns("Math", {"abs", "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh", "cbrt", "ceil",
                 "clz32", "cos", "cosh", "exp", "expm1", "floor", "fround", "hypot", "imul", "log",
                 "log10", "log1p", "log2", "max", "min", "pow", "random", "round", "sign", "sin",
                 "sinh", "sqrt", "tan", "tanh", "trunc", "f16round"},
        Effect::Prim, kNumP);
    prims("Math", {"E", "LN10", "LN2", "LOG10E", "LOG2E", "PI", "SQRT1_2", "SQRT2"}, kNumP);
    object("JSON", "Object.prototype", false);
    fn("JSON.parse", Effect::JsonParse);
    fn("JSON.stringify", Effect::EscapePrim, kStrP | kUndef);

    // ---- collections. A Set keeps its values in both `keys` and `elems`, so
    // forEach and the iterators read one layout for both kinds. ----
    for (const char* coll : {"Map", "Set", "WeakMap", "WeakSet"}) {
        const std::string name = coll;
        object(name, "Function.prototype", false, Effect::Escape, Effect::CollectionCtor,
               name + ".prototype");
        object(name + ".prototype", "Object.prototype", true);
        fn(name + ".prototype.constructor", Effect::Escape);
        fns(name + ".prototype", {"delete", "has"}, Effect::Prim, kBoolP);
    }
    for (const char* coll : {"Map", "Set"}) {
        const std::string p = std::string(coll) + ".prototype";
        fn(p + ".clear", Effect::NoOp);
        fn(p + ".entries", Effect::MapEntries);
        fn(p + ".forEach", Effect::MapForEach);
        fn(p + ".values", Effect::MapValues);
        prim(p + ".size", kNumP);
    }
    fn("Map.prototype.get", Effect::MapGet);
    fn("Map.prototype.set", Effect::MapSet);
    fn("Map.prototype.keys", Effect::MapKeys);
    fn("Map.groupBy", Effect::Escape);
    fn("WeakMap.prototype.get", Effect::MapGet);
    fn("WeakMap.prototype.set", Effect::MapSet);
    fn("Set.prototype.add", Effect::SetAdd);
    fn("Set.prototype.keys", Effect::MapValues);
    fns("Set.prototype", {"union", "intersection", "difference", "symmetricDifference",
                          "isSubsetOf", "isSupersetOf", "isDisjointFrom"},
        Effect::Escape);
    fn("WeakSet.prototype.add", Effect::SetAdd);

    // ---- errors, regexps, dates: builtin-shaped objects. ----
    object("Error.prototype", "Object.prototype", false);
    prims("Error.prototype", {"name", "message"}, kStrP);
    fn("Error.prototype.toString", Effect::Prim, kStrP);
    for (const char* err : {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
                            "EvalError", "URIError", "AggregateError"}) {
        object(err, "Function.prototype", false, Effect::NewOther, Effect::NewOther,
               "Error.prototype");
    }
    // Adds `stack` to its argument.
    fn("Error.captureStackTrace", Effect::Escape);
    object("RegExp", "Function.prototype", false, Effect::NewOther, Effect::NewOther,
           "RegExp.prototype");
    object("RegExp.prototype", "Object.prototype", false);
    fn("RegExp.prototype.exec", Effect::StrArray);
    fn("RegExp.prototype.test", Effect::Prim, kBoolP);
    prims("RegExp.prototype", {"lastIndex"}, kNumP);
    prims("RegExp.prototype", {"source", "flags"}, kStrP);
    object("Date", "Function.prototype", false, Effect::Prim, Effect::NewOther, "Date.prototype",
           kStrP);
    fns("Date", {"now", "parse", "UTC"}, Effect::Prim, kNumP);
    object("Date.prototype", "Object.prototype", false);
    fns("Date.prototype", {"getTime", "valueOf", "getFullYear", "getMonth", "getDate", "getDay",
                           "getHours", "getMinutes", "getSeconds", "getMilliseconds",
                           "getTimezoneOffset"},
        Effect::Prim, kNumP);
    fns("Date.prototype", {"toISOString", "toString", "toDateString", "toTimeString",
                           "toLocaleString", "toLocaleDateString", "toLocaleTimeString",
                           "toUTCString", "toJSON"},
        Effect::Prim, kStrP);

    // ---- the global functions ----
    fns("", {"parseInt", "parseFloat"}, Effect::Prim, kNumP);
    fns("", {"isNaN", "isFinite"}, Effect::Prim, kBoolP);
    fns("", {"encodeURIComponent", "encodeURI", "decodeURIComponent", "decodeURI", "escape",
             "unescape"},
        Effect::Prim, kStrP);

    // ---- what the host provides and the analysis still knows: the logger
    // (it formats its arguments and keeps none of them) and the clock. ----
    object("console", "Object.prototype", false);
    fns("console", {"log", "warn", "error", "info", "debug", "trace", "time", "timeEnd",
                    "timeLog", "group", "groupCollapsed", "groupEnd", "table", "assert",
                    "count", "countReset", "dir"},
        Effect::NoOp);
    object("performance", "Object.prototype", false);
    fn("performance.now", Effect::Prim, kNumP);
}

const BuiltinCatalog& builtinCatalog() {
    static const BuiltinCatalog catalog;
    return catalog;
}

}  // namespace bronze::wpo
