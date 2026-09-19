#pragma once

#include <chrono>
#include <cstdio>

namespace bronze::support {

// Whether `bronze build --timings` was asked for.
//
// A duration is the one thing bronze prints that cannot be deterministic, and
// The determinism rule is about bronze's own output — so the whole mechanism is
// off unless asked for, writes to stderr, and is compared by nothing. The flag
// is process-global rather than a parameter because the two places that report
// are the CLI and the brass backend, and threading a bool through
// `codegen::Backend::emitObject` would put a debugging concern into the
// interface every future backend has to implement.
bool timingsEnabled();
void setTimingsEnabled(bool on);

// Wall time per phase, printed to stderr as each `mark` lands. The CLI reports
// the five top-level phases with it and the backend reports the inside of its
// own, indented one level deeper, so a 47 s "codegen" line says which of
// translation, brass's compile and the section tables to attack. Constructed
// disabled it costs nothing and prints nothing.
class PhaseTimer {
public:
    explicit PhaseTimer(bool enabled, int indent = 2) : enabled_(enabled), indent_(indent) {
        if (enabled_) start_ = last_ = Clock::now();
    }

    void mark(const char* phase) {
        if (!enabled_) return;
        const auto now = Clock::now();
        std::fprintf(stderr, "%*s%-14s %8.1f ms\n", indent_, "", phase, millisSince(last_, now));
        last_ = now;
    }

    void total() {
        if (!enabled_) return;
        const auto now = Clock::now();
        std::fprintf(stderr, "%*s%-14s %8.1f ms\n", indent_, "", "total", millisSince(start_, now));
    }

private:
    using Clock = std::chrono::steady_clock;
    static double millisSince(Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    bool enabled_;
    int indent_;
    Clock::time_point start_{};
    Clock::time_point last_{};
};

}  // namespace bronze::support
