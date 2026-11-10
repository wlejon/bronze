#pragma once

// FAST ENTRIES: a second door into a native builtin, for compiled code's
// method sites (codegen-brass il2mir/il_native_fast.h), with the receiver and
// the arguments in registers instead of (env, this, argc, argv).
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
//     Array with no named-property box (whose member is the intrinsic, because
//     Array.prototype refuses every change); for a STRING entry a String whose
//     `String.prototype` member is this builtin's function object (the method
//     cache's PRIMITIVE entry plus the function's code word). Every entry
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

// Array.prototype.indexOf / includes with one argument (builtin_array_search.cpp).
uint64_t bronze_fast_array_index_of(uint64_t thisBits, uint64_t needleBits);
uint64_t bronze_fast_array_includes(uint64_t thisBits, uint64_t needleBits);

// String.prototype members (builtin_string_fast.cpp), each beside the function
// object's own code (builtin_string.cpp), whose address a site compares the
// member it finds against.
uint64_t bronze_fast_string_char_code_at(uint64_t thisBits, uint64_t posBits);
uint64_t bronze_fast_string_char_at(uint64_t thisBits, uint64_t posBits);
uint64_t bronze_fast_string_index_of(uint64_t thisBits, uint64_t searchBits, uint64_t posBits);
uint64_t bronze_fast_string_starts_with(uint64_t thisBits, uint64_t searchBits, uint64_t posBits);
uint64_t bronze_fast_string_slice(uint64_t thisBits, uint64_t startBits, uint64_t endBits);
uint64_t bronze_fast_string_substring(uint64_t thisBits, uint64_t startBits, uint64_t endBits);

uint64_t bronze_string_char_code_at(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_char_at(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_index_of(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_starts_with(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_slice(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t bronze_string_substring(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // extern "C"
