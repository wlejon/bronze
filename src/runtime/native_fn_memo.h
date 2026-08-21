#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/value.h"

// Two memos over the same table, for the two shapes of "give me the builtin
// function object for this name".
//
// The bill they exist for, measured on `many_meshes` (360 frames, 70.94 M
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
// entries each — 5,000 a frame, one per mesh. That is three.js's
// `WebGLProperties` / `WebGLAttributes` reading `.get` and `.set` off a
// WeakMap, and a WeakMap is not a plain object, so no inline cache can hold
// the answer and the read walks a hardcoded member ladder to a fresh
// interning every single time. The two line items are one event.
//
// Identity is NOT at risk here, and that is worth saying plainly because
// function identity is observable. Nothing below MERGES two function objects:
// `bronze_function_singleton` already interns on the code pointer and already
// returns one object per native builtin for the life of the thread —
// `m.get === m.get` was true before this file existed. All these memos change
// is how many instructions it takes to find the object that already exists.
//
// GC: both tables hold a code pointer (immortal module text), a key INDEX
// (an integer), and an index into the runtime's interned-native vector — never
// a Value. The vector is a root source; these are not scanned, and never need
// to be. An entry that has gone stale (a module unload renumbers the vector)
// is caught by `rtFunctionSingletonAt`'s identity check and refilled.
//
// Seam: BRONZE_NO_FN_SINGLETON_CACHE=1 makes both memos always miss.

namespace bronze::runtime {

// The interned function object for `code`, from the memo when it can be and
// from `bronze_function_singleton` when it cannot. `arity` and the rest are
// the helper's arguments, used only on the fill path — an already-interned
// code pointer ignores them, exactly as the helper does.
Value rtNativeSingleton(bronze_fn_code code, uint32_t arity);

// The member memo: `(receiver kind, interned key index) -> that same interned
// native`. Answers only for a receiver kind whose member ladder is a C table
// with no object behind it — a Map, Set, WeakMap or WeakSet — and only for a
// receiver carrying no own-property box, so an expando can never be shadowed
// by one of these answers.
//
// `kind` is the receiver's `HeapObjectHeader::flags`. Returns undefined on a
// miss, which is also what it returns for a key whose answer is not a plain
// interned native (`size` is a number; an absent member is a diagnosed error).
Value rtNativeMemberProbe(uint16_t kind, uint32_t keyIndex);
void rtNativeMemberFill(uint16_t kind, uint32_t keyIndex, Value resolved);

// BRONZE_NO_FN_SINGLETON_CACHE=1, read through the per-thread ABI block.
bool rtNativeMemoEnabled() noexcept;

// Declared here rather than in rt_state.h because these two are the memo's
// window onto the interned-native vector and have no other caller.
uint32_t rtFunctionSingletonIndexOf(bronze_fn_code code);
Value rtFunctionSingletonAt(uint32_t index, bronze_fn_code expect);

}  // namespace bronze::runtime
