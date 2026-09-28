
#include "runtime/sampler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <brass/debug/jit_code_registry.hpp>

#include "runtime/stack_trace.h"
#include "runtime/symbolize.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

namespace bronze::runtime {

#ifndef _WIN32

// The sampler is Win32-specific (SuspendThread + GetThreadContext + the x64
// unwind walk). Elsewhere the note is a no-op; the campaign machine is
// Windows and a POSIX signal-based port is a later chunk if ever needed.
void samplerNoteJsThread() noexcept {}

#else

namespace {

constexpr uint32_t kMaxFrames = 64;

struct ModuleRange {
    uint64_t base;
    uint64_t end;
};

struct SamplerState {
    HANDLE target = nullptr;          // duplicated handle to the JS thread
    DWORD targetId = 0;
    HANDLE thread = nullptr;          // the sampler thread itself
    volatile LONG stop = 0;
    uint32_t hz = 1000;
    uint64_t qpcFreq = 0;
    uint64_t qpcStart = 0;
    // The sample log: [header, pc0..pcN-1]*, header = (relMs << 8) | count.
    // Appended by the sampler thread only, read after join; no lock needed.
    std::vector<uint64_t> log;
    bool truncated = false;
    // Loaded-module ranges, refreshed off the hot path. The walk consults
    // this before RtlLookupFunctionEntry so a garbage PC can never send the
    // lookup into ntdll's dynamic-function-table path (which takes a lock the
    // suspended thread could in principle hold).
    std::vector<ModuleRange> modules;
    uint64_t lastModuleRefreshMs = 0;
    // BRONZE_SAMPLE=all: the process's other threads, re-listed each second.
    bool allThreads = false;
    DWORD samplerId = 0;
    std::vector<HANDLE> others;
    uint64_t lastThreadRefreshMs = 0;
};

SamplerState* g_state = nullptr;

uint64_t qpcNow() {
    LARGE_INTEGER li;
    ::QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

void refreshModuleRanges(SamplerState& st) {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 512);
    st.modules.clear();
    st.modules.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        MODULEINFO mi;
        if (::GetModuleInformation(::GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            const uint64_t base = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
            st.modules.push_back({base, base + mi.SizeOfImage});
        }
    }
    std::sort(st.modules.begin(), st.modules.end(),
              [](const ModuleRange& a, const ModuleRange& b) { return a.base < b.base; });
}

void refreshOtherThreads(SamplerState& st) {
    for (HANDLE h : st.others) ::CloseHandle(h);
    st.others.clear();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == st.targetId || te.th32ThreadID == st.samplerId) continue;
        HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                te.th32ThreadID);
        if (h) st.others.push_back(h);
    }
    ::CloseHandle(snap);
}

bool pcInKnownModule(const SamplerState& st, uint64_t pc) {
    auto it = std::upper_bound(st.modules.begin(), st.modules.end(), pc,
                               [](uint64_t v, const ModuleRange& m) { return v < m.base; });
    if (it == st.modules.begin()) return false;
    --it;
    return pc >= it->base && pc < it->end;
}

// Walks the suspended target's stack into `pcs`, returning the frame count.
// Reads only the target's own stack memory and the modules' static unwind
// tables; allocates nothing. A misstep in the unwind (a frame the data does
// not describe, a torn prologue) ends the walk rather than faulting: every
// dereference is bounds-checked against the thread's stack limits.
uint32_t walkStack(const SamplerState& st, const CONTEXT& inCtx, uint64_t* pcs) {
    CONTEXT ctx = inCtx;
    uint32_t n = 0;
    // The stack bounds, so the leaf-frame return-address read below can never
    // touch memory outside the target's stack.
    uint64_t stackLo = 0, stackHi = 0;
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (::VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.Rsp)), &mbi,
                           sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT) {
            stackLo = reinterpret_cast<uint64_t>(mbi.BaseAddress);
            stackHi = stackLo + mbi.RegionSize;
        }
    }
    while (n < kMaxFrames && ctx.Rip != 0) {
        pcs[n++] = ctx.Rip;
        if (!pcInKnownModule(st, ctx.Rip)) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION rf = ::RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (rf == nullptr) {
            // A true leaf function: the return address is at RSP.
            if (ctx.Rsp < stackLo || ctx.Rsp + 8 > stackHi) break;
            ctx.Rip = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(ctx.Rsp));
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx, &handlerData,
                           &establisher, nullptr);
        if (ctx.Rsp != 0 && stackLo != 0 && (ctx.Rsp < stackLo || ctx.Rsp > stackHi)) break;
    }
    return n;
}

DWORD WINAPI samplerLoop(LPVOID param) {
    auto& st = *static_cast<SamplerState*>(param);
    st.samplerId = ::GetCurrentThreadId();
    ::timeBeginPeriod(1);
    refreshModuleRanges(st);
    const double periodMs = 1000.0 / st.hz;
    double nextDue = 0.0;
    uint64_t pcs[kMaxFrames];
    // ~32M words = 256MB ceiling; at 1 kHz with typical 12-frame stacks that
    // is over 40 minutes of run, far past any bench.
    const size_t kMaxWords = (256ull << 20) / sizeof(uint64_t);
    st.log.reserve(1 << 22);
    while (::InterlockedCompareExchange(&st.stop, 0, 0) == 0) {
        const uint64_t nowQpc = qpcNow();
        const double relMs =
            1000.0 * static_cast<double>(nowQpc - st.qpcStart) / static_cast<double>(st.qpcFreq);
        if (relMs < nextDue) {
            ::Sleep(1);
            continue;
        }
        nextDue = relMs + periodMs;
        if (relMs - st.lastModuleRefreshMs > 1000.0) {
            st.lastModuleRefreshMs = static_cast<uint64_t>(relMs);
            refreshModuleRanges(st);
        }
        if (st.allThreads && relMs - st.lastThreadRefreshMs > 1000.0) {
            st.lastThreadRefreshMs = static_cast<uint64_t>(relMs);
            refreshOtherThreads(st);
        }
        if (st.log.size() + (kMaxFrames + 1) * (1 + st.others.size()) > kMaxWords) {
            st.truncated = true;
            break;
        }
        auto sampleOne = [&](HANDLE thread) {
            if (::SuspendThread(thread) == static_cast<DWORD>(-1)) return;
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            uint32_t n = 0;
            if (::GetThreadContext(thread, &ctx)) {
                n = walkStack(st, ctx, pcs);
            }
            ::ResumeThread(thread);
            if (n > 0) {
                // Append AFTER the resume: vector growth may allocate, and an
                // allocation while the target holds the CRT heap lock
                // suspended would deadlock the process.
                st.log.push_back((static_cast<uint64_t>(relMs) << 8) | n);
                st.log.insert(st.log.end(), pcs, pcs + n);
            }
        };
        sampleOne(st.target);
        for (HANDLE h : st.others) sampleOne(h);
    }
    for (HANDLE h : st.others) ::CloseHandle(h);
    st.others.clear();
    ::timeEndPeriod(1);
    return 0;
}

// --- exit-time aggregation + report ---------------------------------------

struct FuncRow {
    std::string name;
    std::string module;
    uint64_t self = 0;
    uint64_t total = 0;
    uint64_t selfTail = 0;
    uint64_t totalTail = 0;
};

// One source line of compiled JS, credited with every sample whose innermost
// compiled frame is on it — the leaf when the leaf is compiled code, else the
// compiled caller of the runtime helper the leaf is in. That is the line's
// cost as a program sees it: a `+` that became bronze_add is the line's, not
// the helper's. Resolved through the pc tables the backend emitted, the ones
// Error.stack reads, so the line named here is the one a stack would name.
struct LineRow {
    std::string function;
    std::string file;
    uint32_t line = 0;
    uint64_t self = 0;
    uint64_t selfTail = 0;
};

// The innermost compiled frame of one sample, or false when none of its
// frames is compiled JS. Frame 0 holds the instruction the thread was
// stopped on; every later frame holds a return address, one past its call.
bool innermostCompiledSite(const uint64_t* pcs, uint32_t n, CodeSite& out) {
    for (uint32_t f = 0; f < n; ++f) {
        const uint64_t pc = f == 0 ? pcs[f] : pcs[f] - 1;
        if (find_code_site(reinterpret_cast<const void*>(static_cast<uintptr_t>(pc)), out)) {
            return true;
        }
    }
    return false;
}

void dumpSamplerReport() {
    SamplerState* st = g_state;
    if (st == nullptr) return;
    ::InterlockedExchange(&st->stop, 1);
    if (st->thread) {
        ::WaitForSingleObject(st->thread, 5000);
        ::CloseHandle(st->thread);
        st->thread = nullptr;
    }

    // Window: everything, and optionally the last TAIL_MS of the run.
    uint64_t tailMs = 0;
    if (const char* t = std::getenv("BRONZE_SAMPLE_TAIL_MS")) {
        tailMs = static_cast<uint64_t>(std::strtoull(t, nullptr, 10));
    }
    uint64_t lastRel = 0;
    for (size_t i = 0; i < st->log.size();) {
        lastRel = st->log[i] >> 8;
        i += 1 + (st->log[i] & 0xFF);
    }
    const uint64_t tailFrom = (tailMs > 0 && lastRel > tailMs) ? lastRel - tailMs : 0;
    // BRONZE_SAMPLE_HEAD_MS=<ms>: only the samples of the run's first <ms>,
    // everywhere below (a run's startup apart from what follows it).
    uint64_t headMs = 0;
    if (const char* h = std::getenv("BRONZE_SAMPLE_HEAD_MS")) {
        headMs = static_cast<uint64_t>(std::strtoull(h, nullptr, 10));
    }
    auto pastHead = [headMs](uint64_t relMs) { return headMs > 0 && relMs > headMs; };

    // PC -> function key, resolved once per unique PC; then per-sample
    // aggregation: leaf = self, each distinct function on the stack = total.
    std::unordered_map<uint64_t, uint64_t> pcToFunc;   // pc -> funcStart key
    std::unordered_map<uint64_t, FuncRow> rows;        // funcStart -> row
    uint64_t sampleCount = 0, sampleCountTail = 0;

    auto funcKeyFor = [&](uint64_t pc) -> uint64_t {
        auto it = pcToFunc.find(pc);
        if (it != pcToFunc.end()) return it->second;
        SymbolizedPc sp;
        symbolizePc(pc, sp);
        uint64_t key = sp.funcStart;
        // Code a brass JIT tier installed is in no module: its registry
        // names the function and the tier ("layout [tier 2]"), so a tiered
        // run's profile says which tier each hot function ran in.
        brass::debug::JitCodeInfo jit;
        const bool isJit = !sp.resolved && brass::debug::find_jit_code(static_cast<uintptr_t>(pc), &jit);
        if (isJit) key = static_cast<uint64_t>(jit.start);
        pcToFunc.emplace(pc, key);
        auto& row = rows[key];
        if (row.name.empty()) {
            if (isJit) {
                row.name = jit.name + " [" + brass::debug::jit_tier_label(jit.tier) + "]";
                row.module = "jit";
            } else {
                // Unresolved: the unwind-table name (module+offset) when
                // there is one, so distinct functions stay distinct rows.
                row.name = (sp.resolved || sp.name[0]) ? sp.name : "(unresolved)";
                row.module = sp.module;
            }
        }
        return key;
    };

    // (descriptor, file, line) -> row: the descriptor's address, one per
    // compiled function, and the file and line the pc table gave (a merged
    // top level spans several files).
    std::map<std::tuple<const void*, const void*, uint32_t>, LineRow> lineRows;

    // BRONZE_SAMPLE_WITHIN=<text>: only the samples whose stack holds a
    // function whose name contains <text> — one phase's own profile (say
    // `inferModule`) out of a whole run.
    const char* within = std::getenv("BRONZE_SAMPLE_WITHIN");
    if (within && !within[0]) within = nullptr;
    std::unordered_set<uint64_t> seen;
    for (size_t i = 0; i < st->log.size();) {
        const uint64_t relMs = st->log[i] >> 8;
        const uint32_t n = static_cast<uint32_t>(st->log[i] & 0xFF);
        const uint64_t* pcs = &st->log[i + 1];
        i += 1 + n;
        if (within) {
            bool hit = false;
            for (uint32_t f = 0; f < n && !hit; ++f) {
                hit = rows[funcKeyFor(pcs[f])].name.find(within) != std::string::npos;
            }
            if (!hit) continue;
        }
        if (pastHead(relMs)) continue;
        const bool inTail = relMs >= tailFrom;
        ++sampleCount;
        if (inTail) ++sampleCountTail;
        seen.clear();
        for (uint32_t f = 0; f < n; ++f) {
            const uint64_t key = funcKeyFor(pcs[f]);
            auto& row = rows[key];
            if (f == 0) {
                row.self++;
                if (inTail) row.selfTail++;
            }
            if (seen.insert(key).second) {
                row.total++;
                if (inTail) row.totalTail++;
            }
        }
        if (CodeSite site; innermostCompiledSite(pcs, n, site)) {
            const bronze_fn_desc* desc = site.range->desc;
            auto& lr = lineRows[{desc, site.file, site.line}];
            if (lr.line == 0) {
                lr.function = (desc->name && desc->name[0]) ? desc->name : "<anonymous>";
                lr.file = (site.file && site.file[0]) ? site.file : "<anonymous>";
                lr.line = site.line ? site.line : 1;
            }
            lr.self++;
            if (inTail) lr.selfTail++;
        }
    }
    if (sampleCount == 0) return;

    // BRONZE_SAMPLE_CALLERS=<text>[|<text>...]: for each sample whose stack
    // holds a function whose name contains <text>, the chain of its callers
    // (six frames up from the innermost match), counted and printed, one
    // table per text. Self time says what is hot; this says who asked for it.
    std::vector<std::string> foci;
    if (const char* list = std::getenv("BRONZE_SAMPLE_CALLERS"); list && list[0]) {
        std::string_view rest(list);
        while (!rest.empty()) {
            const size_t bar = rest.find('|');
            const std::string_view one = rest.substr(0, bar);
            if (!one.empty()) foci.emplace_back(one);
            if (bar == std::string_view::npos) break;
            rest.remove_prefix(bar + 1);
        }
    }
    for (const std::string& focusText : foci) {
        const char* focus = focusText.c_str();
        std::unordered_map<std::string, uint64_t> chains;
        uint64_t hits = 0;
        for (size_t i = 0; i < st->log.size();) {
            const uint64_t relMs = st->log[i] >> 8;
            const uint32_t n = static_cast<uint32_t>(st->log[i] & 0xFF);
            const uint64_t* pcs = &st->log[i + 1];
            i += 1 + n;
            if (pastHead(relMs)) continue;
            for (uint32_t f = 0; f < n; ++f) {
                if (rows[funcKeyFor(pcs[f])].name.find(focus) == std::string::npos) continue;
                std::string chain;
                for (uint32_t g = f; g < n && g < f + 7; ++g) {
                    if (g != f) chain += "  <-  ";
                    chain += rows[funcKeyFor(pcs[g])].name.substr(0, 60);
                }
                ++chains[chain];
                ++hits;
                break;
            }
        }
        std::vector<std::pair<uint64_t, const std::string*>> ranked;
        for (const auto& [c, k] : chains) ranked.push_back({k, &c});
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::fprintf(stderr, "\n=== callers of '%s': %llu samples ===\n", focus,
                     static_cast<unsigned long long>(hits));
        for (size_t k = 0; k < ranked.size() && k < 25; ++k) {
            std::fprintf(stderr, "%7llu  %s\n", static_cast<unsigned long long>(ranked[k].first),
                         ranked[k].second->c_str());
        }
    }

    std::vector<const LineRow*> sortedLines;
    sortedLines.reserve(lineRows.size());
    for (const auto& [k, r] : lineRows) sortedLines.push_back(&r);
    std::sort(sortedLines.begin(), sortedLines.end(), [](const LineRow* a, const LineRow* b) {
        if (a->self != b->self) return a->self > b->self;
        if (a->file != b->file) return a->file < b->file;
        return a->line < b->line;
    });

    std::vector<FuncRow*> sorted;
    sorted.reserve(rows.size());
    for (auto& [k, r] : rows) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [](const FuncRow* a, const FuncRow* b) {
        if (a->self != b->self) return a->self > b->self;
        return a->name < b->name;
    });

    const double periodMs = 1000.0 / st->hz;

    // Text table to stderr.
    std::fprintf(stderr, "\n=== Bronze Sampling Profile (BRONZE_SAMPLE=1) ===\n");
    std::fprintf(stderr,
                 "samples: %llu (%.0f Hz nominal, %.3f ms/sample), span %llu ms%s%s\n",
                 static_cast<unsigned long long>(sampleCount), static_cast<double>(st->hz),
                 periodMs, static_cast<unsigned long long>(lastRel),
                 st->truncated ? ", TRUNCATED at buffer cap" : "",
                 tailMs ? ", tail window emitted" : "");
    std::fprintf(stderr, "%-56s %-26s %9s %9s %7s\n", "Function", "Module", "Self", "Total",
                 "Self%");
    // BRONZE_SAMPLE_TOP=<n>: rows per table (40 by default).
    size_t topRows = 40;
    if (const char* t = std::getenv("BRONZE_SAMPLE_TOP"); t && t[0]) {
        topRows = static_cast<size_t>(std::strtoul(t, nullptr, 10));
        if (topRows == 0) topRows = 40;
    }
    for (size_t i = 0; i < sorted.size() && i < topRows; ++i) {
        const FuncRow& r = *sorted[i];
        std::fprintf(stderr, "%-56.56s %-26.26s %9llu %9llu %6.2f%%\n", r.name.c_str(),
                     r.module.c_str(), static_cast<unsigned long long>(r.self),
                     static_cast<unsigned long long>(r.total),
                     100.0 * static_cast<double>(r.self) / static_cast<double>(sampleCount));
    }
    if (!sortedLines.empty()) {
        std::fprintf(stderr, "\n%-56s %-36s %9s %7s\n", "Line", "Function", "Self", "Self%");
        for (size_t i = 0; i < sortedLines.size() && i < topRows; ++i) {
            const LineRow& r = *sortedLines[i];
            // The file's basename: the directory is the same for every row.
            const char* base = r.file.c_str();
            if (const char* s = std::strrchr(base, '/')) base = s + 1;
            if (const char* s = std::strrchr(base, '\\')) base = s + 1;
            const std::string where = std::string(base) + ":" + std::to_string(r.line);
            std::fprintf(stderr, "%-56.56s %-36.36s %9llu %6.2f%%\n", where.c_str(),
                         r.function.c_str(), static_cast<unsigned long long>(r.self),
                         100.0 * static_cast<double>(r.self) / static_cast<double>(sampleCount));
        }
    }
    std::fflush(stderr);

    // JSON.
    const char* outPath = std::getenv("BRONZE_SAMPLE_OUT");
    if (outPath == nullptr || outPath[0] == 0) outPath = "bronze_sample.json";
    FILE* f = std::fopen(outPath, "wb");
    if (f == nullptr) return;
    auto jsonEscape = [](const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '"' || c == '\\') {
                o += '\\';
                o += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += c;
            }
        }
        return o;
    };
    std::fprintf(f,
                 "{\n\"version\":\"bronze-sample-v0\",\n\"hz\":%u,\n\"period_ms\":%.6f,\n"
                 "\"samples\":%llu,\n\"samples_tail\":%llu,\n\"span_ms\":%llu,\n"
                 "\"tail_ms\":%llu,\n\"truncated\":%s,\n\"functions\":[\n",
                 st->hz, periodMs, static_cast<unsigned long long>(sampleCount),
                 static_cast<unsigned long long>(sampleCountTail),
                 static_cast<unsigned long long>(lastRel),
                 static_cast<unsigned long long>(tailMs), st->truncated ? "true" : "false");
    bool first = true;
    for (const FuncRow* r : sorted) {
        std::fprintf(f,
                     "%s{\"name\":\"%s\",\"module\":\"%s\",\"self\":%llu,\"total\":%llu,"
                     "\"self_tail\":%llu,\"total_tail\":%llu}",
                     first ? "" : ",\n", jsonEscape(r->name).c_str(),
                     jsonEscape(r->module).c_str(), static_cast<unsigned long long>(r->self),
                     static_cast<unsigned long long>(r->total),
                     static_cast<unsigned long long>(r->selfTail),
                     static_cast<unsigned long long>(r->totalTail));
        first = false;
    }
    std::fprintf(f, "\n],\n\"lines\":[\n");
    first = true;
    for (const LineRow* r : sortedLines) {
        std::fprintf(f,
                     "%s{\"function\":\"%s\",\"file\":\"%s\",\"line\":%u,\"self\":%llu,"
                     "\"self_tail\":%llu}",
                     first ? "" : ",\n", jsonEscape(r->function).c_str(),
                     jsonEscape(r->file).c_str(), r->line,
                     static_cast<unsigned long long>(r->self),
                     static_cast<unsigned long long>(r->selfTail));
        first = false;
    }
    std::fprintf(f, "\n]}\n");
    std::fclose(f);
    std::fprintf(stderr, "sampler: wrote %s\n", outPath);
}

}  // namespace

void samplerNoteJsThread() noexcept {
    static bool noted = false;
    if (noted) return;
    noted = true;
    const char* env = std::getenv("BRONZE_SAMPLE");
    if (env == nullptr || (std::strcmp(env, "1") != 0 && std::strcmp(env, "all") != 0)) return;

    auto* st = new SamplerState();
    st->allThreads = std::strcmp(env, "all") == 0;
    if (const char* hz = std::getenv("BRONZE_SAMPLE_HZ")) {
        const long v = std::strtol(hz, nullptr, 10);
        if (v >= 50 && v <= 4000) st->hz = static_cast<uint32_t>(v);
    }
    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    st->qpcFreq = static_cast<uint64_t>(freq.QuadPart);
    st->qpcStart = qpcNow();
    st->targetId = ::GetCurrentThreadId();
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                           &st->target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_QUERY_INFORMATION,
                           FALSE, 0)) {
        delete st;
        return;
    }
    g_state = st;
    st->thread = ::CreateThread(nullptr, 0, samplerLoop, st, 0, nullptr);
    if (st->thread == nullptr) {
        g_state = nullptr;
        ::CloseHandle(st->target);
        delete st;
        return;
    }
    std::atexit(dumpSamplerReport);
    std::fprintf(stderr, "sampler: BRONZE_SAMPLE armed at %u Hz on thread %lu\n", st->hz,
                 static_cast<unsigned long>(st->targetId));
}

#endif  // _WIN32

}  // namespace bronze::runtime
