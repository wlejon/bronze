// How fast real programs reach their optimized code under `bronze run`, held
// to pinned budgets.
//
// Each workload runs in the tiered pipeline (the default tier, as bro runs
// an app) with brass's tier timeline on (BRASS_TIER_LOG, brass
// runtime/tier_timeline.hpp), which reports, from the pipeline's start:
//
//   hot_tier2_ms    when the last of the program's hottest functions had its
//                   optimized (tier-2 or OSR) code
//   hot_latency_ms  the longest any of them waited from asking for that code
//                   to having it
//   hot_untiered    hot functions that never got it
//   compile_ms      compile work, every thread
//   stall_ms        tiering work done on the program's own thread
//   max_stall_ms    the longest single piece of it
//   deopts          guard failures in optimized code
//
// Each workload runs kRuns times and each metric is the median. The bounds
// sit well above today's numbers (the table below), so crossing one is a
// regression in kind (a compile moved back onto the program's thread, a
// loop that stopped reaching OSR, a speculation that now deopts every run),
// not a slow machine.
//
//   bronze_tier_ratchet           run and check (the `tier-ratchet` test)
//   bronze_tier_ratchet --report  run and print, never fail

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../oracle/run_process.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kRuns = 3;

struct Workload {
    const char* name;
    std::filesystem::path entry;
    std::string hostGlobals;
};

// Baseline (2026-09-26, Windows, Release; median of 3):
//
//   workload        hot_tier2  latency  untiered  compile  stall  max_stall  deopts  first_batch
//   cpu_work          247        51        0        164     6.7     4.4       0        -
//   alloc_batches      15        11        0         11     0.1     0.1       0        3
//   threejs           221         9        0         63    23.7    10.7       0        -
//   pixi              955        35        0        170    11.9     1.2       3        -
//   osr_leave          10         6        0         50     0.0     0.0      23        -
//
// Before the OSR entry's plan and copy moved to the compiling worker,
// cpu_work stalled 30 ms (one 10.8 ms piece); before a never-run property
// site stopped being armed, alloc_batches deopted twice and its first batch
// took 9-10 ms. Before a frame left invalidated OSR code at its next
// backedge (brass OsrEntryPlan::leave_check), osr_leave deopted
// 19,957 times, nearly all in one frame's loop.
struct Bound {
    const char* workload;
    const char* metric;
    double limit;
};
const Bound kBounds[] = {
    {"cpu_work", "hot_tier2_ms", 600},     {"cpu_work", "hot_latency_ms", 150},
    {"cpu_work", "hot_untiered", 0},       {"cpu_work", "stall_ms", 20},
    {"cpu_work", "max_stall_ms", 9},       {"cpu_work", "deopts", 2},
    {"alloc_batches", "hot_tier2_ms", 80}, {"alloc_batches", "hot_latency_ms", 50},
    {"alloc_batches", "hot_untiered", 0},  {"alloc_batches", "stall_ms", 5},
    {"alloc_batches", "deopts", 0},        {"alloc_batches", "first_batch_ms", 8},
    {"threejs", "hot_tier2_ms", 700},      {"threejs", "hot_latency_ms", 60},
    {"threejs", "hot_untiered", 1},        {"threejs", "stall_ms", 70},
    {"threejs", "max_stall_ms", 30},       {"pixi", "hot_tier2_ms", 2500},
    {"pixi", "hot_latency_ms", 150},       {"pixi", "hot_untiered", 0},
    {"pixi", "stall_ms", 40},              {"pixi", "max_stall_ms", 10},
    {"pixi", "deopts", 10},                {"osr_leave", "deopts", 60},
};

using Metrics = std::map<std::string, double>;

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// The summary lines of a report file: one per program the run released, all
// over the same process-wide timeline; each metric is the largest of them.
Metrics parseReport(const std::filesystem::path& file) {
    Metrics m;
    std::ifstream in(file);
    std::string line;
    const std::string prefix = "tier-timeline wall_ms=";
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::istringstream words(line.substr(std::strlen("tier-timeline ")));
        std::string word;
        while (words >> word) {
            const size_t eq = word.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = word.substr(0, eq);
            const double value = std::strtod(word.c_str() + eq + 1, nullptr);
            auto [it, fresh] = m.emplace(key, value);
            if (!fresh) it->second = std::max(it->second, value);
        }
    }
    return m;
}

// "first=<n>ms" in the program's output: alloc_batches' first batch.
void parseOutput(const std::string& out, Metrics& m) {
    const size_t at = out.find("first=");
    if (at != std::string::npos) m["first_batch_ms"] = std::strtod(out.c_str() + at + 6, nullptr);
}

bool runOnce(const Workload& w, int run, Metrics& out) {
    const std::filesystem::path report = std::filesystem::temp_directory_path() /
                                         ("bronze_tier_ratchet_" + std::string(w.name) + "_" + std::to_string(run) +
                                          ".txt");
    std::error_code ec;
    std::filesystem::remove(report, ec);
    std::string cmd = oracle::quoted(TEST_BRONZE_CLI) + " run " + oracle::quoted(w.entry.string());
    if (!w.hostGlobals.empty()) cmd += " --host-globals " + oracle::quoted(w.hostGlobals);
    setEnv("BRASS_TIER_LOG", report.string());
    const oracle::RunResult r = oracle::runCommand(cmd, false, oracle::kRunTimeoutMs * 4);
    setEnv("BRASS_TIER_LOG", "");
    if (!r.ran || r.exitCode != 0) {
        std::printf("  %s: run %d failed (exit %d%s)\n%s\n", w.name, run, r.exitCode, r.timedOut ? ", timed out" : "",
                    r.errors.c_str());
        return false;
    }
    out = parseReport(report);
    std::filesystem::remove(report, ec);
    if (out.empty()) {
        std::printf("  %s: run %d wrote no tier report\n", w.name, run);
        return false;
    }
    parseOutput(r.output, out);
    return true;
}

Metrics median(const std::vector<Metrics>& runs) {
    Metrics m;
    for (const auto& [key, _] : runs.front()) {
        std::vector<double> v;
        for (const Metrics& r : runs) {
            auto it = r.find(key);
            if (it != r.end()) v.push_back(it->second);
        }
        std::sort(v.begin(), v.end());
        m[key] = v[v.size() / 2];
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    const bool report_only = argc == 2 && std::strcmp(argv[1], "--report") == 0;
    const std::filesystem::path here = TEST_TIER_RATCHET_DIR;
    const std::filesystem::path oracle_dir = TEST_ORACLE_DIR;
    const std::vector<Workload> workloads = {
        {"cpu_work", here / "cpu_work.js", {}},
        {"alloc_batches", here / "alloc_batches.js", {}},
        {"threejs", oracle_dir / "threejs" / "main.js", {}},
        {"pixi", oracle_dir / "pixi" / "main.js", (oracle_dir / "pixi" / "host.globals").string()},
        {"osr_leave", here / "osr_carried_deopt.js", {}},
    };
    const char* shown[] = {"hot_tier2_ms", "hot_latency_ms", "hot_untiered", "compile_ms",
                           "stall_ms",     "max_stall_ms",   "deopts",       "first_batch_ms"};

    int failures = 0;
    std::map<std::string, Metrics> results;
    std::printf("%-14s", "workload");
    for (const char* k : shown) std::printf(" %14s", k);
    std::printf("\n");
    for (const Workload& w : workloads) {
        std::vector<Metrics> runs;
        for (int i = 0; i < kRuns; ++i) {
            Metrics m;
            if (!runOnce(w, i, m)) break;
            runs.push_back(std::move(m));
        }
        if (runs.size() != kRuns) {
            ++failures;
            continue;
        }
        const Metrics m = median(runs);
        std::printf("%-14s", w.name);
        for (const char* k : shown) {
            auto it = m.find(k);
            if (it == m.end()) std::printf(" %14s", "-");
            else std::printf(" %14.2f", it->second);
        }
        std::printf("\n");
        results[w.name] = m;
    }
    std::printf("\n");
    for (const Bound& b : kBounds) {
        auto w = results.find(b.workload);
        if (w == results.end()) continue;
        auto it = w->second.find(b.metric);
        const bool present = it != w->second.end();
        const double value = present ? it->second : -1.0;
        const bool ok = present && value <= b.limit;
        std::printf("  %-14s %-16s %9.2f  (limit %.2f)%s\n", b.workload, b.metric, value, b.limit,
                    ok ? "" : present ? "  FAILED" : "  MISSING");
        if (!ok) ++failures;
    }
    if (report_only) return 0;
    return failures ? 1 : 0;
}
