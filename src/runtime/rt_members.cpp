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

// Array.prototype (plus `constructor`), minus `length`, which is real.
const char* const kArrayMembers[] = {
    "at", "concat", "constructor", "copyWithin", "entries", "every", "fill", "filter",
    "find", "findIndex", "findLast", "findLastIndex", "flat", "flatMap", "forEach",
    "includes", "indexOf", "join", "keys", "lastIndexOf", "map", "pop", "push", "reduce",
    "reduceRight", "reverse", "shift", "slice", "some", "sort", "splice", "toLocaleString",
    "toReversed", "toSorted", "toSpliced", "toString", "unshift", "values", "with",
};

// String.prototype, minus `length`, which is real.
const char* const kStringMembers[] = {
    "at", "charAt", "charCodeAt", "codePointAt", "concat", "constructor", "endsWith",
    "includes", "indexOf", "isWellFormed", "lastIndexOf", "localeCompare",
    "normalize", "padEnd", "padStart", "repeat",
    "slice", "startsWith", "substr", "substring", "toLocaleLowerCase",
    "toLocaleUpperCase", "toLowerCase", "toString", "toUpperCase", "toWellFormed",
    "trim", "trimEnd", "trimStart", "valueOf",
};

// %TypedArray%.prototype, minus `length` and `buffer`, which are real.
const char* const kTypedArrayMembers[] = {
    "BYTES_PER_ELEMENT", "at", "byteLength", "byteOffset", "constructor", "copyWithin",
    "entries", "every", "fill", "filter", "find", "findIndex", "findLast", "findLastIndex",
    "forEach", "includes", "indexOf", "join", "keys", "lastIndexOf", "map", "reduce",
    "reduceRight", "reverse", "set", "slice", "some", "sort", "subarray", "toLocaleString",
    "toReversed", "toSorted", "toString", "values", "with",
};

// ArrayBuffer.prototype, minus `byteLength`, which is real.
const char* const kArrayBufferMembers[] = {
    "constructor", "detached", "maxByteLength", "resizable", "resize", "slice",
    "transfer", "transferToFixedLength",
};

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

void rtCheckTypedArrayMember(const std::string& key) {
    rtCheckUnimplementedMember("Float32Array.prototype", kTypedArrayMembers,
                               std::size(kTypedArrayMembers), key);
}

void rtCheckArrayBufferMember(const std::string& key) {
    rtCheckUnimplementedMember("ArrayBuffer.prototype", kArrayBufferMembers,
                               std::size(kArrayBufferMembers), key);
}

void rtCheckFunctionMember(const std::string& key) {
    rtCheckUnimplementedMember("Function.prototype", kFunctionMembers,
                               std::size(kFunctionMembers), key);
}

}  // namespace bronze::runtime
