#pragma once

#include <cstdint>
#include <string_view>

#include "runtime/profile.h"

namespace bronze::runtime {

extern bool g_icLogEnabled;

void initIcLog();
void dumpIcLogReport();

void icLogRecordPropGet(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry);
void icLogRecordPropSet(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry, bool strict);
void icLogRecordDynamicCall(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits);
// Why a computed read did NOT come from the elem cache (runtime/elem_ic.cpp).
// Every fall-through path names itself, so the differential the bill is read
// with stays exact: a total that does not move under a change is a change that
// did not reach this cache.
void icLogRecordElemIcMiss(const char* reason, uint64_t objBits, uint64_t keyBits);
// Where a GC root block outgrew its inline storage (runtime/root_slots.h) and
// how far it had got when it did. Not a refusal — a spill is legal and the
// block keeps working — but the only way to know whether kRootSlotsInline and
// kRootBlockInline were chosen well is to see how often the real workload
// passes them, and at what width.
void icLogRecordRootSpill(const char* reason, uint32_t reached);

inline void recordPropGetMiss(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry) {
    if (BRONZE_UNLIKELY(g_icLogEnabled)) {
        icLogRecordPropGet(objBits, keyIndex, icEntry);
    }
}

inline void recordPropSetMiss(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint64_t* icEntry, bool strict) {
    if (BRONZE_UNLIKELY(g_icLogEnabled)) {
        icLogRecordPropSet(objBits, keyIndex, valBits, icEntry, strict);
    }
}

inline void recordDynamicCallMiss(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    if (BRONZE_UNLIKELY(g_icLogEnabled)) {
        icLogRecordDynamicCall(calleeBits, thisBits, argc, argvBits);
    }
}

inline void recordElemIcMiss(const char* reason, uint64_t objBits, uint64_t keyBits) {
    if (BRONZE_UNLIKELY(g_icLogEnabled)) {
        icLogRecordElemIcMiss(reason, objBits, keyBits);
    }
}

}  // namespace bronze::runtime
