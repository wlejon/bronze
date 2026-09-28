// The out-of-line halves of the inline property paths (bronze_abi.h,
// bronze_prop_poly_scan and the `*_counted` helpers): what a Tier-0 or Tier-1
// body calls where it would otherwise carry the code inline. Tier 2 expands
// the scan back inline (il2mir/il_ic_stub.h), so a hot polymorphic read keeps
// its inline hit.

#include "abi/bronze_abi.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
constexpr uint64_t kOwnDataLimit = uint64_t{1} << 32;

uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace

extern "C" uint64_t bronze_prop_poly_scan(uint64_t objBits, uint64_t* site, uint32_t* counter) {
    if (counter) ++*counter;
    if ((objBits >> 48) != BRONZE_ABI_TAG_OBJECT) return 0;
    const auto* obj = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(objBits & kPayloadMask));
    uint32_t headerLow;
    std::memcpy(&headerLow, obj, sizeof headerLow);
    if (headerLow != kPlainHeaderLow) return 0;
    const uint64_t shape = load64(obj + BRONZE_ABI_OBJ_SHAPE_OFFSET);
    const auto* entries = reinterpret_cast<const uint8_t*>(site);
    for (uint32_t way = 1; way < BRONZE_ABI_IC_WAYS; ++way) {
        const uint8_t* entry = entries + way * BRONZE_ABI_IC_ENTRY_SIZE;
        if (load64(entry + BRONZE_ABI_IC_SHAPE_OFFSET) != shape) continue;
        const uint64_t slot = load64(entry + BRONZE_ABI_IC_SLOT_OFFSET);
        if (slot >= kOwnDataLimit) return 0;
        if (slot < BRONZE_ABI_OBJ_INLINE_SLOTS) {
            return reinterpret_cast<uintptr_t>(obj) + BRONZE_ABI_OBJ_SLOTS_OFFSET + slot * 8;
        }
        const uint64_t overflow = load64(obj + BRONZE_ABI_OBJ_OVERFLOW_OFFSET) & kPayloadMask;
        return overflow + BRONZE_ABI_HDR_BYTES + (slot - BRONZE_ABI_OBJ_INLINE_SLOTS) * 8;
    }
    return 0;
}

extern "C" uint64_t bronze_prop_get_counted(uint64_t objBits, uint32_t keyIndex, uint64_t* icEntry,
                                            uint32_t* counter) {
    if (counter) ++*counter;
    return bronze_prop_get(objBits, keyIndex, icEntry);
}

extern "C" void bronze_prop_set_counted(uint64_t objBits, uint32_t keyIndex, uint64_t value, uint64_t* icEntry,
                                        bool strict, uint32_t* counter) {
    if (counter) ++*counter;
    bronze_prop_set(objBits, keyIndex, value, icEntry, strict);
}
