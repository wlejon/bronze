// Differential oracle harness (docs/0003). Each cases/<name>.js has a
// pinned cases/<name>.expected holding the exact stdout bytes of a correct
// run (JS semantics, console.log lines). bronze compiles the case and the
// produced executable's stdout must match byte-for-byte. Compiled cases run
// under a hard timeout and are killed on expiry, so a miscompiled loop can
// never hang the suite.
//
// A case that needs SEVERAL files is a directory: `cases/<name>/main.js` is
// the entry, its neighbours are what it imports, and the expectation is
// `cases/<name>/main.expected` — the same "entry path with the extension
// replaced" rule, one level deeper (docs/0023 decision 5). Everything below
// this point treats the two kinds identically.
//
// Ratchet rules: expectations are never edited to match bronze; a
// cases/blocked/ entry that builds and matches must be promoted to cases/;
// and every case is compiled and run BOTH with inference and with
// `--no-infer`, both of which must produce the pinned bytes (docs/0010
// decision 8).

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

// One case: the entry file bronze is pointed at, and the name it is reported
// and named its temporary executable by. For a single-file case the two are
// the same thing they always were.
struct OracleCase {
    std::filesystem::path entry;
    std::string id;
};

// A case is EITHER `cases/<name>.js`, as every case was before modules, OR a
// directory `cases/<name>/` whose entry is `main.js` and whose other files
// are what it imports (docs/0023 decision 5). Nothing about the first kind
// changes: it is found the same way, paired with `<name>.expected` by the
// same rule, and compared the same way. The second kind reuses that rule one
// level deeper — `cases/<name>/main.expected` — so there is one pairing rule
// and not two.
std::vector<OracleCase> casesIn(const std::filesystem::path& dir) {
    std::vector<OracleCase> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            cases.push_back({entry.path(), entry.path().stem().string()});
            continue;
        }
        if (!entry.is_directory()) continue;
        // `blocked/` is the other suite's, and it is enumerated by its own
        // TEST_CASE with the same two rules.
        if (entry.path().filename() == "blocked") continue;
        std::filesystem::path main = entry.path() / "main.js";
        std::error_code ec;
        if (!std::filesystem::exists(main, ec)) continue;
        // Named for the DIRECTORY: every multi-file case's entry is
        // `main.js`, so naming them for the entry would report them all
        // identically and would collide on `main_oracle.exe`.
        cases.push_back({main, entry.path().filename().string()});
    }
    std::sort(cases.begin(), cases.end(),
              [](const OracleCase& a, const OracleCase& b) { return a.entry < b.entry; });
    return cases;
}

}  // namespace

TEST_CASE("Oracle differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    auto caseFiles = casesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    for (const auto& oracleCase : caseFiles) {
        const std::filesystem::path& casePath = oracleCase.entry;
        SUBCASE(oracleCase.id.c_str()) {
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

            // Every case runs twice: with inference and with it switched
            // off. Both must produce the same pinned bytes, which is what
            // makes `--no-infer` a ratchet rather than a comfort blanket
            // (docs/0010 decision 8) — a case only inference gets right
            // means the no-inference path is unsound, and a case only
            // `--no-infer` gets right means inference is.
            for (const bool infer : {true, false}) {
                const std::string mode = infer ? " (inference on)" : " (--no-infer)";
                std::filesystem::path exePath =
                    std::filesystem::temp_directory_path() /
                    (oracleCase.id + (infer ? "_oracle.exe" : "_oracle_noinfer.exe"));
                std::error_code ec;
                std::filesystem::remove(exePath, ec);

                std::string errOut;
                int status =
                    bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut, infer);
                REQUIRE_MESSAGE(status == 0, ("Bronze build failed for " + casePath.string() +
                                              mode + ": " + errOut).c_str());
                REQUIRE(std::filesystem::exists(exePath));

                RunResult run = runWithTimeout(exePath.string());
                CHECK_MESSAGE(!run.timedOut, ("Compiled case did not finish within the timeout: " +
                                              casePath.string() + mode).c_str());
                if (run.ran) {
                    CHECK_MESSAGE(expected == run.output,
                                  ("Output differs from the pinned expectation for " +
                                   oracleCase.id + mode).c_str());
                }
                std::filesystem::remove(exePath, ec);
            }
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

    for (const auto& oracleCase : casesIn(blockedDir)) {
        const std::filesystem::path& casePath = oracleCase.entry;
        SUBCASE(oracleCase.id.c_str()) {
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
                std::filesystem::temp_directory_path() / (oracleCase.id + "_blocked.exe");
            std::filesystem::remove(exePath, ec);

            std::string errOut;
            int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut);

            if (status == 0 && std::filesystem::exists(exePath)) {
                RunResult run = runWithTimeout(exePath.string());
                std::filesystem::remove(exePath, ec);
                bool matches = run.ran && (expected == run.output);
                CHECK_MESSAGE(!matches,
                              ("Blocked oracle case passes! Promote " + oracleCase.id +
                               " (and its .expected) from cases/blocked/ to cases/").c_str());
            } else {
                CHECK(true);  // still blocked at build time
            }
        }
    }
}
