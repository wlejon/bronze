#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/rt_builtins.h"
#include "runtime/value.h"

namespace bronze::runtime {

// A callee the RUNTIME calls many times in one operation: a sort's comparator,
// a `forEach` body, a JSON replacer.
//
// The generic route for those is `bronze_dynamic_call`, and per element it
// re-answers a question that cannot have changed since the last element: is
// this value an object, is it a function rather than a proxy, does its arity
// fit the argument count. A comparator cannot become a proxy between two
// comparisons — the sort holds the one value it was given — so the ladder is
// re-run 54,000 times a frame on three.js's render list for one answer.
//
// `DirectCallee` asks once. What it may NOT do is cache the answer as a
// pointer: the collector moves function objects, and `code`/`env_record` read
// before an allocating call are addresses from before it. So the binding is a
// FACT (this value is an ordinary function whose arity fits), and the two
// fields are re-read through the caller's root on every call — two loads,
// against a guard ladder plus an out-of-line helper entry.
//
// The `NewTargetScope(undefined)` push is the other half, and the expensive
// half — a rooted slot and a linked-list link, per call, measured at about
// 5.5 ns of the 66 a three.js render-list comparison costs. It moves OUT of
// the per-element loop and around the whole operation, which the caller does
// (see builtin_array_sort.cpp) rather than this class, because the scope's
// extent is a fact about the operation and not about one call.
//
// That is a real change and not a free one, so it is spelled out. The old
// shape pushed and POPPED per call, so anything the runtime ran between two
// calls — `ToNumber` of a comparator's answer, which can reach a user
// `valueOf` — saw whatever new.target was ambient outside the sort. Under one
// hoisted scope it sees `undefined`. Undefined is what 13.3.12 says a plain
// call's new.target is, and every one of these calls IS a plain call, so the
// hoisted answer is the correct one; the per-call shape was reporting a
// constructor's new.target to a function the constructor never constructed.
// The hoist is therefore NOT under the seam — the caller does it
// unconditionally — so `BRONZE_NO_DIRECT_CALLOUT=1` measures dispatch and
// nothing else, and both settings answer identically. `array_sort_callout`
// pins the answer.
//
// Seam: BRONZE_NO_DIRECT_CALLOUT=1 makes `bind` always refuse, so every
// call-out falls back to `bronze_dynamic_call` and one binary can A/B.
class DirectCallee {
public:
    // True when `callee` is an ordinary function object that `argc` arguments
    // reach without padding — the exact condition under which
    // `bronze_dynamic_call` would have called `fn->code` directly. Everything
    // else (a proxy, a non-function, a callee needing under-arity padding)
    // answers false, and the caller keeps the helper for every element.
    //
    // `arity == 0` is the variadic native's marking, not "takes nothing":
    // fn.h explains why, and the helper reads it the same way.
    bool bind(Value callee, uint32_t argc) noexcept {
        bound_ = false;
        if (!directCalloutEnabled()) return false;
        if (!callee.isObject()) return false;
        HeapObjectHeader* hdr = callee.asObject<HeapObjectHeader>();
        if (hdr->flags != HeapKind::Function) return false;
        const FunctionHeader* fn = reinterpret_cast<const FunctionHeader*>(hdr);
        if (!fn->code) return false;
        if (fn->arity != 0 && argc < fn->arity) return false;
        bound_ = true;
        return true;
    }

    bool bound() const noexcept { return bound_; }

    // `calleeRoot` must be the SAME value `bind` accepted, held in a root
    // across every allocation since — which is what makes re-reading the two
    // header fields here safe rather than merely usual.
    uint64_t call(Rooted<Value>& calleeRoot, uint64_t thisBits, uint32_t argc,
                  const uint64_t* argv) const {
        FunctionHeader* fn = calleeRoot.get().asObject<FunctionHeader>();
        return fn->code(fn->env_record.rawBits(), thisBits, argc, argv);
    }

private:
    static bool directCalloutEnabled() noexcept {
        return bronze_tls_block_addr()->direct_callout_enabled != 0;
    }

    bool bound_ = false;
};

}  // namespace bronze::runtime
