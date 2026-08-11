#pragma once

#include <string>

#include "runtime/value.h"

// The pending-exception cell and the `Error` family (docs/0020).
//
// The cell itself is an ABI global (`bronze_exception_cell` in
// bronze_abi.h) because generated code loads and compares it inline. What is
// here is everything ABOVE that word: how a runtime helper raises a spec'd
// error, how a helper that calls back into JS notices one, and the three
// constructors a program can reach by name.
//
// The line this file draws is decision 6's, and it is the reason `fatal` is
// still the right answer for most of the runtime's hard errors: a TypeError
// ECMA-262 defines becomes a catchable throw, and an unimplemented construct
// or a broken internal invariant does not. Letting a program `catch` "bronze
// has not built this" would turn the loud boundary of docs/0001 decision 8
// into a silent fallback with extra steps.

namespace bronze::runtime {

// Is an exception on its way out? Every runtime loop that calls back into JS
// must ask after each callback and stop — `[1,2,3].forEach(f)` where `f`
// throws must visit one element, not three, and no generated check runs
// inside a builtin's loop.
bool rtExceptionPending() noexcept;

// Discard whatever is pending. Exactly one caller, and it is ECMA-262 7.4.9
// step 6: closing an iterator while a throw is already in flight discards an
// error the iterator's `return` method raises, because the completion already
// on its way out is the one the program is entitled to see (docs/0021
// decision 3). Anywhere else this would be a silent swallow.
void rtClearException() noexcept;

// The three ways to raise, all of which RETURN `undefined` so that a helper
// can `return rtThrowTypeError(...)`. That is not a convenience: the caller
// stores the returned value into a GC root slot before it tests the cell, so
// a helper that returned anything the collector cannot parse would put a bad
// word in a live root (docs/0020 decision 2).
enum class ErrorKind { Error, TypeError, RangeError, SyntaxError };

Value rtThrow(Value thrown) noexcept;
Value rtThrowError(ErrorKind kind, const std::string& message);
Value rtThrowTypeError(const std::string& message);
Value rtThrowRangeError(const std::string& message);
// 22.2.3.1 step 4: a pattern that does not parse is a SyntaxError, and it is
// the one such error a running program can produce — a literal's pattern was
// compiled where it was written (docs/0024 decision 3), so only a pattern
// built at run time can reach here.
Value rtThrowSyntaxError(const std::string& message);

// The constructor objects, by name, for the provided-global path
// (docs/0011 decision 1). `undefined` for a name that is not one of them.
Value rtErrorConstructor(const std::string& name);

// Is this value an Error instance — that is, does `Error.prototype` sit on
// its prototype chain? Asked by `console.log`, which prints an error as
// `Name: message` rather than as a plain object (docs/0020 decision 7).
// Walking the chain rather than reading a header flag keeps error instances
// on the inline property fast path, which only believes flags == 0.
bool rtIsErrorInstance(Value v);

// `Name: message` into `out`, or just `Name` when the message is empty.
// bronze has no stack to print, which is the deliberate divergence from node
// recorded in docs/0020 decision 7.
//
// Answers false — leaving `out` alone — when the value cannot be rendered
// this way WITHOUT ALLOCATING: an accessor `name`, a non-string `message`, a
// dictionary-mode instance. That is not fussiness. console.log's walk holds
// raw heap pointers across every element it formats and is documented to
// allocate nothing, so a moving collection inside it is a use-after-move.
bool rtErrorText(Value v, std::string& out);

// The text an uncaught exception is reported with, without the trailing
// newline. Shared by `bronze_uncaught_exception` and its test.
std::string rtUncaughtText(Value thrown);

}  // namespace bronze::runtime
