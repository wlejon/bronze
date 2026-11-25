#include "host/cli_proc.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace bronze::host {
namespace {

using SteadyClock = std::chrono::steady_clock;

#if defined(_WIN32)

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const wchar_t* s, int len = -1) {
    if (!s || len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    if (len < 0 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// Standard Win32 CommandLineToArgvW quoting.
std::wstring quoteWinArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes > 0) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out.push_back(c);
        }
    }
    if (backslashes > 0) out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring buildWinCommandLine(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) cmd.push_back(L' ');
        cmd += quoteWinArg(utf8ToWide(argv[i]));
    }
    return cmd;
}

std::vector<wchar_t> buildWinEnvBlock(const std::vector<std::pair<std::string, std::string>>& overrides) {
    if (overrides.empty()) return {};
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (LPWCH raw = GetEnvironmentStringsW()) {
        for (const wchar_t* p = raw; *p != L'\0'; ) {
            const size_t len = std::wcslen(p);
            const std::wstring entry(p, len);
            const size_t eq = entry.find(L'=', 1);
            if (eq != std::wstring::npos) {
                entries.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            }
            p += len + 1;
        }
        FreeEnvironmentStringsW(raw);
    }
    for (const auto& [k, v] : overrides) {
        std::wstring wk = utf8ToWide(k);
        std::wstring wv = utf8ToWide(v);
        bool replaced = false;
        for (auto& e : entries) {
            if (_wcsicmp(e.first.c_str(), wk.c_str()) == 0) {
                e.second = wv;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.emplace_back(std::move(wk), std::move(wv));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& [k, v] : entries) {
        block.insert(block.end(), k.begin(), k.end());
        block.push_back(L'=');
        block.insert(block.end(), v.begin(), v.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void readWinPipe(HANDLE h, std::string& out) {
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
    }
}

ProcResult runChildBlocking(uint64_t id, const std::vector<std::string>& argv, const ProcOptions& opts) {
    ProcResult res;
    res.id = id;
    if (argv.empty()) {
        res.spawnError = "spawn: empty command";
        return res;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRd = nullptr, outWr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        res.spawnError = "spawn: CreatePipe(stdout) failed";
        return res;
    }
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        CloseHandle(outRd);
        CloseHandle(outWr);
        res.spawnError = "spawn: CreatePipe(stderr) failed";
        return res;
    }
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nulIn != INVALID_HANDLE_VALUE) ? nulIn : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outWr;
    si.hStdError = errWr;

    std::wstring cmdLine = buildWinCommandLine(argv);
    std::wstring cwdW = opts.cwd.empty() ? std::wstring() : utf8ToWide(opts.cwd);
    std::vector<wchar_t> envBlock = buildWinEnvBlock(opts.envOverrides);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!envBlock.empty()) flags |= CREATE_UNICODE_ENVIRONMENT;

    const BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        envBlock.empty() ? nullptr : envBlock.data(),
        cwdW.empty() ? nullptr : cwdW.c_str(),
        &si,
        &pi);

    CloseHandle(outWr);
    CloseHandle(errWr);
    if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);

    if (!ok) {
        const DWORD err = GetLastError();
        CloseHandle(outRd);
        CloseHandle(errRd);
        if (job) CloseHandle(job);
        res.spawnError = "spawn: CreateProcessW failed (error " + std::to_string(err) + ")";
        return res;
    }

    bool inJob = false;
    if (job) inJob = AssignProcessToJobObject(job, pi.hProcess) != FALSE;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::thread tOut([&]() { readWinPipe(outRd, res.stdoutText); });
    std::thread tErr([&]() { readWinPipe(errRd, res.stderrText); });

    const DWORD waitMs = (opts.timeoutMs > 0)
        ? static_cast<DWORD>(std::min<int64_t>(opts.timeoutMs, INFINITE - 1))
        : INFINITE;
    const DWORD waitRc = WaitForSingleObject(pi.hProcess, waitMs);
    if (waitRc == WAIT_TIMEOUT) {
        res.timedOut = true;
        if (inJob) TerminateJobObject(job, 1);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    tOut.join();
    tErr.join();
    CloseHandle(outRd);
    CloseHandle(errRd);

    if (res.timedOut) {
        res.exitCode = -1;
    } else {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code)) {
            res.exitCode = static_cast<int>(code);
        } else {
            res.exitCode = -1;
        }
    }

    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return res;
}

#else  // POSIX

char** hostEnviron() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

bool isExecutableFile(const std::string& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return ::access(p.c_str(), X_OK) == 0;
}

std::string resolvePosixExe(const std::string& prog, const std::string& pathEnv) {
    if (prog.empty()) return {};
    if (prog.find('/') != std::string::npos) return prog;
    size_t pos = 0;
    while (pos <= pathEnv.size()) {
        size_t next = pathEnv.find(':', pos);
        if (next == std::string::npos) next = pathEnv.size();
        std::string dir = (next == pos) ? "." : pathEnv.substr(pos, next - pos);
        std::string cand = dir + "/" + prog;
        if (isExecutableFile(cand)) return cand;
        if (next == pathEnv.size()) break;
        pos = next + 1;
    }
    return {};
}

std::vector<std::string> buildPosixEnv(const std::vector<std::pair<std::string, std::string>>& overrides,
                                       std::string& outPathEnv) {
    std::vector<std::pair<std::string, std::string>> entries;
    if (char** envp = hostEnviron()) {
        for (char** p = envp; *p != nullptr; ++p) {
            const std::string_view sv(*p);
            const size_t eq = sv.find('=');
            if (eq != std::string_view::npos) {
                entries.emplace_back(std::string(sv.substr(0, eq)), std::string(sv.substr(eq + 1)));
            }
        }
    }
    for (const auto& [k, v] : overrides) {
        bool replaced = false;
        for (auto& e : entries) {
            if (e.first == k) {
                e.second = v;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.emplace_back(k, v);
    }
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const auto& [k, v] : entries) {
        if (k == "PATH") outPathEnv = v;
        out.push_back(k + "=" + v);
    }
    return out;
}

bool setCloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

ProcResult runChildBlocking(uint64_t id, const std::vector<std::string>& argv, const ProcOptions& opts) {
    ProcResult res;
    res.id = id;
    if (argv.empty()) {
        res.spawnError = "spawn: empty command";
        return res;
    }

    std::string pathEnv;
    std::vector<std::string> envStrings = buildPosixEnv(opts.envOverrides, pathEnv);
    const std::string exePath = resolvePosixExe(argv[0], pathEnv);
    if (exePath.empty()) {
        res.spawnError = "spawn: executable not found: " + argv[0];
        return res;
    }

    std::vector<std::string> argvCopy = argv;
    std::vector<char*> argvPtrs;
    argvPtrs.reserve(argvCopy.size() + 1);
    for (auto& s : argvCopy) argvPtrs.push_back(s.data());
    argvPtrs.push_back(nullptr);

    std::vector<char*> envPtrs;
    envPtrs.reserve(envStrings.size() + 1);
    for (auto& s : envStrings) envPtrs.push_back(s.data());
    envPtrs.push_back(nullptr);

    const char* cwdPtr = opts.cwd.empty() ? nullptr : opts.cwd.c_str();

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int execPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
        res.spawnError = "spawn: pipe(stdout) failed";
        return res;
    }
    if (::pipe(errPipe) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        res.spawnError = "spawn: pipe(stderr) failed";
        return res;
    }
    if (::pipe(execPipe) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        res.spawnError = "spawn: pipe(exec) failed";
        return res;
    }
    setCloexec(outPipe[0]);
    setCloexec(errPipe[0]);
    setCloexec(execPipe[0]);
    setCloexec(execPipe[1]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        ::close(execPipe[0]);
        ::close(execPipe[1]);
        res.spawnError = std::string("spawn: fork failed: ") + std::strerror(e);
        return res;
    }

    if (pid == 0) {
        // Child: async-signal-safe calls only.
        (void)::setpgid(0, 0);
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            (void)::dup2(devNull, STDIN_FILENO);
            if (devNull != STDIN_FILENO) ::close(devNull);
        }
        (void)::dup2(outPipe[1], STDOUT_FILENO);
        (void)::dup2(errPipe[1], STDERR_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        ::close(execPipe[0]);

        if (cwdPtr && ::chdir(cwdPtr) != 0) {
            const int e = errno;
            const ssize_t ignored = ::write(execPipe[1], &e, sizeof(e));
            (void)ignored;
            ::_exit(127);
        }

        ::execve(exePath.c_str(), argvPtrs.data(), envPtrs.data());
        const int e = errno;
        const ssize_t ignored = ::write(execPipe[1], &e, sizeof(e));
        (void)ignored;
        ::_exit(127);
    }

    // Parent.
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    ::close(execPipe[1]);

    // Check execve error pipe (closes automatically on successful execve).
    int childErrno = 0;
    ssize_t en = 0;
    do {
        en = ::read(execPipe[0], &childErrno, sizeof(childErrno));
    } while (en < 0 && errno == EINTR);
    ::close(execPipe[0]);

    if (en == static_cast<ssize_t>(sizeof(childErrno)) && childErrno != 0) {
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        res.spawnError = std::string("spawn: exec failed for ") + argv[0] + ": " + std::strerror(childErrno);
        return res;
    }

    const bool hasTimeout = opts.timeoutMs > 0;
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(hasTimeout ? opts.timeoutMs : 0);

    int fds[2] = {outPipe[0], errPipe[0]};
    std::string* targets[2] = {&res.stdoutText, &res.stderrText};
    char buf[8192];

    while (fds[0] >= 0 || fds[1] >= 0) {
        struct pollfd pfds[2];
        int nfds = 0;
        int mapIdx[2] = {-1, -1};
        for (int i = 0; i < 2; ++i) {
            if (fds[i] >= 0) {
                pfds[nfds].fd = fds[i];
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                mapIdx[nfds] = i;
                ++nfds;
            }
        }

        int pollTimeout = -1;
        if (hasTimeout) {
            const auto now = SteadyClock::now();
            if (now >= deadline) {
                res.timedOut = true;
                break;
            }
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            pollTimeout = static_cast<int>(std::min<int64_t>(std::max<int64_t>(rem, 1), 60000));
        }

        const int pr = ::poll(pfds, static_cast<nfds_t>(nfds), pollTimeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (hasTimeout && SteadyClock::now() >= deadline) {
                res.timedOut = true;
                break;
            }
            continue;
        }
        for (int k = 0; k < nfds; ++k) {
            const int i = mapIdx[k];
            if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                const ssize_t n = ::read(fds[i], buf, sizeof(buf));
                if (n > 0) {
                    targets[i]->append(buf, static_cast<size_t>(n));
                } else if (n == 0 || (n < 0 && errno != EINTR)) {
                    ::close(fds[i]);
                    fds[i] = -1;
                }
            }
        }
    }

    if (fds[0] >= 0) ::close(fds[0]);
    if (fds[1] >= 0) ::close(fds[1]);

    if (res.timedOut) {
        (void)::kill(-pid, SIGKILL);
        (void)::kill(pid, SIGKILL);
    }

    int status = 0;
    if (hasTimeout && !res.timedOut) {
        // Pipes closed; wait for process exit within remaining timeout.
        for (;;) {
            const pid_t wr = ::waitpid(pid, &status, WNOHANG);
            if (wr == pid) break;
            if (wr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (SteadyClock::now() >= deadline) {
                res.timedOut = true;
                (void)::kill(-pid, SIGKILL);
                (void)::kill(pid, SIGKILL);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } else {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    if (res.timedOut) {
        res.exitCode = -1;
    } else if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.exitCode = -WTERMSIG(status);
    } else {
        res.exitCode = -1;
    }
    return res;
}

#endif

struct ProcManager {
    std::mutex mu;
    std::condition_variable cv;
    uint64_t nextId = 1;
    std::unordered_map<uint64_t, std::thread> threads;
    std::deque<uint64_t> completedOrder;
    std::unordered_map<uint64_t, ProcResult> completed;

    ~ProcManager() {
        std::vector<std::thread> toJoin;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& [_, t] : threads) {
                if (t.joinable()) toJoin.push_back(std::move(t));
            }
            threads.clear();
        }
        for (auto& t : toJoin) t.join();
    }
};

ProcManager& manager() {
    static ProcManager m;
    return m;
}

}  // namespace

uint64_t spawnProcess(const std::vector<std::string>& argv, const ProcOptions& opts) {
    auto& m = manager();
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(m.mu);
        id = m.nextId++;
        m.threads.emplace(id, std::thread([id, argv, opts]() {
            ProcResult res = runChildBlocking(id, argv, opts);
            auto& mgr = manager();
            {
                std::lock_guard<std::mutex> lock(mgr.mu);
                mgr.completed.emplace(id, std::move(res));
                mgr.completedOrder.push_back(id);
            }
            mgr.cv.notify_all();
        }));
    }
    return id;
}

ProcResult waitProcess(uint64_t id) {
    auto& m = manager();
    ProcResult res;
    std::thread t;
    {
        std::unique_lock<std::mutex> lk(m.mu);
        m.cv.wait(lk, [&]() {
            return m.completed.count(id) > 0 || m.threads.count(id) == 0;
        });
        auto it = m.completed.find(id);
        if (it == m.completed.end()) {
            res.id = id;
            res.spawnError = "wait: unknown process handle " + std::to_string(id);
            return res;
        }
        res = std::move(it->second);
        m.completed.erase(it);
        auto oit = std::find(m.completedOrder.begin(), m.completedOrder.end(), id);
        if (oit != m.completedOrder.end()) m.completedOrder.erase(oit);
        auto tit = m.threads.find(id);
        if (tit != m.threads.end()) {
            t = std::move(tit->second);
            m.threads.erase(tit);
        }
    }
    if (t.joinable()) t.join();
    return res;
}

std::optional<ProcResult> waitAnyProcess() {
    auto& m = manager();
    ProcResult res;
    std::thread t;
    {
        std::unique_lock<std::mutex> lk(m.mu);
        if (m.completedOrder.empty() && m.threads.empty()) return std::nullopt;
        m.cv.wait(lk, [&]() {
            return !m.completedOrder.empty() || m.threads.empty();
        });
        if (m.completedOrder.empty()) return std::nullopt;
        const uint64_t id = m.completedOrder.front();
        m.completedOrder.pop_front();
        auto it = m.completed.find(id);
        res = std::move(it->second);
        m.completed.erase(it);
        auto tit = m.threads.find(id);
        if (tit != m.threads.end()) {
            t = std::move(tit->second);
            m.threads.erase(tit);
        }
    }
    if (t.joinable()) t.join();
    return res;
}

ProcResult execProcess(const std::vector<std::string>& argv, const ProcOptions& opts) {
    return runChildBlocking(0, argv, opts);
}

}  // namespace bronze::host
