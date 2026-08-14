#pragma once

#include <cstdint>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#define BRONZE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BRONZE_UNLIKELY(x) (x)
#endif

namespace bronze::runtime {

// Global fast-path flag, read once at startup from BRONZE_PROFILE=1.
extern bool g_profileEnabled;

void initProfile();
void dumpProfileReport();

void profileRecordHelper(const char* helperName);
void profileRecordProp(const char* helperName, uint32_t keyIndex, const void* icSite);
void profileRecordElem(const char* helperName);
void profileRecordCall(const char* helperName, uint64_t calleeBits);

inline void recordHelperCall(const char* helperName) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordHelper(helperName);
    }
}

inline void recordPropCall(const char* helperName, uint32_t keyIndex, const void* icSite) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordProp(helperName, keyIndex, icSite);
    }
}

inline void recordElemCall(const char* helperName) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordElem(helperName);
    }
}

inline void recordCallSite(const char* helperName, uint64_t calleeBits) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordCall(helperName, calleeBits);
    }
}

}  // namespace bronze::runtime
