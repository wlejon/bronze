// Members ECMA-262 defines and bronze has not built.
//
// A property JS really does not define reads as `undefined`, which is correct
// and stays. A property the LANGUAGE defines and bronze has not built is a
// different thing: returning `undefined` for it claims arrays have no `push`,
// and the program then feature-tests it away or calls it and dies naming
// bits. That is the silent fallback the house rules forbid, so every name
// below is diagnosed by name until it lands.
//
// Membership is therefore the ECMA-262 question "does this member exist?",
// never "have we got round to it?" — a name bronze DOES implement is answered
// before these tables are consulted, and leaves its table when it lands.

#include <iterator>
#include <string>

#include "runtime/fatal.h"
#include "runtime/rt_internal.h"

namespace bronze::runtime {

namespace {

// Array.prototype, minus `length` and `constructor`, which are real.
const char* const kArrayMembers[] = {
    "at", "concat", "copyWithin", "entries", "every", "fill", "filter",
    "find", "findIndex", "findLast", "findLastIndex", "flat", "flatMap", "forEach",
    "includes", "indexOf", "join", "keys", "lastIndexOf", "map", "pop", "push", "reduce",
    "reduceRight", "reverse", "shift", "slice", "some", "sort", "splice", "toLocaleString",
    "toReversed", "toSorted", "toSpliced", "toString", "unshift", "values", "with",
};

// String.prototype, minus `length` and `constructor`, which are real.
const char* const kStringMembers[] = {
    "at", "charAt", "charCodeAt", "codePointAt", "concat", "endsWith",
    "includes", "indexOf", "isWellFormed", "lastIndexOf", "localeCompare",
    "normalize", "padEnd", "padStart", "repeat",
    "slice", "startsWith", "substr", "substring", "toLocaleLowerCase",
    "toLocaleUpperCase", "toLowerCase", "toString", "toUpperCase", "toWellFormed",
    "trim", "trimEnd", "trimStart", "valueOf",
};

// The typed-array and ArrayBuffer tables are NOT here: they live in
// builtin_typed_array.cpp, beside the members that answer, because that file is
// where a name leaves the list when it lands and the two halves must be read
// together.

// Function.prototype, minus `prototype`, which is real.
const char* const kFunctionMembers[] = {
    "apply", "bind", "call", "constructor", "length", "name", "toString",
};

}  // namespace

void rtCheckUnimplementedMember(const char* receiver, const char* const* names, size_t count,
                                const std::string& key) {
    for (size_t i = 0; i < count; ++i) {
        if (key != names[i]) continue;
        std::string msg = std::string("unsupported: ") + receiver + "." + key +
                          " is not implemented";
        fatal(msg.c_str());
    }
}

void rtCheckArrayMember(const std::string& key) {
    rtCheckUnimplementedMember("Array.prototype", kArrayMembers, std::size(kArrayMembers), key);
}

void rtCheckStringMember(const std::string& key) {
    rtCheckUnimplementedMember("String.prototype", kStringMembers, std::size(kStringMembers), key);
}

void rtCheckFunctionMember(const std::string& key) {
    rtCheckUnimplementedMember("Function.prototype", kFunctionMembers,
                               std::size(kFunctionMembers), key);
}

}  // namespace bronze::runtime
