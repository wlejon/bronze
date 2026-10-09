#include "embed/embed_profiler.h"

namespace bronze::embed {

void profilerRegisterThread(uint32_t kind, std::string_view name) { runtime::registerProfilerThread(kind, name); }

void profilerUnregisterThread() { runtime::unregisterProfilerThread(); }

bool profilerStart(const runtime::ProfilerOptions& options, std::string* error) {
    return runtime::profilerStart(options, error);
}

bool profilerRunning() { return runtime::profilerRunning(); }

bool profilerStop(const runtime::ProfilerStopOptions& options, runtime::ProfilerResult& out, std::string* error) {
    return runtime::profilerStop(options, out, error);
}

void setMirNameDescriber(runtime::MirNameDescriber describer) { runtime::setMirNameDescriber(describer); }

void setMirNameTier1Rejection(runtime::MirNameTier1Rejection lookup) { runtime::setMirNameTier1Rejection(lookup); }

}  // namespace bronze::embed
