#pragma once

// The shape-flow census (BRONZE_SHAPE_CENSUS=1): per property-access site,
// which receiver shapes actually flow through it, at what poly degree, and
// how much of its value traffic is numbers — the input contract for a future
// whole-program monomorphizer. Schema and field documentation:
// docs/shape-census.md (versioned, v0).
//
// Mechanism: census mode disables every inline-cache LATCH in the runtime —
// property-IC fills (object.cpp, rt_prop_absent.cpp), the array-method
// sentinel, the method-call IC latch, the elem cache, static-shape publishes
// and family stamps — so every property access that would have hit an inline
// fast path misses into its runtime helper instead, where the census records
// it. That makes the instrument runtime-only: no codegen change, no new ABI
// surface, and complete coverage of monomorphic-hit traffic that a
// miss-logging instrument (BRONZE_IC_LOG) structurally cannot see. The price
// is heavy slowdown in census mode, which is acceptable and reported; with
// the env var unset every hook is one predicted branch on a global.
//
// Sites are identified by their IC-site address (module data, one per
// compiled source site) where the helper receives one, else by the helper
// call's return address. Function attribution resolves those addresses
// through the modules' PDBs at dump time (symbolize.h), so census rows and
// sampler rows spell function names identically.

#include <cstdint>

// The helper call's return address is the site's function attribution — the
// PC lands inside the compiled JS function that owns the access. MSVC spells
// the intrinsic _ReturnAddress (declared here so hot helper files need not
// pull in <intrin.h>); GCC/Clang spell it __builtin_return_address.
#if defined(_MSC_VER)
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
#define BRONZE_CENSUS_RET_ADDR() _ReturnAddress()
#else
#define BRONZE_CENSUS_RET_ADDR() __builtin_return_address(0)
#endif

namespace bronze::runtime {

extern bool g_shapeCensusEnabled;

enum class CensusKind : uint8_t {
    PropGet = 0,
    PropSet = 1,
    ElemGet = 2,
    ElemSet = 3,
    MethodGet = 4,
    SuperGet = 5,
    SuperSet = 6,
};

// Opaque per-record token handed back so the RESULT of a read can be
// classified after the underlying operation ran. Null when the census is
// disabled or the record was skipped.
using CensusToken = void*;

// Records one access at `siteId` (the IC entry / method-IC site when the
// helper has one, else null and `retaddr` keys the site). For writes,
// `hasValue` is true and `valBits` is the stored value; for reads the value
// arrives via censusRecordResult. `keyBits` carries a computed key
// (BRONZE_ABI_KEY_NONE-style sites pass keyIndex = UINT32_MAX and the key
// value instead). Must be called BEFORE the underlying operation: the
// receiver may move under any allocating call, and the shape is read here.
CensusToken censusRecordAccess(CensusKind kind, uint64_t objBits, uint32_t keyIndex,
                               uint64_t keyBits, const void* siteId, const void* retaddr,
                               bool hasValue, uint64_t valBits);

// Classifies a read's result tag against the record `tok` names. Safe on a
// null token.
void censusRecordResult(CensusToken tok, uint64_t resultBits);

// True while census mode must keep an IC latch from being written. Reads one
// global; callable from any latch site.
inline bool censusFillsSuppressed();

// bronze_call_method fetches the method through bronze_prop_get with a stack
// scratch site; that inner get is the METHOD-IC site's traffic, not a
// property site of its own, and recording it under a stack address would
// mint one census row per call. The method helper brackets its inner get
// with these.
void censusEnterNested();
void censusLeaveNested();
bool censusInNested();

// Definition below rather than in the class of one-liner .cpps: the latch
// sites that consult this are hot paths in NORMAL mode too, and the whole
// check must stay a load and a test.
inline bool censusFillsSuppressed() { return g_shapeCensusEnabled; }

}  // namespace bronze::runtime
