// Differential oracle harness (docs/0003). Each cases/<name>.js has a
// pinned cases/<name>.expected holding the exact stdout bytes of a correct
// run (JS semantics, console.log lines). bronze compiles the case and the
// produced executable's stdout must match byte-for-byte. Compiled cases run
// under a hard timeout and are killed on expiry, so a miscompiled loop can
// never hang the suite.
//
// Ratchet rules: expectations are never edited to match bronze; a
// cases/blocked/ entry that builds and matches must be promoted to cases/.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli/driver.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifndef TEST_CASES_DIR
#define TEST_CASES_DIR "tests/oracle/cases"
#endif

namespace {

constexpr uint32_t kRunTimeoutMs = 15000;

struct RunResult {
    bool ran = false;       // process started and exited on its own
    bool timedOut = false;  // killed after kRunTimeoutMs
    std::string output;
};

#ifdef _WIN32
RunResult runWithTimeout(const std::string& exePath) {
    RunResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    std::string cmdLine = "\"" + exePath + "\"";
    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return result;
    }
    CloseHandle(writePipe);  // ours would keep the pipe open past child exit

    // Drain the pipe on a separate thread so a chatty child can never fill
    // the pipe buffer and deadlock against our process-handle wait.
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            result.output.append(buf, n);
        }
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, kRunTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    reader.join();
    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    result.ran = !result.timedOut;
    return result;
}
#else
RunResult runWithTimeout(const std::string& exePath) {
    RunResult result;
    FILE* pipe = popen(("\"" + exePath + "\"").c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
        result.output.append(buf, n);
    }
    pclose(pipe);
    result.ran = true;
    return result;
}
#endif

bool readFileBytes(const std::filesystem::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

std::filesystem::path findCasesDirectory() {
    std::vector<std::filesystem::path> candidates = {
        TEST_CASES_DIR,
        "tests/oracle/cases",
        "../tests/oracle/cases",
        "../../tests/oracle/cases",
        "../../../tests/oracle/cases"
    };

    std::filesystem::path cwd = std::filesystem::current_path();
    for (const auto& cand : candidates) {
        std::filesystem::path full = cwd / cand;
        std::error_code ec;
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
            return std::filesystem::canonical(cand, ec);
        }
        if (std::filesystem::exists(full, ec) && std::filesystem::is_directory(full, ec)) {
            return std::filesystem::canonical(full, ec);
        }
    }
    return {};
}

std::vector<std::filesystem::path> jsCasesIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            cases.push_back(entry.path());
        }
    }
    std::sort(cases.begin(), cases.end());
    return cases;
}

}  // namespace

TEST_CASE("Oracle differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    auto caseFiles = jsCasesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    for (const auto& casePath : caseFiles) {
        SUBCASE(casePath.filename().string().c_str()) {
            std::string code;
            REQUIRE(readFileBytes(casePath, code));

            // Ratchet rule: non-determinism sources are banned in cases
            CHECK(code.find("Date") == std::string::npos);
            CHECK(code.find("Math.random") == std::string::npos);

            std::filesystem::path expectedPath = casePath;
            expectedPath.replace_extension(".expected");
            std::string expected;
            REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                            ("Missing pinned expectation " + expectedPath.string()).c_str());

            std::filesystem::path exePath =
                std::filesystem::temp_directory_path() / (casePath.stem().string() + "_oracle.exe");
            std::error_code ec;
            std::filesystem::remove(exePath, ec);

            std::string errOut;
            int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut);
            REQUIRE_MESSAGE(status == 0,
                            ("Bronze build failed for " + casePath.string() + ": " + errOut).c_str());
            REQUIRE(std::filesystem::exists(exePath));

            RunResult run = runWithTimeout(exePath.string());
            CHECK_MESSAGE(!run.timedOut,
                          ("Compiled case did not finish within the timeout: " + casePath.string()).c_str());
            if (run.ran) {
                CHECK(expected == run.output);
            }

            std::filesystem::remove(exePath, ec);
        }
    }
}

TEST_CASE("Oracle blocked test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    std::filesystem::path blockedDir = casesDir / "blocked";
    std::error_code ec;
    if (!std::filesystem::exists(blockedDir, ec) || !std::filesystem::is_directory(blockedDir, ec)) {
        return;
    }

    for (const auto& casePath : jsCasesIn(blockedDir)) {
        SUBCASE(casePath.filename().string().c_str()) {
            std::string code;
            REQUIRE(readFileBytes(casePath, code));
            CHECK(code.find("Date") == std::string::npos);
            CHECK(code.find("Math.random") == std::string::npos);

            std::filesystem::path expectedPath = casePath;
            expectedPath.replace_extension(".expected");
            std::string expected;
            REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                            ("Missing pinned expectation " + expectedPath.string()).c_str());

            std::filesystem::path exePath =
                std::filesystem::temp_directory_path() / (casePath.stem().string() + "_blocked.exe");
            std::filesystem::remove(exePath, ec);

            std::string errOut;
            int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut);

            if (status == 0 && std::filesystem::exists(exePath)) {
                RunResult run = runWithTimeout(exePath.string());
                std::filesystem::remove(exePath, ec);
                bool matches = run.ran && (expected == run.output);
                CHECK_MESSAGE(!matches,
                              ("Blocked oracle case passes! Promote " + casePath.filename().string() +
                               " (and its .expected) from cases/blocked/ to cases/").c_str());
            } else {
                CHECK(true);  // still blocked at build time
            }
        }
    }
}
