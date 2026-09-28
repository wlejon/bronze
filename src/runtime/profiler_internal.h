#pragma once

// What profiler.cpp (the sampler thread) hands profiler_report.cpp (the
// aggregation at stop): the raw log and the symbols its frames were resolved
// to while their code was still installed.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/profiler.h"

namespace bronze::runtime::profiler_detail {

enum class SymKind : uint8_t { Native, Jit, Aot, Interp };

struct Sym {
    SymKind kind = SymKind::Native;
    std::string name;
    std::string tier;
    std::string module;
    std::string file;
    uint32_t line = 0;
    // Native: the pc to symbolize at stop (functions merge by their start).
    uint64_t pc = 0;
};

// One sample in the log:
//   header  (relMs << 32) | (threadIndex << 16) | (nativeCount << 8) | interpCount
//   nativeCount x (sp, sym)      innermost first
//   interpCount x (address, sym) innermost first
inline uint64_t packHeader(uint64_t relMs, uint32_t thread, uint32_t nNative, uint32_t nInterp) {
    return (relMs << 32) | (uint64_t(thread & 0xFFFF) << 16) | (uint64_t(nNative & 0xFF) << 8) | (nInterp & 0xFF);
}
inline uint32_t headerThread(uint64_t h) { return uint32_t(h >> 16) & 0xFFFF; }
inline uint32_t headerNative(uint64_t h) { return uint32_t(h >> 8) & 0xFF; }
inline uint32_t headerInterp(uint64_t h) { return uint32_t(h) & 0xFF; }

struct Collected {
    uint32_t hz = 0;
    double durationMs = 0;
    bool truncated = false;
    std::vector<uint64_t> log;
    std::vector<Sym> syms;
    std::vector<ProfilerThread> threads;
};

// Aggregates `c` into `out` (profiler_report.cpp).
void aggregate(Collected& c, const ProfilerStopOptions& options, ProfilerResult& out);

}  // namespace bronze::runtime::profiler_detail
