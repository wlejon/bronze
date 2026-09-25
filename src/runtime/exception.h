#pragma once

#include <string>
#include <utility>

#include <brass/runtime/exception.hpp>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

// How a JS exception travels, and the `Error` family.
//
// A throw is a C++ `brass::runtime::BrassException` carrying the thrown
// Value's bits. Compiled JS raises the same exception natively (brass's
// `throw`), and a compiled frame's landing pads catch either kind, so one
// exception crosses compiled code, the runtime and host natives alike. A
// runtime helper that calls back into JS therefore does nothing after the
// call to stop on a throw: the exception unwinds it, destructors (Rooted,
// RootedArgs) and all. Only a helper the spec gives a catch (a promise job, an
// iterator close, `finally`-shaped cleanup) catches, with rtTryCatch.
//
// The line this file draws is the reason `fatal` is still the right answer for
// most of the runtime's hard errors: a TypeError ECMA-262 defines becomes a
// catchable throw, and an unimplemented construct or a broken internal
// invariant does not. Letting a program `catch` "bronze has not built this"
// would turn a loud boundary into a silent fallback with extra steps.

namespace bronze::runtime {

// Runs `body` and catches a JS throw out of it: true, with the thrown value in
// `thrown`, when one happened. The value is stored after the catch block has
// ended, never inside it: on MSVC a catch block runs with the thrown-from
// frames still on the stack, so a collection there would walk dead frames.
// `thrown` is the caller's to root (a Rooted's slot) before it allocates.
template <typename Body>
bool rtTryCatch(Body&& body, Value& thrown) {
    uint64_t bits = 0;
    bool threw = false;
    try {
        std::forward<Body>(body)();
    } catch (const brass::runtime::BrassException& e) {
        bits = e.value().raw();
        threw = true;
    }
    if (threw) thrown = Value(bits);
    return threw;
}

// Runs `body`, which steps the iteration record `rec` (an iter.open result),
// and when it throws, closes the iterator before the throw continues: 7.4.9
// IteratorClose with a throw completion, where whatever `return` does is
// discarded and the original throw is the one that leaves.
template <typename Body>
void rtCloseIteratorOnThrow(const Rooted<Value>& rec, Body&& body) {
    Value caught;
    if (!rtTryCatch(std::forward<Body>(body), caught)) return;
    Rooted<Value> thrown{caught};
    bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    throw brass::runtime::BrassException(brass::HostValue::from_raw(thrown.get().rawBits()));
}

// The ways to raise. None of them returns; they are typed `Value` so that a
// helper can `return rtThrowTypeError(...)` from any function returning one.
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

// MSVC warns (C4646) on a noreturn function with a non-void type, which
// these are on purpose.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4646)
#endif
[[noreturn]] Value rtThrow(Value thrown);
[[noreturn]] Value rtThrowError(ErrorKind kind, const std::string& message);
[[noreturn]] Value rtThrowTypeError(const std::string& message);
[[noreturn]] Value rtThrowRangeError(const std::string& message);
// 22.2.3.1 step 4: a pattern that does not parse is a SyntaxError, and it is
// the one such error a running program can produce — a literal's pattern was
// compiled where it was written, so only a pattern built at run time can reach
// here.
[[noreturn]] Value rtThrowSyntaxError(const std::string& message);
// 13.5.3 / 6.2.5.5: an unresolvable reference that is EVALUATED. Raised from
// `bronze_reference_error`, the one instruction lowering emits for a name it
// could not resolve — never from the runtime's own internals, which have no
// names to fail to resolve.
[[noreturn]] Value rtThrowReferenceError(const std::string& message);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// The constructor objects, by name, for the provided-global path. `undefined`
// for a name that is not one of them.
Value rtErrorConstructor(const std::string& name);

// A fresh error instance WITHOUT raising it: the value, message set (or left
// to the prototype's empty string when `message` is empty). For an error that is a promise's REJECTION REASON — never
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
// newline. Shared by rtUncaughtReport and its test.
std::string rtUncaughtText(Value thrown);

// The whole report: an Error instance is inspected with its stack, the way
// node prints one, and anything else is rtUncaughtText. What a program whose
// top level threw prints (rtRunModuleEntry), and what `bronze run` prints for
// the same program, so the entry points report one thing.
std::string rtUncaughtReport(Value thrown);

// Runs a compiled module's entry from C++ and ends the process the way node
// does when its top level throws: the report on STDERR, exit status 1. What
// the standalone main and embed's runMain / runEntry run a module through.
void rtRunModuleEntry(void (*entry)());

}  // namespace bronze::runtime
