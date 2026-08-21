#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "support/source.h"

namespace bronze::lower {

struct CategoryStats {
    uint32_t nativeCount = 0;
    uint32_t dynamicCount = 0;
    // Of `nativeCount`, how many carried the stronger LAYOUT proof and compiled
    // to a constant-offset access. Reported separately because the two are
    // different claims with different consequences: an identity proof is
    // counted by the backend's cache, a layout proof changes what the backend
    // emits (property accesses only; the other categories leave it zero).
    uint32_t staticSlotCount = 0;
    // Bail reason -> occurrences count
    std::map<std::string, uint32_t> bailReasons;

    uint32_t total() const { return nativeCount + dynamicCount; }
};

struct ModuleInferStats {
    std::string moduleName;
    CategoryStats propertyAccesses;
    CategoryStats calls;
    CategoryStats elementOps;
};

class InferStatsCollector {
public:
    InferStatsCollector() = default;

    void setSourceSet(const SourceSet* sources) { sources_ = sources; }

    void recordPropertyAccess(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    // A site that took the constant-offset form. Always a subset of the native
    // ones (a layout proof implies the identity proof), and recorded from the
    // stamping step rather than passed to the call above, because the two
    // decisions are made at different points in lowering a site.
    void recordStaticSlot(uint16_t fileId);
    void recordCall(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordElementOp(uint16_t fileId, bool isNative, const std::string& bailReason = "");

    // The class-layout verdicts, whole-program rather than per module: a class
    // is proven or refused once, wherever its declaration was written.
    void recordClassLayouts(uint32_t proven, const std::map<std::string, uint32_t>& refusals);

    std::string format() const;

private:
    ModuleInferStats& getOrCreateModule(uint16_t fileId);

    const SourceSet* sources_ = nullptr;
    // Map from normalized module name to ModuleInferStats for deterministic ordering
    std::map<std::string, ModuleInferStats> modules_;
    std::map<uint16_t, std::string> fileIdToName_;
    uint32_t classesProven_ = 0;
    std::map<std::string, uint32_t> classRefusals_;
};

}  // namespace bronze::lower
