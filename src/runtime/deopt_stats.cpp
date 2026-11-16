// THE DEOPT REPORT: what every module's resume twins counted, printed at exit
// under BRONZE_DEOPT_STATS=1 (docs/deopt-stats.md). A read-only diagnostic:
// the counters are bumped by generated code whether or not anyone reads them
// (codegen-brass/deopt_sites.h), and this file only reads them back.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"

namespace {

struct Table {
    const uint64_t* counts = nullptr;
    const char* text = nullptr;
    uint32_t generics = 0;
    uint32_t sites = 0;
    uint64_t retireAfter = 0;
};

struct State {
    std::mutex mu;
    std::vector<Table> tables;
    bool armed = false;
};

State& state() {
    static State* s = new State();  // leaked on purpose: read by the atexit report
    return *s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) return out;
        at = next + 1;
    }
}

void report() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    struct Row {
        uint64_t count;
        std::string function, id, what, where;
    };
    std::vector<Row> rows;
    std::vector<std::string> retired;
    uint64_t total = 0;
    for (const Table& t : s.tables) {
        std::vector<std::string> lines = split(t.text, '\n');
        // A function's rank is its place among the distinct names, in order.
        std::string last;
        uint32_t rank = 0;
        bool first = true;
        for (uint32_t i = 0; i < t.sites && i < lines.size(); ++i) {
            std::vector<std::string> f = split(lines[i], '\t');
            if (f.size() < 4) continue;
            if (!first && f[0] != last) ++rank;
            if (first || f[0] != last) {
                if (rank < t.generics && t.counts[rank] >= t.retireAfter) {
                    retired.push_back(f[0] + " (" + std::to_string(t.counts[rank]) + " exits)");
                }
            }
            first = false;
            last = f[0];
            const uint64_t n = t.counts[t.generics + i];
            if (n == 0) continue;
            total += n;
            rows.push_back({n, f[0], f[1], f[2], f[3]});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
    std::fprintf(stderr, "deopt: %llu exits at %zu resume points\n", static_cast<unsigned long long>(total),
                 rows.size());
    std::fprintf(stderr, "%12s  %-4s  %-48s  %-24s  %s\n", "exits", "id", "guards -> resumes at", "where", "function");
    for (const Row& r : rows) {
        std::fprintf(stderr, "%12llu  %-4s  %-48s  %-24s  %s\n", static_cast<unsigned long long>(r.count),
                     r.id.c_str(), r.what.c_str(), r.where.c_str(), r.function.c_str());
    }
    for (const std::string& r : retired) std::fprintf(stderr, "deopt: retired %s\n", r.c_str());
}

}  // namespace

extern "C" void bronze_register_deopt_counts(uint64_t* counts, const char* text, uint32_t generics, uint32_t sites,
                                             uint64_t retireAfter) {
    static const bool wanted = [] {
        const char* v = std::getenv("BRONZE_DEOPT_STATS");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    if (!wanted || counts == nullptr || text == nullptr) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    // A module instance registered again (its init ran twice) replaces itself.
    std::erase_if(s.tables, [counts](const Table& t) { return t.counts == counts; });
    s.tables.push_back({counts, text, generics, sites, retireAfter});
    if (!s.armed) {
        s.armed = true;
        std::atexit(report);
    }
}
