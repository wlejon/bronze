// The per-thread A/B seams: every BRONZE_NO_* switch that lowers an inline
// fast path's enable flag, read once per thread at its first touch of the
// runtime (Heap's constructor, reached from rtHeap), before any generated code
// on that thread can read a flag through a helper.

#include <cstdlib>
#include <cstring>

#include "abi/bronze_abi.h"
#include "runtime/elem_ic.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

namespace {

bool envIsOne(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

}  // namespace

void rtReadThreadSeams() {
    // The inline fast-path enable flags default to 1 in the TLS block.
    bronze_tls_block* tls = bronze_tls_block_addr();
    if (envIsOne("BRONZE_NO_INLINE_CALL")) tls->inline_call_enabled = 0;
    if (envIsOne("BRONZE_NO_ARRAY_METHOD_IC")) tls->array_method_ic_enabled = 0;
    if (envIsOne("BRONZE_NO_INLINE_OVERFLOW_SET")) tls->inline_overflow_set_enabled = 0;
    if (envIsOne("BRONZE_NO_INLINE_ACCESSOR")) tls->inline_accessor_enabled = 0;
    if (envIsOne("BRONZE_NO_POLY_IC")) tls->poly_ic_enabled = 0;
    if (envIsOne("BRONZE_NO_NEG_IC")) tls->negative_ic_enabled = 0;
    if (envIsOne("BRONZE_NO_ELEM_IC")) tls->elem_ic_enabled = 0;

    // BRONZE_NO_ELEM_SET_IC, read by elem_ic.cpp because its flag is not in the
    // ABI block, but read HERE so every seam is settled at one first touch.
    elemSetCacheReadSeam();

    // BRONZE_NO_FN_STATICS_IC, read here for the same reason: the flag lives in
    // rt_prop_function.cpp rather than in the ABI's TLS block.
    fnStaticsIcReadSeam();

    if (envIsOne("BRONZE_NO_DIRECT_CALLOUT")) tls->direct_callout_enabled = 0;
    if (envIsOne("BRONZE_NO_ELEM_ABSENT")) tls->elem_absent_enabled = 0;

    // The string-key identity latch, latch-side: with this off no fill or hit
    // ever writes a non-zero key_ident, so the inline string arm can only
    // miss into the helper it always took (elem_ic.h).
    if (envIsOne("BRONZE_NO_ELEM_KEY_IC")) tls->elem_key_ic_enabled = 0;

    // The undefined-vs-number relational arm: with this off, a compare whose
    // operand is `undefined` keeps the bronze_rel_* helper.
    if (envIsOne("BRONZE_NO_UNDEF_REL")) tls->undef_rel_enabled = 0;

    // Array.prototype.sort's hoisted-roots merge engine: with this off the
    // sort keeps the per-comparison Rooted churn (builtin_array_sort.cpp).
    if (envIsOne("BRONZE_NO_SORT_FAST")) tls->sort_fast_enabled = 0;

    // The allocation-free Map/WeakMap lookup probe: with this off every
    // `get`/`has` runs the full rooted prologue (map.cpp,
    // builtin_weak_map.cpp).
    if (envIsOne("BRONZE_NO_MAP_FAST")) tls->map_fast_enabled = 0;

    // %TypedArray%.prototype.set's number-elements fast loop over a plain
    // array source: with this off every element keeps its rooted spec-shaped
    // iteration (builtin_typed_array_methods.cpp).
    if (envIsOne("BRONZE_NO_TA_SET_FAST")) tls->ta_set_fast_enabled = 0;

    // The inline truthiness arms for bool/undefined/null/object operands:
    // with this off only the number arm stays inline and every other operand
    // keeps the bronze_unbox_bool helper.
    if (envIsOne("BRONZE_NO_TRUTHY_INLINE")) tls->truthy_inline_enabled = 0;

    if (envIsOne("BRONZE_NO_FN_SINGLETON_CACHE")) tls->fn_singleton_cache_enabled = 0;
    if (envIsOne("BRONZE_NO_ITER_FAST")) tls->iter_fast_enabled = 0;
    if (envIsOne("BRONZE_NO_INLINE_ROOTS")) tls->inline_roots_enabled = 0;
    if (envIsOne("BRONZE_NO_STRICT_EQ_INLINE")) tls->strict_eq_inline_enabled = 0;

    // Two ways to lower the inline elem probe off, and the second is not a
    // convenience: with the TABLE off nothing is ever installed, so an inline
    // probe could only miss.
    if (envIsOne("BRONZE_NO_ELEM_INLINE") || tls->elem_ic_enabled == 0) {
        tls->elem_inline_enabled = 0;
    }

    const char* noMethodCallIc = std::getenv("BRONZE_NO_METHOD_CALL_IC");
    if (!noMethodCallIc) noMethodCallIc = std::getenv("BRONZE_NO_CALL_IC");
    if (noMethodCallIc && std::strcmp(noMethodCallIc, "1") == 0) tls->method_call_ic_enabled = 0;

    // Narrower than the switch above: the method IC stays, but latches only
    // the env-free direct entries (rt_state.h, rtSetEnvMethodIcEnabled).
    if (envIsOne("BRONZE_NO_ENV_METHOD_IC")) rtSetEnvMethodIcEnabled(false);

    // Narrower still: the method IC keeps every plain-receiver form, but never
    // latches the exotic-receiver (Array/collection) entries
    // (rt_state.h, rtSetExoticMethodIcEnabled).
    if (envIsOne("BRONZE_NO_EXOTIC_METHOD_IC")) rtSetExoticMethodIcEnabled(false);

    // And narrower again: way-0 latching keeps every form, but a displaced
    // plain-direct entry is dropped instead of moved to way 1
    // (rt_state.h, rtSetPolyMethodIcEnabled).
    if (envIsOne("BRONZE_NO_POLY_METHOD_IC")) rtSetPolyMethodIcEnabled(false);

    // Shape-census mode (BRONZE_SHAPE_CENSUS=1, runtime/shape_census.h):
    // every latch the runtime can reach through a TLS word goes down, so all
    // property traffic keeps missing into the helpers that record it. The
    // remaining latches consult censusFillsSuppressed() at their own sites.
    if (envIsOne("BRONZE_SHAPE_CENSUS")) {
        tls->elem_ic_enabled = 0;
        tls->elem_inline_enabled = 0;
        tls->elem_key_ic_enabled = 0;
        tls->elem_absent_enabled = 0;
        tls->array_method_ic_enabled = 0;
        tls->method_call_ic_enabled = 0;
    }

    // The computed-read cache's table address, published where the seam that
    // gates reading it is set, so a thread never has one without the other.
    elemCachePublish();
}

}  // namespace bronze::runtime
