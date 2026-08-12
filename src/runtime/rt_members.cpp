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

// Array.prototype. What is NOT here is either implemented — the table in
// builtin_array.cpp answers first — or `length` and `constructor`, which are
// real. `sort` and `splice` are the two a real program misses first.
const char* const kArrayMembers[] = {
    "copyWithin", "entries", "flat", "flatMap", "keys", "sort", "splice",
    "toLocaleString", "toReversed", "toSorted", "toSpliced", "toString", "values", "with",
};

// String.prototype, on the same rule — and the answering side is the
// `String.prototype` OBJECT, filled from two tables: the plain members in
// builtin_string.cpp and the pattern-taking ones (`match`, `replace`,
// `search`, …) in builtin_string_regexp.cpp. This list is what a full-chain
// miss is checked against, so it is consulted after the object and after
// `Object.prototype` above it have both failed to answer.
//
// The five locale members are here as unimplemented rather than aliased to
// their non-locale twins: the deterministic-output rule forbids a locale
// function, and answering with `toLowerCase` would be right for most inputs
// and silently wrong for the ones the member exists to get right.
// `toLocaleString` (22.1.3.27) is one of them, and it is on this list rather
// than left to `Object.prototype`'s: it is a NEARER member of a different
// prototype, and a diagnostic that named the wrong holder would send a reader
// to the wrong file.
const char* const kStringMembers[] = {
    "isWellFormed", "localeCompare", "normalize", "substr",
    "toLocaleLowerCase", "toLocaleString", "toLocaleUpperCase", "toWellFormed",
};

// The typed-array and ArrayBuffer tables are NOT here: they live in
// builtin_typed_array.cpp, beside the members that answer, because that file is
// where a name leaves the list when it lands and the two halves must be read
// together.

// Function.prototype, minus `prototype`, which is real, and minus `call` and
// `apply`, which builtin_function.cpp answers.
const char* const kFunctionMembers[] = {
    "bind", "constructor", "length", "name", "toString",
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
