#pragma once

// Coroutine bodies at the runtime's end: a generator, an async function, an
// async generator or an awaiting top level runs as a compiled body on a brass
// coroutine FRAME, and the objects here drive it.
//
// The frame is held as an Object-tagged Value (a heap object brass laid out),
// in the internal slot of the object that owns it — the generator object, the
// async function's machine, the async generator object — so it lives exactly
// as long as that owner and the collector traces what it holds. While a body
// runs, brass's resume keeps it rooted.
//
// A resumption hands the body a mode (next / throw / return) and a value, and
// comes back with the body's completion or its next suspension: which kind
// (BRONZE_ABI_SUSPEND_*) and with what value. A throw out of the body leaves
// rtCoroResume as the JS throw it is.

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/value.h"

namespace bronze::runtime {

struct CoroStep {
    bool done = false;
    uint32_t kind = 0;  // BRONZE_ABI_SUSPEND_*, when not done
};

// Called once the owner's slot holds `frame`: takes the frame out of brass's
// roots, so that it lives exactly as long as its owner.
void rtCoroAdopt(Rooted<Value>& frame);

// One resumption of `frame`. `out` receives the returned or the suspension's
// value.
CoroStep rtCoroResume(Rooted<Value>& frame, uint32_t mode, Rooted<Value>& sent,
                      Rooted<Value>& out);

// 27.7.5.3 Await's subscription for `owner` (an async function's machine or
// an async generator object): PromiseResolve on `awaited`, then its settling
// resumes the owner's body with next or throw. False, with the exception in
// `thrown`, when PromiseResolve itself threw — which the body is to receive as
// a throw at the await.
bool rtCoroAwait(Rooted<Value>& owner, Rooted<Value>& awaited, Rooted<Value>& thrown);

// The two owners an await resumes (builtin_async.cpp,
// builtin_async_generator.cpp).
Value rtStartAsyncFunction(Rooted<Value>& frame);
void rtAsyncFunctionResume(Rooted<Value>& machine, uint32_t mode, Rooted<Value>& sent);
Value rtCreateAsyncGeneratorObject(Rooted<Value>& frame);
void rtAsyncGeneratorResumeFromAwait(Rooted<Value>& gen, uint32_t mode, Rooted<Value>& sent);

}  // namespace bronze::runtime
