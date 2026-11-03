#pragma once

// The abstract heap of the whole-program analysis: one abstract object per
// ALLOCATION SITE (and, where one instruction can make several kinds of object,
// per site and tag), field-sensitive by key-pool index.
//
// Every IL instruction that makes an object names one: `create.object`,
// `create.array`, `new` (one per site and constructor function), `create.func`
// (the function object, and for a constructible function the `prototype`
// object the runtime makes for it on first use), `env.create`, a builtin's
// result (`new Float32Array`, `[].slice()`), and the rest arrays and iterator
// records the calling convention and the iteration protocol build. Builtin
// objects (`Math`, `Array.prototype`) are one abstract object per catalog path.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "il/il.h"
#include "opt/wpo/value.h"

namespace bronze::wpo {

enum class ObjKind : uint8_t {
    Plain,       // an object literal, `Object.create`, `{...spread}`
    Array,       // an array literal, a rest array, a builtin's array result
    Instance,    // the receiver `new F` allocates for a function F of the program
    Function,    // a closure (`create.func`) or a function singleton (`func.ref`)
    Proto,       // the `prototype` object of a constructible function
    Env,         // an environment record; `proto` is its parent record
    TypedArray,  // a view made by a typed-array constructor
    Collection,  // a Map, Set, WeakMap or WeakSet
    Bound,       // `f.bind(...)`: `env` is the target, `boundThis` its receiver
    Iterator,    // an iteration record or an iterator a builtin returned
    Builtin,     // a builtin object or function, by catalog path
    Other,       // any other builtin-shaped object the program made (Error, RegExp, ...)
};
const char* objKindName(ObjKind kind);

struct Accessor {
    AVal getters;
    AVal setters;
};

struct AObj {
    ObjKind kind = ObjKind::Plain;
    // Function: the IL function. Instance: the constructor it was made by.
    // Proto: the function it is the `prototype` of.
    uint32_t fn = UINT32_MAX;
    // The global instruction id (InstIndex) of the instruction that made it,
    // UINT32_MAX for a builtin and for the module environment.
    uint32_t site = UINT32_MAX;
    // Builtin: its catalog entry. Collection / TypedArray / Other: the catalog
    // entry of the constructor that made it, for its name and its prototype.
    uint32_t builtin = UINT32_MAX;
    // A tag that tells two objects one site made apart (`new` makes one per
    // constructor, a builtin call one per result it builds).
    uint32_t tag = 0;

    // Own DATA properties by key index (an environment's slots by slot index).
    std::map<uint32_t, AVal> fields;
    // Own ACCESSOR properties by key index.
    std::map<uint32_t, Accessor> accessors;
    // Index-keyed properties, and every value written under a key the analysis
    // could not name. A Map's / Set's values; an iteration record's values.
    AVal elems;
    // The join of every value in `fields`, kept as they are written: what a
    // read under an unknown key sees of them.
    AVal allFields;
    // A Map's keys.
    AVal keys;
    // Symbol-keyed properties.
    AVal sym;
    // [[Prototype]]; for an environment record, its parent record.
    AVal proto;
    // Function: the environment record it closed over. Bound: its target.
    AVal env;
    AVal boundThis;
    std::vector<AVal> boundArgs;
    // Own keys that are present from the object's creation on and before any
    // code can read it: a class prototype's methods and accessors, its
    // `constructor`. A read of one stops at this object rather than going on
    // up the chain. Sorted.
    std::vector<uint32_t> definite;

    // Reached code the analysis cannot see: reads of any key may also give an
    // unknown value, and the object may gain keys the program never names.
    bool escaped = false;
    // Written under a key the analysis could not name.
    bool anyKey = false;
    // A property was deleted from it.
    bool deleted = false;
    // A property's attributes were changed after it was made (freeze, a
    // non-writable define).
    bool attrsChanged = false;
    // Its prototype was replaced after it was made (`__proto__ =`,
    // `Object.setPrototypeOf` is an escape).
    bool protoSet = false;
    uint32_t escapeReason = UINT32_MAX;
    // The instruction whose transfer made it escape (UINT32_MAX: none, such
    // as an export bound before the first instruction runs).
    uint32_t escapeSite = UINT32_MAX;
};

// A dense numbering of every instruction of the module: a site is one
// uint32_t, and maps keyed by it are flat.
class InstIndex {
public:
    explicit InstIndex(const il::Module& mod);
    uint32_t id(uint32_t fn, uint32_t block, uint32_t inst) const {
        return blockBase_[fnBlockBase_[fn] + block] + inst;
    }
    struct Loc {
        uint32_t fn = 0;
        uint32_t block = 0;
        uint32_t inst = 0;
    };
    Loc locate(uint32_t id) const;
    uint32_t size() const { return total_; }

private:
    std::vector<uint32_t> fnBlockBase_;  // first entry of each function in blockBase_
    std::vector<uint32_t> blockBase_;    // first instruction id of each block
    std::vector<uint32_t> blockFn_;      // the function of each entry in blockBase_
    uint32_t total_ = 0;
};

// The key pool: the module's own key constants, plus the names the analysis
// itself needs to ask about (`prototype`, `length`), appended past the
// module's when the program never spelled them.
class KeyTable {
public:
    explicit KeyTable(const il::Module& mod);
    uint32_t intern(const std::string& name);
    const std::string& name(uint32_t key) const { return names_[key]; }
    // Whether the key is a canonical array index, cached.
    bool isIndex(uint32_t key);
    uint32_t size() const { return static_cast<uint32_t>(names_.size()); }

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::vector<int8_t> indexCache_;  // -1 unknown, 0 no, 1 yes
};

}  // namespace bronze::wpo
