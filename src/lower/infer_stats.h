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
    void recordCall(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordElementOp(uint16_t fileId, bool isNative, const std::string& bailReason = "");

    std::string format() const;

private:
    ModuleInferStats& getOrCreateModule(uint16_t fileId);

    const SourceSet* sources_ = nullptr;
    // Map from normalized module name to ModuleInferStats for deterministic ordering
    std::map<std::string, ModuleInferStats> modules_;
    std::map<uint16_t, std::string> fileIdToName_;
};

}  // namespace bronze::lower
