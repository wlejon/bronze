#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "il/il.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

struct SafeConstructorInfo {
    bool isSafe = false;
    std::vector<std::pair<uint32_t, uint32_t>> propArgMap;
};

struct FunctionEffects {
    bool hasUnresolvedCalls = false;
    bool mutatesLength = false;
    std::vector<uint8_t> writtenKeys;
    std::vector<uint32_t> directCallees;
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

bool isProvablyNonNullish(il::ValueId val, const il::Function& fn,
                          const std::vector<DefSite>& defs,
                          const il::Module& module,
                          std::vector<std::optional<SafeConstructorInfo>>& ctorCache);

std::vector<FunctionEffects> computeModuleEffects(
    const il::Module& module,
    const std::vector<uint8_t>& isAccessorKey,
    std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
    uint32_t lengthKeyIndex);

bool canMutateProperty(il::ValueId recv, uint32_t keyIndex,
                       const il::Instruction& inst, const il::Function& fn,
                       const std::vector<DefSite>& defs,
                       const std::vector<uint8_t>& inLoop,
                       const il::Module& module,
                       std::vector<std::optional<SafeConstructorInfo>>& ctorCache,
                       const std::vector<uint8_t>& isAccessorKey,
                       const std::vector<FunctionEffects>& funcEffects,
                       uint32_t lengthKeyIndex);

}  // namespace bronze::lower
