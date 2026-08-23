#pragma once

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

// The pending-exception cell and the `Error` family.
//
// The cell itself is the `exception_cell` field of the per-thread ABI block
// (bronze_abi.h) because generated code loads and compares it inline. What is
// here is everything ABOVE that word: how a runtime helper raises a spec'd
// error, how a helper that calls back into JS notices one, and the three
// constructors a program can reach by name.
//
// The line this file draws is the reason `fatal` is still the right answer for
// most of the runtime's hard errors: a TypeError ECMA-262 defines becomes a
// catchable throw, and an unimplemented construct or a broken internal
// invariant does not. Letting a program `catch` "bronze has not built this"
// would turn a loud boundary into a silent fallback with extra steps.

namespace bronze::runtime {

// Is an exception on its way out? Every runtime loop that calls back into JS
// must ask after each callback and stop — `[1,2,3].forEach(f)` where `f`
// throws must visit one element, not three, and no generated check runs
// inside a builtin's loop.
//
// Inline for the same reason runtime/tls_block.h's accessor is: this is one
// TLS word compared against one constant, asked after every callback of every
// runtime loop, and the chunk-6 sampler charged the out-of-line version 0.21
// ms/frame of `many_meshes` — all of it call overhead. The cell's meaning and
// every write to it are unchanged; only the read stopped being a call.
inline bool rtExceptionPending() noexcept {
    return rtTls()->exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS;
}

// Discard whatever is pending. Exactly one caller, and it is ECMA-262 7.4.9
// step 6: closing an iterator while a throw is already in flight discards an
// error the iterator's `return` method raises, because the completion already
// on its way out is the one the program is entitled to see. Anywhere else this
// would be a silent swallow.
inline void rtClearException() noexcept {
    rtTls()->exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;
}

// The three ways to raise, all of which RETURN `undefined` so that a helper can
// `return rtThrowTypeError(...)`. That is not a convenience: the caller stores
// the returned value into a GC root slot before it tests the cell, so a helper
// that returned anything the collector cannot parse would put a bad word in a
// live root.
// `AggregateError` (20.5.7) joined with the promise work: `Promise.any`
// rejects with one, and what bronze raises a program must be able to catch
// and name. Its constructor takes (errors, message) — one more leading
// argument than the others — which its own ctor body handles; everything
// else about it is the family pattern.
enum class ErrorKind {
    Error,
    TypeError,
    RangeError,
    SyntaxError,
    ReferenceError,
    // 19.2.6.1.1: what `decodeURI` and its three siblings throw for a
    // malformed escape sequence, and nothing else in the language throws.
    URIError,
    AggregateError,
};

Value rtThrow(Value thrown) noexcept;
Value rtThrowError(ErrorKind kind, const std::string& message);
Value rtThrowTypeError(const std::string& message);
Value rtThrowRangeError(const std::string& message);
// 22.2.3.1 step 4: a pattern that does not parse is a SyntaxError, and it is
// the one such error a running program can produce — a literal's pattern was
// compiled where it was written, so only a pattern built at run time can reach
// here.
Value rtThrowSyntaxError(const std::string& message);
// 13.5.3 / 6.2.5.5: an unresolvable reference that is EVALUATED. Raised from
// `bronze_reference_error`, the one instruction lowering emits for a name it
// could not resolve — never from the runtime's own internals, which have no
// names to fail to resolve.
Value rtThrowReferenceError(const std::string& message);

// The constructor objects, by name, for the provided-global path. `undefined`
// for a name that is not one of them.
Value rtErrorConstructor(const std::string& name);

// A fresh error instance WITHOUT raising it: the value, message set (or left
// to the prototype's empty string when `message` is empty), nothing in the
// pending cell. For an error that is a promise's REJECTION REASON — never
// thrown, so `rtThrowError` is the wrong shape — and for the resolve-cycle
// TypeError, which 27.2.1.3.2 rejects with rather than throws.
Value rtNewErrorValue(ErrorKind kind, const std::string& message);

// Is this value an Error instance — that is, does `Error.prototype` sit on its
// prototype chain? Asked by `console.log`, which prints an error as `Name:
// message` rather than as a plain object. Walking the chain rather than reading
// a header flag keeps error instances on the inline property fast path, which
// only believes flags == HeapKind::Plain.
bool rtIsErrorInstance(Value v);

// `Name: message` into `out`, or just `Name` when the message is empty. bronze
// has no stack to print, which is a deliberate divergence from node.
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
