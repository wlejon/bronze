#include "lower/infer_stats.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace bronze::lower {

namespace {

std::string normalizePath(const std::string& path) {
    std::string norm = path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }
    return norm;
}

struct ReasonEntry {
    std::string reason;
    uint32_t count = 0;
};

std::vector<ReasonEntry> sortedReasons(const std::map<std::string, uint32_t>& reasons) {
    std::vector<ReasonEntry> entries;
    entries.reserve(reasons.size());
    for (const auto& [reason, count] : reasons) {
        entries.push_back({reason, count});
    }
    std::sort(entries.begin(), entries.end(), [](const ReasonEntry& a, const ReasonEntry& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.reason < b.reason;
    });
    return entries;
}

std::string formatPct(uint32_t num, uint32_t denom) {
    if (denom == 0) return "0.0%";
    uint32_t pct10 = static_cast<uint32_t>((static_cast<uint64_t>(num) * 1000 + denom / 2) / denom);
    return std::to_string(pct10 / 10) + "." + std::to_string(pct10 % 10) + "%";
}

void formatCategory(std::string& out, const char* name, const CategoryStats& stats) {
    out += "  ";
    out += name;
    out += ":\n";
    out += "    native: " + std::to_string(stats.nativeCount) + "\n";
    if (stats.staticSlotCount != 0) {
        out += "    of which static-slot: " + std::to_string(stats.staticSlotCount) + "\n";
    }
    if (stats.familySlotCount != 0) {
        out += "    of which family-guarded: " + std::to_string(stats.familySlotCount) + "\n";
    }
    out += "    dynamic: " + std::to_string(stats.dynamicCount) + "\n";
    if (!stats.bailReasons.empty()) {
        out += "    top bail reasons:\n";
        auto sorted = sortedReasons(stats.bailReasons);
        for (const auto& entry : sorted) {
            out += "      " + entry.reason + ": " + std::to_string(entry.count) + "\n";
        }
    }
}

}  // namespace

ModuleInferStats& InferStatsCollector::getOrCreateModule(uint16_t fileId) {
    std::string name;
    if (auto it = fileIdToName_.find(fileId); it != fileIdToName_.end()) {
        name = it->second;
    } else if (sources_ && fileId < sources_->size()) {
        name = normalizePath(std::string(sources_->at(fileId).name()));
        fileIdToName_[fileId] = name;
    } else {
        name = "file_" + std::to_string(fileId);
        fileIdToName_[fileId] = name;
    }

    auto it = modules_.find(name);
    if (it == modules_.end()) {
        ModuleInferStats mod;
        mod.moduleName = name;
        auto [inserted, _] = modules_.emplace(name, std::move(mod));
        return inserted->second;
    }
    return it->second;
}

void InferStatsCollector::recordStaticSlot(uint16_t fileId, bool family) {
    auto& props = getOrCreateModule(fileId).propertyAccesses;
    ++props.staticSlotCount;
    if (family) ++props.familySlotCount;
}

void InferStatsCollector::recordPropertyAccess(uint16_t fileId, bool isNative,
                                               const std::string& bailReason) {
    auto& mod = getOrCreateModule(fileId);
    if (isNative) {
        ++mod.propertyAccesses.nativeCount;
    } else {
        ++mod.propertyAccesses.dynamicCount;
        if (!bailReason.empty()) {
            ++mod.propertyAccesses.bailReasons[bailReason];
        }
    }
}

void InferStatsCollector::recordCall(uint16_t fileId, bool isNative,
                                     const std::string& bailReason) {
    auto& mod = getOrCreateModule(fileId);
    if (isNative) {
        ++mod.calls.nativeCount;
    } else {
        ++mod.calls.dynamicCount;
        if (!bailReason.empty()) {
            ++mod.calls.bailReasons[bailReason];
        }
    }
}

void InferStatsCollector::recordClassLayouts(uint32_t proven, uint32_t familyMembers,
                                             uint32_t familyRoots,
                                             const std::map<std::string, uint32_t>& refusals) {
    classesProven_ = proven;
    classFamilyMembers_ = familyMembers;
    classFamilyRoots_ = familyRoots;
    classRefusals_ = refusals;
}

void InferStatsCollector::recordElementOp(uint16_t fileId, bool isNative,
                                          const std::string& bailReason) {
    auto& mod = getOrCreateModule(fileId);
    if (isNative) {
        ++mod.elementOps.nativeCount;
    } else {
        ++mod.elementOps.dynamicCount;
        if (!bailReason.empty()) {
            ++mod.elementOps.bailReasons[bailReason];
        }
    }
}

void InferStatsCollector::recordFieldAudit(
    const types::InferenceResult::FieldAuditReport& report, uint32_t provenReadSites) {
    fieldAudit_ = report;
    fieldProvenReads_ = provenReadSites;
}

std::string InferStatsCollector::format() const {
    std::string out = "=== Inference Statistics ===\n";

    CategoryStats totalProps;
    CategoryStats totalCalls;
    CategoryStats totalElems;

    uint32_t refusedClasses = 0;
    for (const auto& [reason, count] : classRefusals_) {
        (void)reason;
        refusedClasses += count;
    }
    if (classesProven_ != 0 || refusedClasses != 0) {
        out += "\nClass Layouts: " + std::to_string(classesProven_) + " proven, " +
               std::to_string(refusedClasses) + " refused (" +
               formatPct(classesProven_, classesProven_ + refusedClasses) + " proven)\n";
        if (classFamilyMembers_ != 0) {
            out += "  layout families: " + std::to_string(classFamilyRoots_) + " roots over " +
                   std::to_string(classFamilyMembers_) + " classes\n";
        }
        for (const auto& entry : sortedReasons(classRefusals_)) {
            out += "  " + entry.reason + ": " + std::to_string(entry.count) + "\n";
        }
    }

    if (fieldAudit_.namesWritten != 0) {
        const auto& fa = fieldAudit_;
        out += "\nField-Type Audit: " + std::to_string(fa.namesClean) + " of " +
               std::to_string(fa.namesWritten) +
               " written property names hold only numbers (" +
               formatPct(fa.namesClean, fa.namesWritten) + ")\n";
        out += "  number field reads: " + std::to_string(fa.numberFieldReads) + " sites, " +
               std::to_string(fieldProvenReads_) + " proven; refused " +
               std::to_string(fa.refusedNotBuiltHere) + " receiver not built here, " +
               std::to_string(fa.refusedByClass) + " class layout, " +
               std::to_string(fa.refusedByAudit) + " write audit\n";
        for (const auto& entry : sortedReasons(fa.globalRefusals)) {
            out += "  EVERY name refused, " + entry.reason + ": " +
                   std::to_string(entry.count) + " sites\n";
        }
        if (!fa.globalRefusals.empty()) {
            out += "  names with no refusal of their own: " +
                   std::to_string(fa.namesLocallyClean) + "\n";
        }
        for (const auto& entry : sortedReasons(fa.refusals)) {
            out += "  " + entry.reason + ": " + std::to_string(entry.count) + "\n";
        }
    }

    for (const auto& [name, mod] : modules_) {
        out += "\nModule: " + name + "\n";
        formatCategory(out, "Property Accesses", mod.propertyAccesses);
        formatCategory(out, "Calls", mod.calls);
        formatCategory(out, "Element Operations", mod.elementOps);

        totalProps.nativeCount += mod.propertyAccesses.nativeCount;
        totalProps.staticSlotCount += mod.propertyAccesses.staticSlotCount;
        totalProps.familySlotCount += mod.propertyAccesses.familySlotCount;
        totalProps.dynamicCount += mod.propertyAccesses.dynamicCount;
        totalCalls.nativeCount += mod.calls.nativeCount;
        totalCalls.dynamicCount += mod.calls.dynamicCount;
        totalElems.nativeCount += mod.elementOps.nativeCount;
        totalElems.dynamicCount += mod.elementOps.dynamicCount;
    }

    out += "\nTotal:\n";
    out += "  Property Accesses: " + std::to_string(totalProps.nativeCount) + " native, " +
           std::to_string(totalProps.dynamicCount) + " dynamic (" +
           formatPct(totalProps.nativeCount, totalProps.total()) + " native)\n";
    out += "  Calls:             " + std::to_string(totalCalls.nativeCount) + " native, " +
           std::to_string(totalCalls.dynamicCount) + " dynamic (" +
           formatPct(totalCalls.nativeCount, totalCalls.total()) + " native)\n";
    out += "  Element Operations: " + std::to_string(totalElems.nativeCount) + " native, " +
           std::to_string(totalElems.dynamicCount) + " dynamic (" +
           formatPct(totalElems.nativeCount, totalElems.total()) + " native)\n";

    return out;
}

}  // namespace bronze::lower
