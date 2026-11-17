#include "codegen-brass/size_report.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string_view>
#include <unordered_map>

#include "codegen-brass/brass_backend_sections.h"

namespace bronze::codegen {

namespace {

bool g_sizeReport = false;

constexpr std::string_view kTwinSuffix = "__twin";

const char* heatName(il::Function::Heat h) {
    switch (h) {
        case il::Function::Heat::Unknown: return "unknown";
        case il::Function::Heat::Never: return "never";
        case il::Function::Heat::Entered: return "entered";
        case il::Function::Heat::Hot: return "hot";
    }
    return "?";
}

struct Row {
    size_t functions = 0;
    size_t code = 0;
    size_t stackMap = 0;
    size_t pcTable = 0;
    size_t inlineFrames = 0;
    size_t total() const { return code + stackMap + pcTable + inlineFrames; }
};

double mb(size_t bytes) { return static_cast<double>(bytes) / 1e6; }

// The symbol with the digits of `name` folded, so the per-file and
// per-segment tables group.
std::string groupName(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c >= '0' && c <= '9') {
            if (out.empty() || out.back() != '#') out.push_back('#');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

bool sizeReportEnabled() { return g_sizeReport; }
void setSizeReportEnabled(bool on) { g_sizeReport = on; }

void printSizeReport(const brass::object::ObjectFile& obj, const il::Module& module,
                     const std::vector<std::string>& uniqueNames, const std::string& entrySymbol) {
    std::fprintf(stderr, "size report:\n  sections:");
    for (const auto& sec : obj.sections) {
        if (sec.data.size() < 1024) continue;
        std::fprintf(stderr, " %s %.2f MB (%zu relocations);", sec.name.c_str(), mb(sec.data.size()),
                     sec.relocations.size());
    }
    std::fputc('\n', stderr);

    std::unordered_map<std::string, uint32_t> fnOfName;
    for (uint32_t i = 0; i < uniqueNames.size(); ++i) fnOfName.emplace(uniqueNames[i], i);
    std::unordered_map<std::string, size_t> symSize;
    for (const auto& s : obj.symbols) {
        if (s.section_index >= 0) symSize[s.name] = s.size;
    }
    const auto take = [&](const std::string& name) -> size_t {
        auto it = symSize.find(name);
        if (it == symSize.end()) return 0;
        const size_t n = it->second;
        symSize.erase(it);
        return n;
    };

    struct Fn {
        std::string name;
        std::string kind;
        il::Function::Heat heat = il::Function::Heat::Unknown;
        uint64_t entries = 0;
        Row row;
    };
    std::vector<Fn> fns;
    std::map<std::pair<std::string, int>, Row> byKind;
    for (const auto& cfi : obj.functions) {
        Fn f;
        f.name = cfi.name;
        std::string base = cfi.name;
        const bool twin = base.size() > kTwinSuffix.size() && base.ends_with(kTwinSuffix);
        if (twin) base.resize(base.size() - kTwinSuffix.size());
        auto it = fnOfName.find(base);
        if (it == fnOfName.end()) {
            f.kind = "thunk";
        } else {
            const il::Function& fn = module.functions[it->second];
            f.heat = fn.heat;
            f.entries = fn.harvestEntries;
            if (twin) f.kind = "G twin";
            else if (fn.isOptimistic()) f.kind = "S+ $opt";
            else if (fn.isSpeculated()) f.kind = "S $spec";
            else if (fn.isEntryPoint || base.starts_with("main")) f.kind = "main/seg";
            else f.kind = "G";
        }
        take(cfi.name);
        f.row.functions = 1;
        f.row.code = cfi.text_size;
        f.row.stackMap = take(moduleSymbolName(entrySymbol, "__bronze_stack_map_" + cfi.name));
        f.row.pcTable = take(moduleSymbolName(entrySymbol, "__bronze_pc_table_" + cfi.name));
        f.row.inlineFrames = take(moduleSymbolName(entrySymbol, "__bronze_inline_frames_" + cfi.name));
        Row& r = byKind[{f.kind, static_cast<int>(f.heat)}];
        r.functions += 1;
        r.code += f.row.code;
        r.stackMap += f.row.stackMap;
        r.pcTable += f.row.pcTable;
        r.inlineFrames += f.row.inlineFrames;
        fns.push_back(std::move(f));
    }

    std::fprintf(stderr, "  functions by copy and harvest heat (MB):\n");
    std::fprintf(stderr, "    %-10s %-8s %7s %8s %8s %8s %8s %8s\n", "copy", "heat", "count", "code", "stackmap",
                 "pctable", "inlfr", "total");
    Row all;
    for (const auto& [key, r] : byKind) {
        std::fprintf(stderr, "    %-10s %-8s %7zu %8.2f %8.2f %8.2f %8.2f %8.2f\n", key.first.c_str(),
                     heatName(static_cast<il::Function::Heat>(key.second)), r.functions, mb(r.code),
                     mb(r.stackMap), mb(r.pcTable), mb(r.inlineFrames), mb(r.total()));
        all.functions += r.functions;
        all.code += r.code;
        all.stackMap += r.stackMap;
        all.pcTable += r.pcTable;
        all.inlineFrames += r.inlineFrames;
    }
    std::fprintf(stderr, "    %-10s %-8s %7zu %8.2f %8.2f %8.2f %8.2f %8.2f\n", "all", "", all.functions,
                 mb(all.code), mb(all.stackMap), mb(all.pcTable), mb(all.inlineFrames), mb(all.total()));

    // What is left: the module-wide tables, grouped by name.
    std::map<std::string, size_t> groups;
    for (const auto& [name, size] : symSize) groups[groupName(name)] += size;
    std::vector<std::pair<std::string, size_t>> sorted(groups.begin(), groups.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "  module tables (MB):\n");
    for (size_t i = 0; i < sorted.size() && i < 16; ++i) {
        std::fprintf(stderr, "    %8.2f %s\n", mb(sorted[i].second), sorted[i].first.c_str());
    }

    std::sort(fns.begin(), fns.end(), [](const Fn& a, const Fn& b) { return a.row.total() > b.row.total(); });
    std::fprintf(stderr, "  largest functions (KB: code, stack map, pc table; harvested entries):\n");
    for (size_t i = 0; i < fns.size() && i < 24; ++i) {
        const Fn& f = fns[i];
        std::fprintf(stderr, "    %7.0f %7.0f %7.0f  %-8s %-8s %8llu  %s\n", f.row.code / 1e3, f.row.stackMap / 1e3,
                     f.row.pcTable / 1e3, f.kind.c_str(), heatName(f.heat),
                     static_cast<unsigned long long>(f.entries), f.name.c_str());
    }
}

}  // namespace bronze::codegen
