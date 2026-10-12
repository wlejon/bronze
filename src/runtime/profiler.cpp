#include "runtime/profiler.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "abi/bronze_abi.h"
#include "runtime/profiler_internal.h"

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define BRONZE_PROFILER_SUPPORTED 1
#define BRONZE_PROFILER_WIN32 1
#include <brass/debug/jit_code_registry.hpp>
#include <brass/mir/function.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include "runtime/stack_sampling.h"
#include "runtime/stack_trace.h"

#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#elif defined(__APPLE__) && defined(__aarch64__)
// arm64 macOS: mach thread_suspend + thread_get_state, the frame-pointer
// chain (stack_sampling.cpp), mach_absolute_time.
#define BRONZE_PROFILER_SUPPORTED 1
#define BRONZE_PROFILER_DARWIN 1
#include <brass/debug/jit_code_registry.hpp>
#include <brass/mir/function.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include "runtime/stack_sampling.h"
#include "runtime/stack_trace.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <thread>
#endif

namespace bronze::runtime {

namespace {
std::atomic<MirNameDescriber> g_describer{nullptr};
std::atomic<MirNameTier1Rejection> g_tier1Rejection{nullptr};
}  // namespace

void setMirNameDescriber(MirNameDescriber describer) noexcept {
    g_describer.store(describer, std::memory_order_release);
}

void setMirNameTier1Rejection(MirNameTier1Rejection lookup) noexcept {
    g_tier1Rejection.store(lookup, std::memory_order_release);
}

MirNameTier1Rejection mirNameTier1Rejection() noexcept {
    return g_tier1Rejection.load(std::memory_order_acquire);
}

#if !defined(BRONZE_PROFILER_SUPPORTED)

void registerProfilerThread(uint32_t, std::string_view) {}
void unregisterProfilerThread() {}
bool profilerStart(const ProfilerOptions&, std::string* error) {
    if (error) *error = "the sampling profiler is supported on Windows x64 and arm64 macOS only";
    return false;
}
bool profilerRunning() { return false; }
bool profilerStop(const ProfilerStopOptions&, ProfilerResult&, std::string* error) {
    if (error) *error = "no profile is running";
    return false;
}

#else

using namespace profiler_detail;

namespace {

constexpr uint32_t kMaxNative = 64;
constexpr uint32_t kMaxInterp = 64;
// 256 MB of log; at 1 kHz with typical stacks, far past any window a script
// would profile.
constexpr size_t kMaxWords = (256ull << 20) / sizeof(uint64_t);

#if BRONZE_PROFILER_WIN32
using ThreadId = DWORD;
using ThreadHandle = HANDLE;
constexpr ThreadHandle kNoThread = nullptr;
#else
using ThreadId = uint64_t;           // pthread_threadid_np
using ThreadHandle = mach_port_t;    // a send right of our own, released at unregister
constexpr ThreadHandle kNoThread = MACH_PORT_NULL;
#endif

struct RegisteredThread {
    ThreadId id = 0;
    ThreadHandle handle = kNoThread;
    uint32_t kind = kProfilerThreadOther;
    std::string name;
    brass::FastFrame* const* topSlot = nullptr;
    // arm64 macOS: the thread's stack, read at registration (the walk stays
    // inside it).
    uint64_t stackLo = 0, stackHi = 0;
};

std::mutex& registryMutex() {
    static auto* mu = new std::mutex();
    return *mu;
}
std::vector<RegisteredThread>& registered() {
    static auto* v = new std::vector<RegisteredThread>();
    return *v;
}

const char* kindName(uint32_t kind) {
    switch (kind) {
        case kProfilerThreadMain: return "main";
        case kProfilerThreadWorker: return "worker";
        default: return "other";
    }
}

// One suspended thread's raw capture, resolved after it is resumed.
struct Raw {
    ThreadId threadId = 0;
    uint32_t kind = kProfilerThreadOther;
    std::string nameCopy;  // copied from the registry while its lock is held
    uint32_t nNative = 0;
    uint32_t nInterp = 0;
    uint64_t pcs[kMaxNative];
    uint64_t sps[kMaxNative];
    const void* frameAddrs[kMaxInterp];
    const brass::Function* fns[kMaxInterp];
    // arm64: frame 1 is the link register, which a frameless leaf's caller
    // is only in; dropped at record when it names frame 0's own function
    // (a stale LR from a call the function made earlier).
    bool lrFrame = false;
};

struct Session {
#if BRONZE_PROFILER_WIN32
    HANDLE thread = nullptr;
#else
    std::thread thread;
#endif
    std::atomic<bool> stop{false};
    uint32_t hz = 1000;
    uint32_t mask = kProfilerThreadMain;
    bool processThreads = false;
    uint64_t qpcFreq = 0;
    uint64_t qpcStart = 0;
    ThreadId samplerId = 0;
    sampling::ModuleRanges modules;
    Collected c;
    std::unordered_map<uint64_t, uint32_t> pcSym;
    std::unordered_map<uintptr_t, uint32_t> jitSym;  // by code start + tier
    std::unordered_map<const void*, uint32_t> descSym;
    std::unordered_map<const void*, uint32_t> fnSym;
    std::unordered_map<ThreadId, uint32_t> threadIndex;
    std::vector<std::pair<ThreadId, ThreadHandle>> others;
    std::vector<Raw> raws;
};

std::mutex g_sessionMu;
Session* g_session = nullptr;

#if BRONZE_PROFILER_WIN32
uint64_t qpcNow() {
    LARGE_INTEGER li;
    ::QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}
uint64_t qpcFrequency() {
    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    return static_cast<uint64_t>(freq.QuadPart);
}
ThreadId currentThreadId() { return ::GetCurrentThreadId(); }
void releaseThread(ThreadHandle h) { ::CloseHandle(h); }
#else
uint64_t qpcNow() { return mach_absolute_time(); }
uint64_t qpcFrequency() {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return static_cast<uint64_t>(1e9 * double(tb.denom) / double(tb.numer));
}
ThreadId currentThreadId() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
}
void releaseThread(ThreadHandle h) { mach_port_deallocate(mach_task_self(), h); }
#endif

uint32_t addSym(Session& s, Sym sym) {
    s.c.syms.push_back(std::move(sym));
    return static_cast<uint32_t>(s.c.syms.size() - 1);
}

void fillFromDesc(Sym& sym, const bronze_fn_desc* desc) {
    if (desc->name && desc->name[0]) sym.name = desc->name;
    if (desc->file && desc->file[0]) sym.file = desc->file;
    sym.line = desc->def_line;
}

// A pc of a sampled frame to its symbol. `lookup` is the instruction's own
// address (a return address minus one). Called with the target resumed.
uint32_t symForPc(Session& s, uint64_t lookup) {
    if (auto it = s.pcSym.find(lookup); it != s.pcSym.end()) return it->second;
    uint32_t id;
    brass::debug::JitCodeInfo jit;
    const bronze_code_range* range = find_code_range(reinterpret_cast<const void*>(static_cast<uintptr_t>(lookup)));
    if (brass::debug::find_jit_code(static_cast<uintptr_t>(lookup), &jit)) {
        const uintptr_t key = jit.start ^ (uintptr_t(jit.tier) << 60);
        if (auto j = s.jitSym.find(key); j != s.jitSym.end()) {
            id = j->second;
        } else {
            Sym sym;
            sym.kind = SymKind::Jit;
            sym.tier = brass::debug::jit_tier_label(jit.tier);
            sym.module = "jit";
            sym.name = jit.name.empty() ? "<anonymous>" : jit.name;
            sym.mirName = jit.name;
            const bronze_fn_desc* desc = range ? range->desc : nullptr;
            if (!desc) {
                if (MirNameDescriber d = g_describer.load(std::memory_order_acquire)) desc = d(jit.name);
            }
            if (desc) fillFromDesc(sym, desc);
            id = addSym(s, std::move(sym));
            s.jitSym.emplace(key, id);
        }
    } else if (range && range->desc) {
        if (auto d = s.descSym.find(range->desc); d != s.descSym.end()) {
            id = d->second;
        } else {
            Sym sym;
            sym.kind = SymKind::Aot;
            sym.tier = "aot";
            sym.name = "<anonymous>";
            sym.pc = lookup;  // its module is named at stop
            fillFromDesc(sym, range->desc);
            id = addSym(s, std::move(sym));
            s.descSym.emplace(range->desc, id);
        }
    } else {
        Sym sym;
        sym.kind = SymKind::Native;
        sym.tier = "native";
        sym.pc = lookup;
        id = addSym(s, std::move(sym));
    }
    s.pcSym.emplace(lookup, id);
    return id;
}

uint32_t symForFunction(Session& s, const brass::Function* fn) {
    if (auto it = s.fnSym.find(fn); it != s.fnSym.end()) return it->second;
    Sym sym;
    sym.kind = SymKind::Interp;
    sym.tier = "interpreter";
    sym.module = "interpreter";
    if (fn) {
        const std::string_view mir = fn->name();
        sym.name = mir.empty() ? "<anonymous>" : std::string(mir);
        sym.mirName = std::string(mir);
        if (MirNameDescriber d = g_describer.load(std::memory_order_acquire)) {
            if (const bronze_fn_desc* desc = d(mir)) fillFromDesc(sym, desc);
        }
    } else {
        sym.name = "<bytecode>";
    }
    const uint32_t id = addSym(s, std::move(sym));
    s.fnSym.emplace(fn, id);
    return id;
}

// Suspends `thread`, captures its stack and interpreter frames into `raw`,
// resumes it. Nothing between the suspend and the resume allocates or takes a
// lock another thread could hold while suspended (stack_sampling.h).
#if BRONZE_PROFILER_DARWIN
// A thread's stack [lo, hi), from its own pthread, read before any suspend
// (pthread_from_mach_thread_np takes the thread list's lock, which a
// suspended thread may hold).
void stackBounds(pthread_t pt, uint64_t& lo, uint64_t& hi) {
    lo = hi = 0;
    if (!pt) return;
    hi = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pt));
    lo = hi - pthread_get_stacksize_np(pt);
}

void capture(Session& s, ThreadHandle thread, brass::FastFrame* const* topSlot, Raw& raw, uint64_t stackLo,
             uint64_t stackHi) {
    raw.nNative = 0;
    raw.nInterp = 0;
    raw.lrFrame = false;
    if (thread_suspend(thread) != KERN_SUCCESS) return;
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &count) ==
        KERN_SUCCESS) {
        sampling::Arm64Context ctx;
        ctx.pc = arm_thread_state64_get_pc(st);
        ctx.lr = arm_thread_state64_get_lr(st);
        ctx.fp = arm_thread_state64_get_fp(st);
        ctx.sp = arm_thread_state64_get_sp(st);
        ctx.stackLo = stackLo;
        ctx.stackHi = stackHi;
        raw.nNative = sampling::walkStack(s.modules, &ctx, raw.pcs, raw.sps, kMaxNative);
        raw.lrFrame = ctx.lrFrame;
        if (topSlot) {
            raw.nInterp = static_cast<uint32_t>(
                brass::FastInterpreter::read_suspended_frames(*topSlot, raw.frameAddrs, raw.fns, kMaxInterp));
        }
    }
    thread_resume(thread);
}

// Every thread of the process but the registered ones and the sampler's.
void refreshOthers(Session& s, std::vector<std::pair<uint64_t, uint64_t>>& bounds) {
    for (auto& [id, h] : s.others) releaseThread(h);
    s.others.clear();
    bounds.clear();
    std::unordered_set<ThreadId> skip{s.samplerId};
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const RegisteredThread& t : registered()) skip.insert(t.id);
    }
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        thread_identifier_info_data_t info;
        mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&info), &ic) !=
                KERN_SUCCESS ||
            skip.count(info.thread_id)) {
            releaseThread(list[i]);
            continue;
        }
        uint64_t lo = 0, hi = 0;
        stackBounds(pthread_from_mach_thread_np(list[i]), lo, hi);
        s.others.emplace_back(info.thread_id, list[i]);
        bounds.emplace_back(lo, hi);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
}
#else
void capture(Session& s, HANDLE thread, brass::FastFrame* const* topSlot, Raw& raw) {
    raw.nNative = 0;
    raw.nInterp = 0;
    if (::SuspendThread(thread) == static_cast<DWORD>(-1)) return;
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (::GetThreadContext(thread, &ctx)) {
        raw.nNative = sampling::walkStack(s.modules, &ctx, raw.pcs, raw.sps, kMaxNative);
        if (topSlot) {
            raw.nInterp = static_cast<uint32_t>(
                brass::FastInterpreter::read_suspended_frames(*topSlot, raw.frameAddrs, raw.fns, kMaxInterp));
        }
    }
    ::ResumeThread(thread);
}

void refreshOthers(Session& s) {
    for (auto& [id, h] : s.others) ::CloseHandle(h);
    s.others.clear();
    std::unordered_set<DWORD> skip{s.samplerId};
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const RegisteredThread& t : registered()) skip.insert(t.id);
    }
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    for (BOOL ok = ::Thread32First(snap, &te); ok; ok = ::Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || skip.count(te.th32ThreadID)) continue;
        HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                te.th32ThreadID);
        if (h) s.others.emplace_back(te.th32ThreadID, h);
    }
    ::CloseHandle(snap);
}
#endif

uint32_t threadSlot(Session& s, const Raw& raw) {
    if (auto it = s.threadIndex.find(raw.threadId); it != s.threadIndex.end()) return it->second;
    ProfilerThread t;
    t.id = raw.threadId;
    t.kind = kindName(raw.kind);
    t.name = raw.nameCopy;
    s.c.threads.push_back(std::move(t));
    const uint32_t idx = static_cast<uint32_t>(s.c.threads.size() - 1);
    s.threadIndex.emplace(raw.threadId, idx);
    return idx;
}

// Resolves one capture and appends it to the log.
void record(Session& s, const Raw& raw, uint64_t relMs) {
    if (raw.nNative == 0 && raw.nInterp == 0) return;
    const uint32_t thread = threadSlot(s, raw);
    s.c.threads[thread].samples++;
    auto& log = s.c.log;
    uint32_t nNative = raw.nNative;
    uint32_t syms[kMaxNative];
    for (uint32_t i = 0; i < raw.nNative; ++i) syms[i] = symForPc(s, i == 0 ? raw.pcs[i] : raw.pcs[i] - 1);
    // A link register that names frame 0's own function was left by a call
    // it made earlier, not by its caller: no frame.
    const bool dropLr = raw.lrFrame && raw.nNative > 1 && syms[1] == syms[0];
    if (dropLr) --nNative;
    log.push_back(packHeader(relMs, thread, nNative, raw.nInterp));
    for (uint32_t i = 0; i < raw.nNative; ++i) {
        if (dropLr && i == 1) continue;
        log.push_back(raw.sps[i]);
        log.push_back(syms[i]);
    }
    for (uint32_t i = 0; i < raw.nInterp; ++i) {
        log.push_back(reinterpret_cast<uintptr_t>(raw.frameAddrs[i]));
        log.push_back(symForFunction(s, raw.fns[i]));
    }
}

void samplerBody(Session& s) {
#if BRONZE_PROFILER_WIN32
    ::timeBeginPeriod(1);
#else
    s.samplerId = currentThreadId();
    std::vector<std::pair<uint64_t, uint64_t>> otherBounds;  // each other thread's stack
#endif
    s.modules.refresh();
    const double periodMs = 1000.0 / s.hz;
    double nextDue = 0.0;
    double lastModuleRefresh = 0.0;
    double lastThreadRefresh = -1e9;
    s.c.log.reserve(1 << 20);
    while (!s.stop.load(std::memory_order_acquire)) {
        const double relMs = 1000.0 * double(qpcNow() - s.qpcStart) / double(s.qpcFreq);
        if (relMs < nextDue) {
#if BRONZE_PROFILER_WIN32
            ::Sleep(1);
#else
            std::this_thread::sleep_for(std::chrono::microseconds(
                std::max<int64_t>(50, static_cast<int64_t>((nextDue - relMs) * 1000.0))));
#endif
            continue;
        }
        nextDue = relMs + periodMs;
        if (relMs - lastModuleRefresh > 1000.0) {
            lastModuleRefresh = relMs;
            s.modules.refresh();
        }
        if (s.processThreads && relMs - lastThreadRefresh > 1000.0) {
            lastThreadRefresh = relMs;
#if BRONZE_PROFILER_WIN32
            refreshOthers(s);
#else
            refreshOthers(s, otherBounds);
#endif
        }
        size_t used = 0;
        {
            // Under the registry lock, so a registered thread cannot exit
            // (its handle closed, its thread-local slot freed) mid-capture.
            std::lock_guard<std::mutex> lock(registryMutex());
            const auto& threads = registered();
            if (s.raws.size() < threads.size() + s.others.size()) s.raws.resize(threads.size() + s.others.size());
            for (const RegisteredThread& t : threads) {
                if (!(t.kind & s.mask)) continue;
                Raw& raw = s.raws[used++];
                raw.threadId = t.id;
                raw.kind = t.kind;
#if BRONZE_PROFILER_WIN32
                capture(s, t.handle, t.topSlot, raw);
#else
                capture(s, t.handle, t.topSlot, raw, t.stackLo, t.stackHi);
#endif
                if (raw.nNative || raw.nInterp) raw.nameCopy = t.name;
            }
        }
        for (size_t o = 0; o < s.others.size(); ++o) {
            Raw& raw = s.raws[used++];
            raw.threadId = s.others[o].first;
            raw.kind = kProfilerThreadOther;
            raw.nameCopy.clear();
#if BRONZE_PROFILER_WIN32
            capture(s, s.others[o].second, nullptr, raw);
#else
            capture(s, s.others[o].second, nullptr, raw, otherBounds[o].first, otherBounds[o].second);
#endif
        }
        if (s.c.log.size() + used * (1 + 2 * (kMaxNative + kMaxInterp)) > kMaxWords) {
            s.c.truncated = true;
            break;
        }
        for (size_t i = 0; i < used; ++i) record(s, s.raws[i], static_cast<uint64_t>(relMs));
    }
    for (auto& [id, h] : s.others) releaseThread(h);
    s.others.clear();
#if BRONZE_PROFILER_WIN32
    ::timeEndPeriod(1);
#endif
}

#if BRONZE_PROFILER_WIN32
DWORD WINAPI samplerLoop(LPVOID param) {
    samplerBody(*static_cast<Session*>(param));
    return 0;
}
#endif

}  // namespace

#if BRONZE_PROFILER_DARWIN

void registerProfilerThread(uint32_t kind, std::string_view name) {
    RegisteredThread t;
    t.id = currentThreadId();
    t.kind = kind;
    t.name = std::string(name);
    t.topSlot = &brass::FastInterpreter::thread_frame_top();
    // A send right of our own (mach_thread_self adds one), released at
    // unregister.
    t.handle = mach_thread_self();
    stackBounds(pthread_self(), t.stackLo, t.stackHi);
    std::lock_guard<std::mutex> lock(registryMutex());
    for (RegisteredThread& existing : registered()) {
        if (existing.id == t.id) {
            releaseThread(existing.handle);
            existing = std::move(t);
            return;
        }
    }
    registered().push_back(std::move(t));
}

void unregisterProfilerThread() {
    const ThreadId id = currentThreadId();
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& v = registered();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].id != id) continue;
        releaseThread(v[i].handle);
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

bool profilerStart(const ProfilerOptions& options, std::string* error) {
    std::lock_guard<std::mutex> lock(g_sessionMu);
    if (g_session) {
        if (error) *error = "a profile is already running";
        return false;
    }
    auto* s = new Session();
    s->hz = options.hz < 50 ? 50 : options.hz > 4000 ? 4000 : options.hz;
    s->mask = options.threads;
    s->processThreads = options.processThreads;
    s->c.hz = s->hz;
    s->qpcFreq = qpcFrequency();
    s->qpcStart = qpcNow();
    try {
        s->thread = std::thread([s] { samplerBody(*s); });
    } catch (...) {
        delete s;
        if (error) *error = "could not create the sampler thread";
        return false;
    }
    g_session = s;
    return true;
}

bool profilerRunning() {
    std::lock_guard<std::mutex> lock(g_sessionMu);
    return g_session != nullptr;
}

bool profilerStop(const ProfilerStopOptions& options, ProfilerResult& out, std::string* error) {
    Session* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        s = g_session;
        g_session = nullptr;
    }
    if (!s) {
        if (error) *error = "no profile is running";
        return false;
    }
    s->stop.store(true, std::memory_order_release);
    s->thread.join();
    s->c.durationMs = 1000.0 * double(qpcNow() - s->qpcStart) / double(s->qpcFreq);
    aggregate(s->c, options, out);
    delete s;
    return true;
}

#else  // BRONZE_PROFILER_WIN32

void registerProfilerThread(uint32_t kind, std::string_view name) {
    RegisteredThread t;
    t.id = ::GetCurrentThreadId();
    t.kind = kind;
    t.name = std::string(name);
    t.topSlot = &brass::FastInterpreter::thread_frame_top();
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &t.handle,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return;
    }
    std::lock_guard<std::mutex> lock(registryMutex());
    for (RegisteredThread& existing : registered()) {
        if (existing.id == t.id) {
            ::CloseHandle(existing.handle);
            existing = std::move(t);
            return;
        }
    }
    registered().push_back(std::move(t));
}

void unregisterProfilerThread() {
    const DWORD id = ::GetCurrentThreadId();
    std::lock_guard<std::mutex> lock(registryMutex());
    auto& v = registered();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].id != id) continue;
        ::CloseHandle(v[i].handle);
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

bool profilerStart(const ProfilerOptions& options, std::string* error) {
    std::lock_guard<std::mutex> lock(g_sessionMu);
    if (g_session) {
        if (error) *error = "a profile is already running";
        return false;
    }
    auto* s = new Session();
    s->hz = options.hz < 50 ? 50 : options.hz > 4000 ? 4000 : options.hz;
    s->mask = options.threads;
    s->processThreads = options.processThreads;
    s->c.hz = s->hz;
    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    s->qpcFreq = static_cast<uint64_t>(freq.QuadPart);
    s->qpcStart = qpcNow();
    DWORD tid = 0;
    s->thread = ::CreateThread(nullptr, 0, samplerLoop, s, CREATE_SUSPENDED, &tid);
    if (!s->thread) {
        delete s;
        if (error) *error = "could not create the sampler thread";
        return false;
    }
    s->samplerId = tid;
    ::ResumeThread(s->thread);
    g_session = s;
    return true;
}

bool profilerRunning() {
    std::lock_guard<std::mutex> lock(g_sessionMu);
    return g_session != nullptr;
}

bool profilerStop(const ProfilerStopOptions& options, ProfilerResult& out, std::string* error) {
    Session* s = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        s = g_session;
        g_session = nullptr;
    }
    if (!s) {
        if (error) *error = "no profile is running";
        return false;
    }
    s->stop.store(true, std::memory_order_release);
    ::WaitForSingleObject(s->thread, INFINITE);
    ::CloseHandle(s->thread);
    s->c.durationMs = 1000.0 * double(qpcNow() - s->qpcStart) / double(s->qpcFreq);
    aggregate(s->c, options, out);
    delete s;
    return true;
}

#endif  // BRONZE_PROFILER_DARWIN / WIN32

#endif  // BRONZE_PROFILER_SUPPORTED

}  // namespace bronze::runtime
