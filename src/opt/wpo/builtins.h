#pragma once

// The builtins the whole-program analysis knows the behaviour of.
//
// A builtin is named by its PATH (`Math`, `Array.prototype.push`), and an entry
// says what calling it and constructing with it do, as one of a closed set of
// effects the solver implements (solver_builtins.cpp). A builtin the catalog
// does not list is the open world: reading it gives an unknown value, and
// calling an unknown value lets every argument escape.
//
// The catalog assumes the builtins are PRISTINE — that neither the program nor
// the host replaced `Math.sqrt` or `Array.prototype.push` — which is the same
// assumption inference already makes for `math.unary`. A write the PROGRAM
// makes to a builtin object is still seen: it is a field of that object like
// any other, and reads see it beside the catalog's answer.
//
// A builtin PROTOTYPE is listed COMPLETELY (`complete`): a key it does not list
// is absent there, so a read of an instance field the prototype chain does not
// hold ends at `undefined` rather than at an unknown value. A namespace or a
// constructor is not, and an unlisted key on one reads as unknown.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bronze::wpo {

enum class Effect : uint8_t {
    None,         // not callable / not constructible
    Prim,         // returns `prims`; reads its arguments and keeps none of them
    NoOp,         // returns undefined and keeps nothing (console.log)
    Escape,       // the open world: the receiver and every argument escape
    EscapePrim,   // every argument escapes, the result is `prims` (JSON.stringify)
    ReturnThis,
    ReturnArg0,
    FreezeArg0,   // returns its argument, whose attributes it changes
    ObjectCtor,
    ObjectCreate,
    ObjectAssign,
    ObjectDefineProperty,
    ObjectKeys,       // every own key name (getOwnPropertyNames)
    ObjectEnumKeys,   // the enumerable own key names (Object.keys)
    ObjectValues,
    ObjectEntries,
    ObjectGetProto,
    ArrayCtor,
    ArrayFrom,
    ArrayOf,
    ArrayPush,
    ArrayPop,
    ArraySplice,
    ArraySlice,
    ArrayConcat,
    ArraySort,
    ArrayFill,
    ArrayForEach,
    ArrayMap,
    ArrayFilter,
    ArrayFind,
    ArrayPredicate,  // some / every / findIndex: callbacks, result `prims`
    ArrayReduce,
    ArrayValues,
    ArrayKeys,
    ArrayEntries,
    ArrayAt,
    ArrayFlat,
    TypedCtor,
    TypedSubarray,
    TypedSet,
    CollectionCtor,
    MapGet,
    MapSet,
    MapForEach,
    MapValues,
    MapKeys,
    MapEntries,
    SetAdd,
    FnCall,
    FnApply,
    FnBind,
    StrReplace,   // a string method that calls a function argument with strings
    StrArray,     // a string method returning an array of strings (split, match)
    SymbolFn,
    NewOther,     // makes a builtin-shaped object (Error, RegExp, Date)
    JsonParse,
};

struct BuiltinDef {
    std::string path;
    Effect call = Effect::None;
    Effect construct = Effect::None;
    // The result of a Prim / ArrayPredicate / EscapePrim call, and the value of
    // a non-object entry (`Math.PI`, `Symbol.iterator`).
    uint16_t prims = 0;
    // An object entry (a namespace, a constructor, a prototype), as opposed
    // to a primitive-valued one.
    bool isObject = true;
    // Every own key is listed: a key that is not reads as absent.
    bool complete = false;
    // The [[Prototype]] path, empty for none listed.
    std::string proto;
    // For a constructor: the prototype its instances get.
    std::string instanceProto;
};

class BuiltinCatalog {
public:
    BuiltinCatalog();
    // The entry at `path`, or UINT32_MAX.
    uint32_t find(const std::string& path) const;
    const BuiltinDef& def(uint32_t index) const { return defs_[index]; }
    uint32_t size() const { return static_cast<uint32_t>(defs_.size()); }

private:
    void add(BuiltinDef def);
    std::vector<BuiltinDef> defs_;
    std::unordered_map<std::string, uint32_t> byPath_;
};

// The catalog, built once.
const BuiltinCatalog& builtinCatalog();

}  // namespace bronze::wpo
