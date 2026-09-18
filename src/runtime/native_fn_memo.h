#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/value.h"

// A memo over the interned-native table: "give me the builtin function object
// for this code pointer".
//
// The bill it exists for, measured on `many_meshes` (360 frames, 70.94 M
// helper invocations). `bronze_function_singleton` was 10.81 M entries —
// 15.2 %, the third largest line item — and NONE of them came from generated
// code: a compiled module's mention of a function declaration is answered from
// its own fn-slot table (llvm_cache.cpp) and calls the helper once per slot,
// ever. Every one of the 10.81 M came from the RUNTIME's own
// `rtNativeFunction`, which has no module and therefore no slot, so each one
// re-answered an invariant question — "which function object is this code
// pointer's?" — with an unordered_map probe and a cross-module call.
//
// What was asking. Sitting directly above them in the same profile:
// `bronze_prop_get` with `.get` at three sites and `.set` at one, 1.80 M
// entries each — 5,000 a frame, one per mesh. That was three.js's
// `WebGLProperties` / `WebGLAttributes` reading `.get` and `.set` off a
// WeakMap back when a WeakMap answered its members from a hardcoded ladder
// that re-interned the native on every read. Every such receiver is an
// ordinary object with a real prototype now, so the inline cache holds those
// answers and the (kind, key) member memo that once sat beside this one is
// gone; what remains is the runtime's own `rtNativeFunction` calls.
//
// Identity is NOT at risk here, and that is worth saying plainly because
// function identity is observable. Nothing below MERGES two function objects:
// `bronze_function_singleton` already interns on the code pointer and already
// returns one object per native builtin for the life of the thread —
// `m.get === m.get` was true before this file existed. All the memo changes
// is how many instructions it takes to find the object that already exists.
//
// GC: the table holds a code pointer (immortal module text) and an index into
// the runtime's interned-native vector — never a Value. The vector is a root
// source; this is not scanned, and never needs to be. An entry that has gone
// stale (a module unload renumbers the vector) is caught by
// `rtFunctionSingletonAt`'s identity check and refilled.
//
// Seam: BRONZE_NO_FN_SINGLETON_CACHE=1 makes the memo always miss.

namespace bronze::runtime {

// The interned function object for `code`, from the memo when it can be and
// from `bronze_function_singleton` when it cannot. `arity`, `name` and
// `length` are the helper's arguments, used only on the CREATE path — an
// already-interned code pointer ignores them, exactly as the helper does, so
// the first creation names the object for the thread's life. `name` null
// records no name at all (rt_builtins.h says when that is the right call);
// the text is interned as a key only when an object is actually made.
Value rtNativeSingleton(bronze_fn_code code, uint32_t arity, const char* name, uint32_t length);

// BRONZE_NO_FN_SINGLETON_CACHE=1, read through the per-thread ABI block.
bool rtNativeMemoEnabled() noexcept;

// Declared here rather than in rt_state.h because these two are the memo's
// window onto the interned-native vector and have no other caller.
uint32_t rtFunctionSingletonIndexOf(bronze_fn_code code);
Value rtFunctionSingletonAt(uint32_t index, bronze_fn_code expect);

}  // namespace bronze::runtime
