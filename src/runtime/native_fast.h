#pragma once

// FAST ENTRIES: a second door into a native builtin, for compiled code's
// method sites, with the receiver and the arguments in registers instead of
// (env, this, argc, argv). An ahead-of-time compile splits each method site
// of one on its receiver kind -- `is.plain_array` for an Array entry,
// `typeof.is` string or number for a String or Number one
// (il/fast_entries.h kFastEntries, lower/selfhost_merge.cpp) -- and calls the entry on the
// passing edge, sparing the site the method cache and the argument array.
//
// A fast entry is not a second implementation. Each one answers the common
// case of its builtin's own algorithm -- the arguments primitives whose
// conversion runs no user code, the receiver of the kind the site's guard
// established -- with the same helpers the builtin uses, and hands everything
// else to the builtin itself, with the arguments it was given in a C array. So
// the two cannot disagree about any input: the fast arm's inputs are a subset
// of the builtin's, answered by the builtin's code.
//
// The contract a caller keeps:
//   - `thisBits` is the receiver the site's guard named: for an ARRAY entry an
//     Array at the bare array shape (whose member is the intrinsic, because
//     Array.prototype is frozen); for a STRING or NUMBER entry a primitive of
//     that type, whose member is the intrinsic because its prototype (and
//     Object.prototype behind it) is frozen (lockdown.h). Every entry
//     re-tests the receiver's kind anyway, so a wrong one costs the builtin's
//     generic path and never an answer.
//   - the site's arguments, padded with `undefined` up to the entry's arity.
//     An entry is only offered for a builtin and arity where an absent
//     argument and an undefined one mean the same (ToIntegerOrInfinity of
//     undefined is 0, an undefined `end` is the length, ...).
//
// GC and exceptions are the builtin's: the fast arm reads its arguments and
// allocates nothing, or roots the receiver before it allocates (a slice) with
// its arguments numbers; the hand-off passes a C array, which every builtin
// copies into its RootedArgs before anything can allocate (the convention the
// method cache's padded call relies on, rt_method_call.cpp). A throw unwinds
// through the entry as through any runtime helper generated code calls.

#include <cstdint>

extern "C" {

// Array.prototype.indexOf / includes with one argument (builtin_array_search.cpp)
// and join (builtin_array_transform.cpp). `push` and `pop` need none: the
// method site's inline path answers them (il_call_entry.cpp).
uint64_t bronze_fast_array_index_of(uint64_t thisBits, uint64_t needleBits);
uint64_t bronze_fast_array_includes(uint64_t thisBits, uint64_t needleBits);
uint64_t bronze_fast_array_join(uint64_t thisBits, uint64_t sepBits);

// Number.prototype.toString (builtin_number_proto.cpp).
uint64_t bronze_fast_number_to_string(uint64_t thisBits, uint64_t radixBits);

// RegExp.prototype.exec / test (builtin_regexp_exec.cpp), for a receiver at
// the intrinsic instance shape (`is.collection` BRONZE_ABI_COLLECTION_REGEXP:
// no own key, the frozen RegExp.prototype).
uint64_t bronze_fast_regexp_exec(uint64_t thisBits, uint64_t strBits);
uint64_t bronze_fast_regexp_test(uint64_t thisBits, uint64_t strBits);

// A regular expression literal's evaluation (lower_expr.cpp): a fresh RegExp
// of the two constant strings. Not a fast entry; declared beside them.
uint64_t bronze_regexp_literal(uint64_t sourceBits, uint64_t flagsBits);

// String.prototype members: split and trim (builtin_string.cpp), the rest
// (builtin_string_search.cpp) beside their builtins, declared below.
uint64_t bronze_fast_string_char_code_at(uint64_t thisBits, uint64_t posBits);
uint64_t bronze_fast_string_char_at(uint64_t thisBits, uint64_t posBits);
uint64_t bronze_fast_string_index_of(uint64_t thisBits, uint64_t searchBits, uint64_t posBits);
uint64_t bronze_fast_string_starts_with(uint64_t thisBits, uint64_t searchBits, uint64_t posBits);
uint64_t bronze_fast_string_slice(uint64_t thisBits, uint64_t startBits, uint64_t endBits);
uint64_t bronze_fast_string_substring(uint64_t thisBits, uint64_t startBits, uint64_t endBits);
uint64_t bronze_fast_string_split(uint64_t thisBits, uint64_t sepBits, uint64_t limitBits);
uint64_t bronze_fast_string_trim(uint64_t thisBits);

uint64_t bronze_string_char_code_at(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_char_at(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_index_of(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_starts_with(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_slice(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_substring(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // extern "C"
