#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bronze::host {

struct ProcOptions {
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> envOverrides;
    int64_t timeoutMs = 0;  // <= 0 means no timeout
    bool mergeStderr = false;
    std::vector<std::string> stopOnMatch;  // stop + terminate child when any line contains a needle
    std::string liveLogPath;               // optional live line log file
};

struct ProcResult {
    uint64_t id = 0;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    bool timedOut = false;
    bool stoppedEarly = false;
    int marks = 0;
    std::string spawnError;
};

// Spawn a child process asynchronously and return its handle id (> 0).
// Stdout and stderr are drained concurrently in the background without shell
// expansion, and stdin is redirected from /dev/null (NUL on Windows).
uint64_t spawnProcess(const std::vector<std::string>& argv, const ProcOptions& opts);

// Block until the child identified by `id` finishes and return its result.
ProcResult waitProcess(uint64_t id);

// Block until any active spawned child finishes and return its result, or
// std::nullopt if no spawned children remain.
std::optional<ProcResult> waitAnyProcess();

// Spawn and immediately wait for completion.
ProcResult execProcess(const std::vector<std::string>& argv, const ProcOptions& opts);

// Linux perf control-fifo marked execution (`perf ... -D -1 --control fifo:ctl,ack -- <cmd>`).
// Watches stdout for `markOn` and `markOff` and sends `enable`/`disable` over the control fifo.
ProcResult execPerfMarked(const std::vector<std::string>& perfPrefix,
                          const std::vector<std::string>& childCmd,
                          const std::string& markOn,
                          const std::string& markOff,
                          const ProcOptions& opts);

}  // namespace bronze::host
