#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "il/il.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

class DirectMethodTable;

struct SafeConstructorInfo {
    bool isSafe = false;
    std::vector<std::pair<uint32_t, uint32_t>> propArgMap;
};

struct FunctionEffects {
    bool hasUnresolvedCalls = false;
    bool mutatesLength = false;
    // The key indexes the function (or anything it calls directly) sets,
    // sorted and distinct: a module has thousands of keys and functions, and
    // a function writes a handful, so a dense flag per key was most of the
    // pass's time.
    std::vector<uint32_t> writtenKeys;
    std::vector<uint32_t> directCallees;
    bool writes(uint32_t key) const {
        return std::binary_search(writtenKeys.begin(), writtenKeys.end(), key);
    }
};

bool isImpureForLoopPropHoist(const il::Instruction& inst);
bool isArrayIndexKey(const std::string& key);
bool isArrayMutatingMethod(const std::string& name);
bool isArrayReadOnlyMethod(const std::string& name);

SafeConstructorInfo analyzeConstructor(const il::Function& calleeFn);

uint32_t findCalleeIndex(const il::Instruction& inst, const il::Function& fn,
                         const std::vector<DefSite>& defs, const il::Module& module);

bool isFreshLocalObject(il::ValueId val, const il::Function& fn,
                        const std::vector<DefSite>& defs,
                        const std::vector<uint8_t>& inLoop);

bool isNonEscapingLocalObject(il::ValueId val, const il::Function& fn,
                             const std::vector<DefSite>& defs);

bool isKnownPlainObject(il::ValueId val, const il::Function& fn,
                        const std::vector<DefSite>& defs,
                        const il::Module& module,
                        std::vector<std::optional<SafeConstructorInfo>>& ctorCache);

bool isMathObject(il::ValueId val, const il::Function& fn,
                  const std::vector<DefSite>& defs,
                  const il::Module& module);

// What `isAccessorKey` says of a key: kPlainKey, nothing installs an accessor
// under it; kProtoAccessorKey, an accessor may answer it from a prototype or a
// literal (a class getter, a builtin accessor, a reflection-touching
// function's write); kInstanceAccessorKey, a reflection call in the module may
// name it — a string literal or a descriptor map's property in a function that
// reaches `defineProperty` and its kin — so even an instance's OWN data
// property may turn into an accessor after the fact.
inline constexpr uint8_t kPlainKey = 0;
inline constexpr uint8_t kProtoAccessorKey = 1;
inline constexpr uint8_t kInstanceAccessorKey = 2;

// Whether `inst`'s key may be answered by an accessor at its receiver. A site
// with a static slot claim names an own data property, which a prototype's
// accessor cannot answer while no prototype is patched — but which a
// `defineProperty` on the instance can still replace, so the claim does not
// exempt a key a reflection call may name.
inline bool keyMayBeAccessor(const std::vector<uint8_t>& isAccessorKey, uint32_t key, const il::Instruction& inst,
                             bool protoPatchFree) {
    if (key >= isAccessorKey.size() || isAccessorKey[key] == kPlainKey) return false;
    if (isAccessorKey[key] == kInstanceAccessorKey) return true;
    // Only a PROVEN claim says the receiver is the class (il.h staticSlotGuessed).
    return !(protoPatchFree && il::provenStaticSlot(inst));
}

bool isProvablyNonNullish(il::ValueId val, const il::Function& fn,
                          const std::vector<DefSite>& defs,
                          const il::Module& module,
                          std::vector<std::optional<SafeConstructorInfo>>& ctorCache);

std::vector<FunctionEffects> computeModuleEffects(
    const il::Module& module,
    const std::vector<uint8_t>& isAccessorKey,
    std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
    uint32_t lengthKeyIndex,
    const DirectMethodTable* directMethods = nullptr);

bool canMutateProperty(il::ValueId recv, uint32_t keyIndex,
                       const il::Instruction& inst, const il::Function& fn,
                       const std::vector<DefSite>& defs,
                       const std::vector<uint8_t>& inLoop,
                       const il::Module& module,
                       std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                       const std::vector<uint8_t>& isAccessorKey,
                       const std::vector<FunctionEffects>& funcEffects,
                       uint32_t lengthKeyIndex,
                       const DirectMethodTable* directMethods = nullptr,
                       bool protoPatchFree = false,
                       const std::vector<uint8_t>* isMutatedMethodKey = nullptr,
                       const std::vector<std::vector<uint32_t>>* closedKeyTargets = nullptr);

bool canMutateLengthViaCoercion(const il::Instruction& inst, il::ValueId recv,
                                uint32_t lengthKeyIndex, const il::Function& fn,
                                const std::vector<DefSite>& defs,
                                const std::vector<uint8_t>& isAccessorKey,
                                bool protoPatchFree);

void sanitizeLoopInBoundsProofs(il::Module& module);

}  // namespace bronze::lower
