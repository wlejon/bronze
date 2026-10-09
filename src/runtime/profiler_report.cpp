// The script-armed profiler's aggregation (profiler.h): the log the sampler
// thread wrote, turned into per-function self/total counts, caller edges and
// an optional text table. Runs on the thread that called stop, after the
// sampler has been joined.

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "runtime/profiler_internal.h"
#include "runtime/symbolize.h"

namespace bronze::runtime::profiler_detail {

namespace {

// The interpreter's own dispatch: billed to the JS function it runs, and left
// out of the caller chains, which read JS function to JS function.
bool isInterpreterInternal(const Sym& s) {
    return s.kind == SymKind::Native && s.name.find("FastInterpreter::") != std::string::npos;
}

std::string baseName(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// symbolize.h is single-threaded by design; a stop on two threads at once
// (or a stop racing BRONZE_SAMPLE's exit dump) takes turns.
std::mutex& symbolizeMutex() {
    static auto* mu = new std::mutex();
    return *mu;
}

struct Frame {
    uint64_t address;
    uint32_t row;
    bool internal;
};

}  // namespace

void aggregate(Collected& c, const ProfilerStopOptions& options, ProfilerResult& out) {
    out = ProfilerResult{};
    out.hz = c.hz;
    out.durationMs = c.durationMs;
    out.truncated = c.truncated;
    out.threads = c.threads;

    // Symbols to rows. A native pc is named now, and pcs of one function
    // merge by its start; a JS function merges by name, tier and source.
    std::vector<uint32_t> symRow(c.syms.size(), 0);
    std::vector<bool> symInternal(c.syms.size(), false);
    {
        std::lock_guard<std::mutex> lock(symbolizeMutex());
        std::map<std::tuple<std::string, std::string, std::string, uint32_t>, uint32_t> jsRows;
        std::unordered_map<uint64_t, uint32_t> nativeRows;
        for (size_t i = 0; i < c.syms.size(); ++i) {
            Sym& s = c.syms[i];
            if (s.kind == SymKind::Native || s.kind == SymKind::Aot) {
                SymbolizedPc sp;
                symbolizePc(s.pc, sp);
                s.module = sp.module;
                if (s.kind == SymKind::Native) {
                    s.name = (sp.resolved || sp.name[0]) ? sp.name : "(unresolved)";
                    auto [it, fresh] = nativeRows.try_emplace(sp.funcStart, uint32_t(out.functions.size()));
                    if (fresh) {
                        ProfilerFunction f;
                        f.name = s.name;
                        f.tier = s.tier;
                        f.module = s.module;
                        out.functions.push_back(std::move(f));
                    }
                    symRow[i] = it->second;
                    symInternal[i] = isInterpreterInternal(s);
                    continue;
                }
            }
            auto [it, fresh] = jsRows.try_emplace(std::make_tuple(s.name, s.tier, s.file, s.line),
                                                  uint32_t(out.functions.size()));
            if (fresh) {
                ProfilerFunction f;
                f.name = s.name;
                f.tier = s.tier;
                f.module = s.module;
                f.file = s.file;
                f.line = s.line;
                if (!s.mirName.empty()) {
                    if (MirNameTier1Rejection rejection = mirNameTier1Rejection()) {
                        f.tier1Rejected = rejection(s.mirName);
                    }
                }
                out.functions.push_back(std::move(f));
            }
            symRow[i] = it->second;
        }
    }

    // Per sample: the native and interpreted frames merged innermost first
    // by stack address (an interpreted frame's record lives inside the
    // interpreter's native frame that runs it), the interpreter's own frames
    // dropped, then self to the first, total to each distinct row, and an
    // edge per distinct adjacent pair.
    std::unordered_map<uint64_t, uint64_t> edges;  // (caller << 32 | callee) -> count
    std::vector<Frame> frames;
    std::unordered_set<uint32_t> seenRows;
    std::unordered_set<uint64_t> seenEdges;
    for (size_t i = 0; i < c.log.size();) {
        const uint64_t h = c.log[i++];
        const uint32_t nNative = headerNative(h);
        const uint32_t nInterp = headerInterp(h);
        frames.clear();
        for (uint32_t k = 0; k < nNative; ++k, i += 2) {
            const uint32_t sym = static_cast<uint32_t>(c.log[i + 1]);
            frames.push_back({c.log[i], symRow[sym], symInternal[sym]});
        }
        for (uint32_t k = 0; k < nInterp; ++k, i += 2) {
            const uint32_t sym = static_cast<uint32_t>(c.log[i + 1]);
            frames.push_back({c.log[i], symRow[sym], false});
        }
        if (nInterp > 0) {
            std::stable_sort(frames.begin(), frames.end(),
                             [](const Frame& a, const Frame& b) { return a.address < b.address; });
        }
        frames.erase(std::remove_if(frames.begin(), frames.end(), [](const Frame& f) { return f.internal; }),
                     frames.end());
        if (frames.empty()) continue;
        ++out.samples;
        out.functions[frames[0].row].self++;
        seenRows.clear();
        seenEdges.clear();
        for (size_t f = 0; f < frames.size(); ++f) {
            if (seenRows.insert(frames[f].row).second) out.functions[frames[f].row].total++;
            if (options.callers && f + 1 < frames.size() && frames[f + 1].row != frames[f].row) {
                const uint64_t key = (uint64_t(frames[f + 1].row) << 32) | frames[f].row;
                if (seenEdges.insert(key).second) edges[key]++;
            }
        }
    }

    // Rows by self, then total; rows never charged are dropped (a symbol
    // whose every frame was the interpreter's own).
    std::vector<uint32_t> order;
    for (uint32_t r = 0; r < out.functions.size(); ++r) {
        if (out.functions[r].total > 0) order.push_back(r);
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const ProfilerFunction& x = out.functions[a];
        const ProfilerFunction& y = out.functions[b];
        if (x.self != y.self) return x.self > y.self;
        if (x.total != y.total) return x.total > y.total;
        return x.name < y.name;
    });
    std::vector<uint32_t> remap(out.functions.size(), UINT32_MAX);
    std::vector<ProfilerFunction> sorted;
    sorted.reserve(order.size());
    for (uint32_t r : order) {
        remap[r] = static_cast<uint32_t>(sorted.size());
        sorted.push_back(std::move(out.functions[r]));
    }
    out.functions = std::move(sorted);
    for (const auto& [key, count] : edges) {
        ProfilerEdge e;
        e.caller = remap[uint32_t(key >> 32)];
        e.callee = remap[uint32_t(key)];
        e.count = count;
        out.edges.push_back(e);
    }
    std::sort(out.edges.begin(), out.edges.end(), [](const ProfilerEdge& a, const ProfilerEdge& b) {
        if (a.count != b.count) return a.count > b.count;
        return std::tie(a.caller, a.callee) < std::tie(b.caller, b.callee);
    });

    if (!options.text) return;
    const size_t top = options.top ? options.top : 40;
    std::string t;
    char line[512];
    std::snprintf(line, sizeof line, "samples: %llu (%u Hz), %.0f ms, %zu thread(s)%s\n",
                  static_cast<unsigned long long>(out.samples), out.hz, out.durationMs, out.threads.size(),
                  out.truncated ? ", TRUNCATED at buffer cap" : "");
    t += line;
    std::snprintf(line, sizeof line, "%-56s %-12s %9s %9s %7s\n", "Function", "Tier", "Self", "Total", "Self%");
    t += line;
    const double denom = out.samples ? double(out.samples) : 1.0;
    for (size_t i = 0; i < out.functions.size() && i < top; ++i) {
        const ProfilerFunction& f = out.functions[i];
        std::string label = f.name;
        if (!f.file.empty()) label += " (" + baseName(f.file) + ":" + std::to_string(f.line) + ")";
        else if (f.tier == "native" && !f.module.empty()) label += " [" + f.module + "]";
        std::snprintf(line, sizeof line, "%-56.56s %-12s %9llu %9llu %6.2f%%\n", label.c_str(), f.tier.c_str(),
                      static_cast<unsigned long long>(f.self), static_cast<unsigned long long>(f.total),
                      100.0 * double(f.self) / denom);
        t += line;
    }
    {
        // The rows above whose function the baseline tier rejected: why.
        std::string rejected;
        std::unordered_set<std::string> listed;
        for (size_t i = 0; i < out.functions.size() && i < top; ++i) {
            const ProfilerFunction& f = out.functions[i];
            if (f.tier1Rejected.empty() || !listed.insert(f.name).second) continue;
            std::snprintf(line, sizeof line, "  %.60s: %.400s\n", f.name.c_str(), f.tier1Rejected.c_str());
            rejected += line;
        }
        if (!rejected.empty()) t += "\nrejected by tier 1 (interpreted until hot, then tier 2):\n" + rejected;
    }
    if (options.callers && !out.edges.empty()) {
        t += "\ncallers (caller -> callee):\n";
        for (size_t i = 0; i < out.edges.size() && i < top; ++i) {
            const ProfilerEdge& e = out.edges[i];
            std::snprintf(line, sizeof line, "%9llu  %.60s -> %.60s\n", static_cast<unsigned long long>(e.count),
                          out.functions[e.caller].name.c_str(), out.functions[e.callee].name.c_str());
            t += line;
        }
    }
    out.text = std::move(t);
}

}  // namespace bronze::runtime::profiler_detail
