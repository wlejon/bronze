// Running one program under a hard timeout with its two output streams
// captured separately, for the oracle harness. A built program and a
// `bronze run` of the same source go through the same function, so what the
// harness compares between the two paths (stdout bytes, exit code, the
// uncaught-error report on stderr) was collected the same way for both.
//
// A subprocess in both cases, deliberately: the runtime is process-global
// with no teardown, so a second program in the same process would see the
// first one's globals, and a miscompiled loop can only be stopped by killing
// the process that runs it. The JIT half is still the in-process path the
// engine uses — `bronze run` calls eval::evalFile exactly as bro's
// eval_jit.cpp does — it is merely isolated per case.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace oracle {

// The limits stop a hung or miscompiled case; they are not a speed check, so
// they are sized for the slowest machine the suite runs on. With four
// workers a hosted CI runner (four hardware threads on two cores) gives each
// case about a third of the core these times were measured on: the slowest
// plain runs (tiers_19, tiers_22 at tier 0) are 9 s here and near 30 s there,
// and the slowest stressed one (async_suspend_protocol under gc-stress at
// tier 2, which scans a 16,000-frame stack at every allocation on the way to
// its RangeError) is 48 s here and near 150 s there.
constexpr uint32_t kRunTimeoutMs = 60000;
constexpr uint32_t kStressTimeoutMs = 300000;

struct RunResult {
    bool ran = false;       // process started and exited on its own
    bool timedOut = false;  // killed after the timeout
    int exitCode = -1;      // 0 on clean exit; 128+signal where the OS says so
    std::string output;     // stdout, byte for byte
    std::string errors;     // stderr, byte for byte
};

inline std::string quoted(const std::string& arg) { return "\"" + arg + "\""; }

// BRASS_DEOPT_STRESS set to anything but off (`1`/`all`, a period `<N>`, a
// `site:` selection), inherited by every run: the guards it picks take
// their exits, so each forced failure runs a site's slow path (the generic
// helper, an inline-cache refill) where the fast path would have run. Every
// third evaluation already doubles a tier-0 run (tiers_19 and tiers_22: about
// 9 s plain, 17 s at period 3 and 30-34 s at period 1 on an idle machine),
// which a loaded machine pushes past the plain limit, so every stressed
// run gets the stress limit, not only period 1.
inline bool deoptStressActive() {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    bool active = false;
    if (_dupenv_s(&env, &len, "BRASS_DEOPT_STRESS") == 0 && env != nullptr) {
        const std::string v(env);
        active = !v.empty() && v != "0" && v != "off";
    }
    free(env);
    return active;
#else
    const char* env = std::getenv("BRASS_DEOPT_STRESS");
    if (env == nullptr) return false;
    const std::string v(env);
    return !v.empty() && v != "0" && v != "off";
#endif
}

// The limit a run gets: the caller's, except that a run at the default limit
// under gc-stress or deopt stress gets the stress limit.
inline uint32_t effectiveTimeout(bool gcStress, uint32_t timeoutMs) {
    static const bool s_deoptStress = deoptStressActive();
    return (timeoutMs == kRunTimeoutMs && (gcStress || s_deoptStress)) ? kStressTimeoutMs : timeoutMs;
}

#ifdef _WIN32
inline RunResult runCommand(const std::string& cmdLine, bool gcStress = false,
                            uint32_t timeoutMs = kRunTimeoutMs) {
    RunResult result;
    const uint32_t effectiveTimeoutMs = effectiveTimeout(gcStress, timeoutMs);

    HANDLE outRead = nullptr;
    HANDLE errRead = nullptr;
    PROCESS_INFORMATION pi{};

    // The whole spawn is one critical section, from the pipes to the child:
    // the environment is process-wide, and an INHERITABLE pipe end that
    // exists while another worker's CreateProcess runs is inherited by that
    // worker's child too, which then holds this pipe open until it exits —
    // and so this read reaches EOF only when the other case has finished,
    // serialising the workers into a chain. Both write ends are closed
    // before the lock is dropped, so no child but ours ever sees them.
    static std::mutex s_spawnMutex;
    {
        std::lock_guard<std::mutex> lock(s_spawnMutex);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE outWrite = nullptr;
        HANDLE errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return result;
        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return result;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        std::string mutableCmd = cmdLine;
        _putenv_s("BRONZE_GC_STRESS", gcStress ? "1" : "");
        BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                 nullptr, &si, &pi);
        _putenv_s("BRONZE_GC_STRESS", "");
        CloseHandle(outWrite);  // ours would keep the pipes open past child exit
        CloseHandle(errWrite);
        if (!ok) {
            CloseHandle(outRead);
            CloseHandle(errRead);
            return result;
        }
    }

    // Drain each pipe on its own thread so a chatty child can never fill a
    // pipe buffer and deadlock against our process-handle wait.
    auto drain = [](HANDLE pipe, std::string& into) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) into.append(buf, n);
    };
    std::thread outReader(drain, outRead, std::ref(result.output));
    std::thread errReader(drain, errRead, std::ref(result.errors));

    DWORD wait = WaitForSingleObject(pi.hProcess, effectiveTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    outReader.join();
    errReader.join();
    CloseHandle(outRead);
    CloseHandle(errRead);
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        result.exitCode = static_cast<int>(code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.ran = !result.timedOut;
    return result;
}
#else
constexpr rlim_t kChildStackBytes = 8 * 1024 * 1024;

// Closes every descriptor above stderr but `keep` (ascending). A fork copies
// all of the harness's descriptors, including ones other workers have open
// without close-on-exec, such as an executable a build is still writing:
// while any process holds that open for writing, running it fails with
// ETXTBSY ("Text file busy"). Async-signal-safe.
inline void closeInheritedFds(const int* keep, int nkeep) {
#if defined(__APPLE__)
    proc_fdinfo fds[1024];
    const int bytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, fds, sizeof(fds));
    if (bytes > 0 && static_cast<size_t>(bytes) < sizeof(fds)) {
        for (int i = 0; i < bytes / static_cast<int>(sizeof(proc_fdinfo)); ++i) {
            const int fd = fds[i].proc_fd;
            bool kept = fd <= 2;
            for (int k = 0; k < nkeep && !kept; ++k) kept = keep[k] == fd;
            if (!kept) ::close(fd);
        }
        return;
    }
#elif defined(__linux__) && defined(SYS_close_range)
    int lo = 3;
    bool ok = true;
    for (int k = 0; k <= nkeep && ok; ++k) {
        const unsigned hi = k < nkeep ? static_cast<unsigned>(keep[k] - 1) : ~0u;
        if (k < nkeep && keep[k] < lo) continue;
        if (static_cast<unsigned>(lo) <= hi) ok = ::syscall(SYS_close_range, lo, hi, 0) == 0;
        if (k < nkeep) lo = keep[k] + 1;
    }
    if (ok) return;
#endif
    struct rlimit files;
    rlim_t top = 65536;
    if (::getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur != RLIM_INFINITY && files.rlim_cur < top) {
        top = files.rlim_cur;
    }
    for (int fd = 3; static_cast<rlim_t>(fd) < top; ++fd) {
        bool kept = false;
        for (int k = 0; k < nkeep && !kept; ++k) kept = keep[k] == fd;
        if (!kept) ::close(fd);
    }
}

// The forked child's watchdog half (see runCommand): it waits for whichever
// comes first, the run ending (EOF on `doneRead`, whose write end only the
// run's processes hold) or the harness going away (EOF on `lifelineRead`,
// whose write end only the harness holds), and in the second case kills
// its whole process group, itself included. Otherwise it reaps the run and
// exits with its status, a signal death as 128 + the signal, the number the
// harness reports for one. Async-signal-safe calls only: it runs in a fork
// of a threaded process and never execs.
[[noreturn]] inline void watchRun(pid_t runner, int lifelineRead, int doneRead) {
    pollfd fds[2] = {{lifelineRead, POLLIN, 0}, {doneRead, POLLIN, 0}};
    for (;;) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents != 0) ::kill(0, SIGKILL);
        if (fds[1].revents != 0) break;
    }
    int status = 0;
    while (::waitpid(runner, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) ::_exit(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) ::_exit(128 + WTERMSIG(status));
    ::_exit(127);
}

// The command line goes through `/bin/sh -c`, as it did under popen, so the
// harness's quoting and `VAR=value` prefixes mean the same thing.
//
// The forked child leads a process group of its own (setpgid) and forks
// again: the grandchild execs the shell, the child stays behind as its
// watchdog (watchRun). A timeout kills that whole group: the watchdog, the
// shell, the program it started, and anything that program started. And
// when the harness itself dies first (ctest's own timeout, a signal, a
// dropped ssh session), the watchdog sees the lifeline pipe close and kills
// the group, so no run outlives the harness that started it. A process in a
// group of its own is out of reach of whatever killed the harness, and
// without the lifeline it would run on, orphaned, for as long as it takes.
//
// The child closes everything else it inherited first (closeInheritedFds):
// a watchdog never execs, so close-on-exec would not do it, and it would
// hold other runs' lifelines and other workers' half-written files open for
// as long as its run lasts.
inline RunResult runCommand(const std::string& cmdLine, bool gcStress = false,
                            uint32_t timeoutMs = kRunTimeoutMs) {
    RunResult result;
    const uint32_t effectiveTimeoutMs = effectiveTimeout(gcStress, timeoutMs);
    // The stack a program gets from a default shell. ctest raises the soft
    // limit to the hard one (64 MB on macOS), and a case that recurses until
    // RangeError then goes eight times as deep: under gc-stress, which walks
    // the whole stack at every allocation, that is 64 times the work, and
    // the pinned output would be compared against a stack nobody runs with.
    // The shell lowers it, not the forked child: on macOS a setrlimit of
    // RLIMIT_STACK in a fork made from a worker thread (every oracle run's
    // case) leaves the stack the program gets at exec unchanged.
    const std::string cmd = "s=$(ulimit -s); if [ \"$s\" = unlimited ] || [ \"$s\" -gt " +
                            std::to_string(kChildStackBytes / 1024) + " ]; then ulimit -s " +
                            std::to_string(kChildStackBytes / 1024) + "; fi; " +
                            (gcStress ? "BRONZE_GC_STRESS=1 " : "") + cmdLine;

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int lifeline[2] = {-1, -1};
    pid_t pid = -1;

    // Pipe creation through fork is one critical section, for the reason the
    // Windows half gives: every pipe end is close-on-exec before another
    // worker can fork, so no other case's child keeps this case's pipes open
    // (macOS has no pipe2, so the flag is set after the fact, under the lock).
    static std::mutex s_spawnMutex;
    {
        std::lock_guard<std::mutex> lock(s_spawnMutex);
        if (::pipe(outPipe) != 0) return result;
        if (::pipe(errPipe) != 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            return result;
        }
        if (::pipe(lifeline) != 0) {
            for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) ::close(fd);
            return result;
        }
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1], lifeline[0], lifeline[1]}) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        pid = ::fork();
        if (pid == 0) {
            // Async-signal-safe calls only from here on (a fork of a
            // threaded process).
            ::setpgid(0, 0);
            int keep[3] = {outPipe[1], errPipe[1], lifeline[0]};
            for (int i = 1; i < 3; ++i) {
                for (int j = i; j > 0 && keep[j - 1] > keep[j]; --j) std::swap(keep[j - 1], keep[j]);
            }
            closeInheritedFds(keep, 3);  // lifeline[1] among them
            // The run holds the write end of `done` (not close-on-exec, so
            // the shell and the program keep it); EOF on the read end is the
            // watchdog's sign that the run is over.
            int done[2] = {-1, -1};
            if (::pipe(done) != 0) ::_exit(127);
            const pid_t runner = ::fork();
            if (runner != 0) {
                ::close(done[1]);
                ::close(outPipe[1]);
                ::close(errPipe[1]);
                if (runner < 0) ::_exit(127);
                watchRun(runner, lifeline[0], done[0]);
            }
            ::close(done[0]);
            ::close(lifeline[0]);
            ::dup2(outPipe[1], STDOUT_FILENO);  // dup2 clears close-on-exec
            ::dup2(errPipe[1], STDERR_FILENO);
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (pid > 0) ::setpgid(pid, pid);  // also set here: no race with the kill below
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        ::close(lifeline[0]);
        if (pid < 0) {
            ::close(outPipe[0]);
            ::close(errPipe[0]);
            ::close(lifeline[1]);
            return result;
        }
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(effectiveTimeoutMs);
    pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    std::string* sinks[2] = {&result.output, &result.errors};
    int open = 2;
    char buf[4096];
    while (open > 0) {
        int waitMs = -1;
        if (!result.timedOut) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) {
                // Past the limit: kill the group, then keep draining until
                // the pipes reach EOF, which they do once the group is gone.
                result.timedOut = true;
                ::kill(-pid, SIGKILL);
                continue;
            }
            waitMs = static_cast<int>(left);
        }
        const int ready = ::poll(fds, 2, waitMs);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || fds[i].revents == 0) continue;
            const ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
            if (n > 0) {
                sinks[i]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                ::close(fds[i].fd);
                fds[i].fd = -1;  // poll ignores a negative fd
                --open;
            }
        }
    }
    for (pollfd& p : fds) {
        if (p.fd >= 0) ::close(p.fd);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    ::close(lifeline[1]);  // only now: closing it tells the watchdog to kill the run
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
    result.ran = !result.timedOut;
    return result;
}
#endif

}  // namespace oracle
