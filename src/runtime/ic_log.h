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

}  // namespace bronze::runtime
