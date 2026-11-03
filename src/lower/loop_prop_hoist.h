#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::lower {

class DirectMethodTable;

// What the module says of each key and function before any read is moved:
// `isAccessorKey` (loop_prop_analysis.h kPlainKey / kProtoAccessorKey /
// kInstanceAccessorKey) by key index, which functions touch reflection
// (`defineProperty` and its kin, `Proxy`) themselves, and the index of
// `length`, UINT32_MAX when no key names it.
struct AccessorKeyFacts {
    std::vector<uint8_t> isAccessorKey;
    std::vector<uint8_t> fnTouchesReflection;
    uint32_t lengthKeyIndex = UINT32_MAX;
};
AccessorKeyFacts computeAccessorKeyFacts(const il::Module& module);

// Hoists loop-invariant property reads out of pure loops into their preheaders.
bool hoistLoopInvariantProps(il::Module& module, const DirectMethodTable* directMethods = nullptr);

}  // namespace bronze::lower
