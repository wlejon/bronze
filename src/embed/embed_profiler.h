#pragma once

// The sampling profiler on the host surface (runtime/profiler.h): a host
// registers the threads that run JS, starts a profile over a window of its
// own choosing and gets the result back as data. A companion of embed.h,
// carrying BRONZE_EMBED_API for the same reason it does: the profiler's
// thread registry and sampler must be the ONE runtime's.
//
//   bronze::embed::profilerRegisterThread(bronze::runtime::kProfilerThreadMain, "main");
//   bronze::embed::profilerStart({.hz = 2000}, &err);
//   ... the window ...
//   bronze::runtime::ProfilerResult r;
//   bronze::embed::profilerStop({.callers = true}, r, &err);
//
// Stopped, it costs nothing: no thread, no hook on any call path.

#include "embed/embed.h"
#include "runtime/profiler.h"

namespace bronze::embed {

// Declares the CALLING thread a JS thread of `kind` (a
// runtime::ProfilerThreadKind), sampled by a profile that selects the kind.
// Call once on the thread itself, before it runs JS; unregister before it
// exits. Registering twice replaces the first registration.
BRONZE_EMBED_API void profilerRegisterThread(uint32_t kind, std::string_view name);
BRONZE_EMBED_API void profilerUnregisterThread();

BRONZE_EMBED_API bool profilerStart(const runtime::ProfilerOptions& options, std::string* error);
BRONZE_EMBED_API bool profilerRunning();
BRONZE_EMBED_API bool profilerStop(const runtime::ProfilerStopOptions& options, runtime::ProfilerResult& out,
                                   std::string* error);

// How the profiler names a function it saw by its MIR name (interpreted, or
// in brass's JIT registry): installed by the engine that loads programs.
BRONZE_EMBED_API void setMirNameDescriber(runtime::MirNameDescriber describer);
// Why the baseline tier rejected a function, by its MIR name: installed by
// the same engine.
BRONZE_EMBED_API void setMirNameTier1Rejection(runtime::MirNameTier1Rejection lookup);

}  // namespace bronze::embed
