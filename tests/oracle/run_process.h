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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace oracle {

constexpr uint32_t kRunTimeoutMs = 15000;

struct RunResult {
    bool ran = false;       // process started and exited on its own
    bool timedOut = false;  // killed after the timeout
    int exitCode = -1;      // 0 on clean exit; 128+signal where the OS says so
    std::string output;     // stdout, byte for byte
    std::string errors;     // stderr, byte for byte
};

inline std::string quoted(const std::string& arg) { return "\"" + arg + "\""; }

#ifdef _WIN32
inline RunResult runCommand(const std::string& cmdLine, bool gcStress = false,
                            uint32_t timeoutMs = kRunTimeoutMs) {
    RunResult result;

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

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
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
inline RunResult runCommand(const std::string& cmdLine, bool gcStress = false,
                            uint32_t timeoutMs = kRunTimeoutMs) {
    (void)timeoutMs;
    RunResult result;
    static std::atomic<unsigned> s_serial{0};
    const std::filesystem::path errFile =
        std::filesystem::temp_directory_path() /
        ("oracle_stderr_" + std::to_string(::getpid()) + "_" + std::to_string(s_serial++) + ".txt");
    std::string cmd = (gcStress ? "BRONZE_GC_STRESS=1 " : "") + cmdLine + " 2>" + quoted(errFile.string());
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
        result.output.append(buf, n);
    }
    int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    }
    {
        std::ifstream in(errFile, std::ios::binary);
        result.errors.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    std::filesystem::remove(errFile, ec);
    result.ran = true;
    return result;
}
#endif

}  // namespace oracle
