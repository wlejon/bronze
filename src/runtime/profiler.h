#pragma once

// The script-armed sampling profiler: the same suspend-and-walk sampler as
// BRONZE_SAMPLE (sampler.h), started and stopped by the host at run time over
// a window of its own choosing, with the result handed back as data rather
// than written at exit. The host reaches it through embed/embed_profiler.h.
//
// Threads are sampled only once registered (registerProfilerThread, called on
// the thread itself: it is what captures the thread's interpreter frame chain,
// which is thread-local), or, with `processThreads`, every thread of the
// process. Nothing runs while stopped: no sampler thread exists, and a
// registration is one mutex and one push, once per thread.
//
// Each sample is one suspended thread's native stack, walked through the
// modules' unwind data and brass's JIT tables (stack_sampling.h), plus the
// FastInterpreter frames running on it. Every frame is classified right after
// the thread is resumed, while the code it names is still installed:
//
//   interpreter   a JS function the fast interpreter was running (Tier 0)
//   tier 1        brass baseline JIT code
//   tier 2        brass optimizing JIT code ("tier 2 osr" entered mid-loop)
//   aot           a JS function compiled ahead of time (an app.dll, bro's own)
//   stub          JIT trampolines and lazy-link stubs
//   native        everything else: runtime helpers, the host, the OS
//
// Self time goes to the innermost frame that is not the interpreter's own
// dispatch loop, so a function the interpreter runs is billed for its
// bytecode while a runtime helper it calls is billed to the helper.
//
// Windows x64 only; elsewhere start() reports that it is unsupported.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct bronze_fn_desc;

namespace bronze::runtime {

enum ProfilerThreadKind : uint32_t {
    kProfilerThreadMain = 1,
    kProfilerThreadWorker = 2,
    kProfilerThreadOther = 4,
};

struct ProfilerOptions {
    uint32_t hz = 1000;  // clamped to [50, 4000]
    // A mask of ProfilerThreadKind: which REGISTERED threads to sample.
    uint32_t threads = kProfilerThreadMain;
    // Every other thread of the process too (compile workers, audio, the
    // host's), reported as kind "other".
    bool processThreads = false;
};

struct ProfilerStopOptions {
    bool callers = false;  // caller -> callee edges
    bool text = false;     // a text table, the shape BRONZE_SAMPLE prints
    uint32_t top = 40;     // rows per text table
};

struct ProfilerFunction {
    std::string name;
    std::string tier;    // see the header comment
    std::string module;  // the image a native or aot frame is in; "jit" / "interpreter"
    std::string file;    // a JS function's source file, when known
    uint32_t line = 0;   // its definition line, when known
    uint64_t self = 0;
    uint64_t total = 0;
};

struct ProfilerEdge {
    uint32_t caller = 0;  // index into functions
    uint32_t callee = 0;
    uint64_t count = 0;
};

struct ProfilerThread {
    uint32_t id = 0;
    std::string kind;  // "main", "worker", "other"
    std::string name;
    uint64_t samples = 0;
};

struct ProfilerResult {
    uint32_t hz = 0;
    double durationMs = 0;
    uint64_t samples = 0;
    bool truncated = false;
    std::vector<ProfilerFunction> functions;  // by self, then total, descending
    std::vector<ProfilerEdge> edges;          // by count, descending
    std::vector<ProfilerThread> threads;
    std::string text;
};

void registerProfilerThread(uint32_t kind, std::string_view name);
void unregisterProfilerThread();

bool profilerStart(const ProfilerOptions& options, std::string* error);
bool profilerRunning();
// Stops the sampler, joins it and aggregates. False with `*error` when no
// profile is running.
bool profilerStop(const ProfilerStopOptions& options, ProfilerResult& out, std::string* error);

// A MIR function name (what the interpreter and brass's JIT registry know a
// function by) to the descriptor of the JS function it compiles, installed by
// the engine that loads programs (codegen-brass's tiered image). Called from
// the sampler thread, never while a target is suspended.
using MirNameDescriber = const bronze_fn_desc* (*)(std::string_view mirName);
void setMirNameDescriber(MirNameDescriber describer) noexcept;

}  // namespace bronze::runtime
