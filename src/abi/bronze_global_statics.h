#ifndef BRONZE_GLOBAL_STATICS_H
#define BRONZE_GLOBAL_STATICS_H

// Which of the globals bronze provides keep their members in a real,
// shape-indexed statics box — and which answer from a C table instead.
//
// This is NOT part of the generated-code ABI; nothing here appears in a
// signature generated code touches, and the ABI fingerprint hashes
// bronze_abi.h alone. It lives in this directory because `bronze::abi` is the
// only module both `bronze::lower` (through `bronze::il`) and
// `bronze::runtime` already depend on, and the fact below is one both halves
// have to agree about.
//
// The distinction is invisible from the language and decisive for the inline
// cache. `Object.keys` is an own data property of an object hanging off
// %Object%'s FunctionHeader, so a shape-keyed entry can name it and generated
// code can load it inline. `Array.isArray`, `String.fromCharCode`,
// `Number.EPSILON` and `Float32Array.BYTES_PER_ELEMENT` are answered by
// `rtGlobalConstructorMember` and its sibling tables, walking a C array by
// name on every read — and `installStaticsCacheEntry` REFUSES to cache those
// receivers, because the table's answer must not be pre-empted by a
// shape-keyed entry that another receiver of the same shape installed.
//
// So a read off a table-answered global can never hit the statics arm, and
// arming it is pure cost: the loads, the compare, and the extra blocks in
// front of a path that always falls through to the helper. `Number.EPSILON`
// in `Quaternion.slerp` is the case that made this table necessary — the
// hottest statics read in the three.js math benchmark, and one that could
// never hit.
//
// The list is therefore an ALLOWLIST rather than a list of exclusions, and
// that direction is the point: a global added to `isProvidedGlobal` tomorrow
// is table-answered until someone shows it is not, so the failure mode of
// forgetting this file is a missed optimisation and never a dead arm in
// somebody's inner loop. `tests/runtime/global_statics_test.cpp` holds every
// entry to its claim against the live runtime.

#include <string_view>

namespace bronze::abi {

// Each name here is a provided global that IS a function object, whose
// members are own data properties of its statics box, and which
// `rtIntrinsicConstructorName` does not refuse. Verified per entry by the
// runtime test named above; `Object` is the one that pays for the mechanism.
inline constexpr std::string_view kStaticsBoxGlobals[] = {
    "Object", "Promise", "Symbol", "BigInt", "Iterator", "Date",
};

inline constexpr bool isStaticsBoxGlobal(std::string_view name) noexcept {
    for (std::string_view candidate : kStaticsBoxGlobals) {
        if (candidate == name) return true;
    }
    return false;
}

}  // namespace bronze::abi

#endif  // BRONZE_GLOBAL_STATICS_H
