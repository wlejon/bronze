#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#define BRONZE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BRONZE_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define BRONZE_UNLIKELY(x) (x)
#define BRONZE_LIKELY(x) (x)
#endif

namespace bronze::runtime {

// Global fast-path flag, read once at startup from BRONZE_PROFILE=1.
extern bool g_profileEnabled;

void initProfile();
void dumpProfileReport();

void profileRecordHelper(const char* helperName);
void profileRecordProp(const char* helperName, uint32_t keyIndex, const void* icSite);
void profileRecordElem(const char* helperName, uint64_t objBits, uint64_t idxBits);
void profileRecordCall(const char* helperName, uint64_t calleeBits);

// How a callee with no `name` is spelled in the report. A native builtin and a
// host function both answer NULL to `FunctionHeader::name` (fn.h explains why
// that is not the empty string), so the profile used to collapse every one of
// them into a single "fn (native/unnamed)" row — which is the row the three.js
// bill could not read. The runtime names what it can from the code pointer;
// anything layered above it (embed's host trampoline, which is ONE code
// pointer shared by every host function) installs a namer that can tell its
// own callees apart. Returns false to decline, and MUST NOT ALLOCATE on the JS
// heap: it runs at a helper entry with a raw receiver in hand.
using ProfileCalleeNamer = bool (*)(uint64_t calleeBits, void* code, char* out, size_t outSize);
void profileSetCalleeNamer(ProfileCalleeNamer namer);

// A name for one native code pointer, recorded once at installation. The
// runtime's own builtins are installed through a single factory that is handed
// the property key a moment later, so this is the one call that turns
// `fn (native @0x7ffd...)` into `Math.cos`.
void profileNameNative(const void* code, std::string_view owner, std::string_view member);

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

inline void recordElemCall(const char* helperName, uint64_t objBits, uint64_t idxBits) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordElem(helperName, objBits, idxBits);
    }
}

inline void recordCallSite(const char* helperName, uint64_t calleeBits) {
    if (BRONZE_UNLIKELY(g_profileEnabled)) {
        profileRecordCall(helperName, calleeBits);
    }
}

}  // namespace bronze::runtime
