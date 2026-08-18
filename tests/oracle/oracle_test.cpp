// Differential oracle harness. Each cases/<name>.js has a pinned
// cases/<name>.expected holding the exact stdout bytes of a correct run (JS
// semantics, console.log lines). bronze compiles the case and the produced
// executable's stdout must match byte-for-byte. Compiled cases run under a hard
// timeout and are killed on expiry, so a miscompiled loop can never hang the
// suite.
//
// A case that needs SEVERAL files is a directory: `cases/<name>/main.js` is the
// entry, its neighbours are what it imports, and the expectation is
// `cases/<name>/main.expected` — the same "entry path with the extension
// replaced" rule, one level deeper. Everything below this point treats the two
// kinds identically.
//
// Ratchet rules: expectations are never edited to match bronze; a
// cases/blocked/ entry that builds and matches must be promoted to cases/; and
// every case is compiled and run BOTH with inference and with `--no-infer`,
// both of which must produce the pinned bytes.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "cli/driver.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif


#ifndef TEST_CASES_DIR
#define TEST_CASES_DIR "tests/oracle/cases"
#endif

#ifndef TEST_THREEJS_DIR
#define TEST_THREEJS_DIR "tests/oracle/threejs"
#endif

#ifndef TEST_PIXI_DIR
#define TEST_PIXI_DIR "tests/oracle/pixi"
#endif

namespace {

constexpr uint32_t kRunTimeoutMs = 15000;

struct RunResult {
    bool ran = false;       // process started and exited on its own
    bool timedOut = false;  // killed after kRunTimeoutMs
    int exitCode = -1;      // 0 on clean exit; 128+signal where the OS says so
    std::string output;
};

#ifdef _WIN32
RunResult runWithTimeout(const std::string& exePath, bool gcStress = false,
                         uint32_t timeoutMs = kRunTimeoutMs) {
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

    static std::mutex s_spawnMutex;
    {
        std::lock_guard<std::mutex> lock(s_spawnMutex);
        if (gcStress) {
            _putenv_s("BRONZE_GC_STRESS", "1");
        } else {
            _putenv_s("BRONZE_GC_STRESS", "");
        }
        BOOL ok = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                 nullptr, &si, &pi);
        _putenv_s("BRONZE_GC_STRESS", "");
        if (!ok) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return result;
        }
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

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    reader.join();
    CloseHandle(readPipe);
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
RunResult runWithTimeout(const std::string& exePath, bool gcStress = false,
                         uint32_t timeoutMs = kRunTimeoutMs) {
    (void)timeoutMs;
    RunResult result;
    std::string cmd = (gcStress ? "BRONZE_GC_STRESS=1 " : "") + ("\"" + exePath + "\"");
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

std::filesystem::path findTestDirectory(const std::filesystem::path& baked,
                                        const std::string& suffix) {
    std::vector<std::filesystem::path> candidates = {
        baked,
        suffix,
        "../" + suffix,
        "../../" + suffix,
        "../../../" + suffix
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

std::filesystem::path findCasesDirectory() {
    return findTestDirectory(TEST_CASES_DIR, "tests/oracle/cases");
}

struct OracleCase {
    std::filesystem::path entry;
    std::string id;
};

std::vector<OracleCase> casesIn(const std::filesystem::path& dir) {
    std::vector<OracleCase> cases;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".js") {
            cases.push_back({entry.path(), entry.path().stem().string()});
            continue;
        }
        if (!entry.is_directory()) continue;
        if (entry.path().filename() == "blocked") continue;
        std::filesystem::path main = entry.path() / "main.js";
        std::error_code ec;
        if (!std::filesystem::exists(main, ec)) continue;
        cases.push_back({main, entry.path().filename().string()});
    }
    std::sort(cases.begin(), cases.end(),
              [](const OracleCase& a, const OracleCase& b) { return a.entry < b.entry; });
    return cases;
}

unsigned int getWorkerJobCount() {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "BRONZE_TEST_JOBS") == 0 && env != nullptr) {
        int n = std::atoi(env);
        std::free(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#else
    if (const char* env = std::getenv("BRONZE_TEST_JOBS")) {
        int n = std::atoi(env);
        if (n > 0) return static_cast<unsigned int>(n);
    }
#endif
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    // Bounded concurrency: at most 4 threads by default (or hw / 2) to prevent machine saturation
    return std::max(1u, std::min(4u, hw / 2));
}

// Does this case READ THE CLOCK? That is the thing a pinned expectation cannot
// survive, and it is narrower than "mentions Date": `Date.UTC`, `Date.parse`,
// the field constructor and every one of 21.4.4's members are pure functions of
// their arguments, and pinning them is exactly how the Date implementation is
// held to ECMA-262.
//
// The three clock reads are `Date.now`, `new Date()` and `Date()` — the two
// no-argument constructor forms. A whitespace-tolerant match on the empty
// argument list is what separates them from `new Date(0)`.
//
// The occurrence must be the IDENTIFIER `Date` and not a name that merely ends
// in it: `d.getUTCDate()` is four characters of "Date" followed by an empty
// argument list, and reading it as the constructor would ban the very getters
// these cases exist to pin.
//
// (Local-TIMEZONE dependence is a second hazard this cannot see: a case may
// call `getHours` only inside a relation that holds in every zone. That is a
// rule for the author, stated in tests/oracle/README.md.)
bool standsAlone(const std::string& code, size_t at) {
    if (at == 0) return true;
    const char before = code[at - 1];
    // '.' rejects a member access, so a `getUTCDate`/`setDate` suffix and a
    // user's own `foo.Date` are both something other than the global.
    return !(std::isalnum(static_cast<unsigned char>(before)) || before == '_' || before == '$' ||
             before == '.');
}

bool readsTheClock(const std::string& code) {
    size_t now = 0;
    while ((now = code.find("Date.now", now)) != std::string::npos) {
        if (standsAlone(code, now)) return true;
        now += 4;
    }
    size_t at = 0;
    while ((at = code.find("Date", at)) != std::string::npos) {
        if (!standsAlone(code, at)) {
            at += 4;
            continue;
        }
        size_t after = at + 4;
        while (after < code.size() && (code[after] == ' ' || code[after] == '\t')) ++after;
        if (after < code.size() && code[after] == '(') {
            size_t inner = after + 1;
            while (inner < code.size() &&
                   (code[inner] == ' ' || code[inner] == '\t' || code[inner] == '\n' ||
                    code[inner] == '\r')) {
                ++inner;
            }
            if (inner < code.size() && code[inner] == ')') return true;
        }
        at = after;
    }
    return false;
}

struct CaseExecutionResult {
    OracleCase oracleCase;
    bool codeReadOk = false;
    bool hasNonDeterminism = false;
    bool expectedReadOk = false;
    std::string expectedPathStr;
    std::string expected;

    // Inference mode (normal + gc stress on the same built binary)
    int buildInferStatus = -1;
    std::string buildInferErr;
    bool inferExeExists = false;
    RunResult runInfer;
    RunResult runInferGc;

    // No-inference mode
    int buildNoInferStatus = -1;
    std::string buildNoInferErr;
    bool noInferExeExists = false;
    RunResult runNoInfer;
    RunResult runNoInferGc;
};

}  // namespace

TEST_CASE("Oracle differential test suite") {
    std::filesystem::path casesDir = findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");

    auto caseFiles = casesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    static std::vector<CaseExecutionResult> results;
    static std::once_flag resultsOnce;

    std::call_once(resultsOnce, [&] {
        results.resize(caseFiles.size());
        const unsigned int numJobs = getWorkerJobCount();
        std::atomic<size_t> nextCaseIdx{0};

        auto worker = [&] {
            while (true) {
                size_t idx = nextCaseIdx.fetch_add(1, std::memory_order_relaxed);
                if (idx >= caseFiles.size()) break;

                const auto& oracleCase = caseFiles[idx];
                CaseExecutionResult& res = results[idx];
                res.oracleCase = oracleCase;

                std::string code;
                res.codeReadOk = readFileBytes(oracleCase.entry, code);
                if (res.codeReadOk) {
                    res.hasNonDeterminism =
                        readsTheClock(code) || code.find("Math.random") != std::string::npos;
                }

                std::filesystem::path expectedPath = oracleCase.entry;
                expectedPath.replace_extension(".expected");
                res.expectedPathStr = expectedPath.string();
                res.expectedReadOk = readFileBytes(expectedPath, res.expected);

                if (!res.codeReadOk || !res.expectedReadOk) continue;

                // 1. Compile with inference on
                std::filesystem::path exeInfer =
                    std::filesystem::temp_directory_path() / (oracleCase.id + "_oracle.exe");
                std::error_code ec;
                std::filesystem::remove(exeInfer, ec);

                res.buildInferStatus = bronze::cli::runBuild(oracleCase.entry.string(),
                                                             exeInfer.string(), &res.buildInferErr, true);
                res.inferExeExists = std::filesystem::exists(exeInfer);

                if (res.buildInferStatus == 0 && res.inferExeExists) {
                    res.runInfer = runWithTimeout(exeInfer.string(), /*gcStress=*/false);
                    // Same compiled binary re-run under GC stress to verify rooting without duplicate builds
                    res.runInferGc = runWithTimeout(exeInfer.string(), /*gcStress=*/true);
                }
                std::filesystem::remove(exeInfer, ec);

                // 2. Compile with --no-infer
                std::filesystem::path exeNoInfer =
                    std::filesystem::temp_directory_path() / (oracleCase.id + "_oracle_noinfer.exe");
                std::filesystem::remove(exeNoInfer, ec);

                res.buildNoInferStatus = bronze::cli::runBuild(
                    oracleCase.entry.string(), exeNoInfer.string(), &res.buildNoInferErr, false);
                res.noInferExeExists = std::filesystem::exists(exeNoInfer);

                if (res.buildNoInferStatus == 0 && res.noInferExeExists) {
                    res.runNoInfer = runWithTimeout(exeNoInfer.string(), /*gcStress=*/false);
                    // Same compiled binary re-run under GC stress to verify rooting without duplicate builds
                    res.runNoInferGc = runWithTimeout(exeNoInfer.string(), /*gcStress=*/true);
                }
                std::filesystem::remove(exeNoInfer, ec);
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(numJobs);
        for (unsigned int i = 0; i < numJobs; ++i) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }
    });

    // Report results sequentially to doctest
    for (const auto& res : results) {
        SUBCASE(res.oracleCase.id.c_str()) {
            REQUIRE(res.codeReadOk);
            CHECK_MESSAGE(!res.hasNonDeterminism,
                          "Banned non-determinism (a clock read or Math.random) in this case");
            REQUIRE_MESSAGE(res.expectedReadOk,
                            ("Missing pinned expectation " + res.expectedPathStr).c_str());

            // Inference on
            REQUIRE_MESSAGE(res.buildInferStatus == 0,
                            ("Bronze build failed for " + res.oracleCase.entry.string() +
                             " (inference on): " + res.buildInferErr).c_str());
            REQUIRE(res.inferExeExists);
            CHECK_MESSAGE(!res.runInfer.timedOut,
                          ("Compiled case did not finish within the timeout: " +
                           res.oracleCase.entry.string() + " (inference on)").c_str());
            if (res.runInfer.ran) {
                CHECK_MESSAGE(res.expected == res.runInfer.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (inference on)").c_str());
            }

            // GC stress (inference on)
            CHECK_MESSAGE(!res.runInferGc.timedOut,
                          ("Compiled case did not finish within the timeout (gc-stress): " +
                           res.oracleCase.entry.string()).c_str());
            if (res.runInferGc.ran) {
                CHECK_MESSAGE(res.expected == res.runInferGc.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (gc-stress)").c_str());
            }

            // --no-infer
            REQUIRE_MESSAGE(res.buildNoInferStatus == 0,
                            ("Bronze build failed for " + res.oracleCase.entry.string() +
                             " (--no-infer): " + res.buildNoInferErr).c_str());
            REQUIRE(res.noInferExeExists);
            CHECK_MESSAGE(!res.runNoInfer.timedOut,
                          ("Compiled case did not finish within the timeout: " +
                           res.oracleCase.entry.string() + " (--no-infer)").c_str());
            if (res.runNoInfer.ran) {
                CHECK_MESSAGE(res.expected == res.runNoInfer.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (--no-infer)").c_str());
            }

            // GC stress (--no-infer)
            CHECK_MESSAGE(!res.runNoInferGc.timedOut,
                          ("Compiled case did not finish within the timeout (gc-stress, --no-infer): " +
                           res.oracleCase.entry.string()).c_str());
            if (res.runNoInferGc.ran) {
                CHECK_MESSAGE(res.expected == res.runNoInferGc.output,
                              ("Output differs from the pinned expectation for " +
                               res.oracleCase.id + " (gc-stress, --no-infer)").c_str());
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
            CHECK(!readsTheClock(code));
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

TEST_CASE("threejs milestone: unmodified r160 compiles and its scene graph holds") {
    std::filesystem::path dir = findTestDirectory(TEST_THREEJS_DIR, "tests/oracle/threejs");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/threejs not found");

    std::filesystem::path casePath = dir / "main.js";
    REQUIRE(std::filesystem::exists(casePath));

    std::string expected;
    std::filesystem::path expectedPath = dir / "main.expected";
    REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                    ("Missing pinned expectation " + expectedPath.string()).c_str());

    for (const bool infer : {true, false}) {
        const std::string mode = infer ? " (inference on)" : " (--no-infer)";
        std::filesystem::path exePath = std::filesystem::temp_directory_path() /
                                        (infer ? "threejs_oracle.exe" : "threejs_oracle_ni.exe");
        std::error_code ec;
        std::filesystem::remove(exePath, ec);

        std::string errOut;
        int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut, infer);
        REQUIRE_MESSAGE(status == 0,
                        ("Bronze failed to build three.js" + mode + ": " + errOut).c_str());
        REQUIRE(std::filesystem::exists(exePath));

        RunResult run = runWithTimeout(exePath.string(), /*gcStress=*/false);
        CHECK_MESSAGE(!run.timedOut, ("three.js case did not finish within the timeout" + mode).c_str());
        if (run.ran) {
            CHECK_MESSAGE(run.exitCode == 0,
                          ("three.js exited with code " + std::to_string(run.exitCode) + mode).c_str());
            CHECK_MESSAGE(expected == run.output,
                          ("three.js output differs from the pinned expectation" + mode).c_str());
        }

        // Same executable, every allocation now moving the whole live set.
        RunResult stressed = runWithTimeout(exePath.string(), /*gcStress=*/true);
        CHECK_MESSAGE(!stressed.timedOut,
                      ("three.js case did not finish within the timeout (gc-stress" + mode + ")").c_str());
        if (stressed.ran) {
            CHECK_MESSAGE(stressed.exitCode == 0,
                          ("three.js exited with code " + std::to_string(stressed.exitCode) +
                           " (gc-stress" + mode + ")").c_str());
            CHECK_MESSAGE(expected == stressed.output,
                          ("three.js output differs from the pinned expectation (gc-stress" + mode + ")").c_str());
        }
        std::filesystem::remove(exePath, ec);
    }
}

// The pixi milestone: the published pixi.js v8 ESM bundle, vendored
// byte-for-byte (tests/oracle/pixi/README.md), compiles and a scene graph
// built from its public API matches an expectation derived by reading pixi's
// source — never by running bronze or node. Same shape as the three.js
// milestone above, plus one seam that milestone does not need: pixi's
// import-time code reads two browser globals (`navigator`, `Intl`), which
// setup.mjs defines on `globalThis` and host.globals admits at compile time.
TEST_CASE("pixi milestone: unmodified v8.19.0 compiles and its scene graph holds") {
    std::filesystem::path dir = findTestDirectory(TEST_PIXI_DIR, "tests/oracle/pixi");
    REQUIRE_MESSAGE(!dir.empty(), "tests/oracle/pixi not found");

    std::filesystem::path casePath = dir / "main.js";
    REQUIRE(std::filesystem::exists(casePath));

    std::string expected;
    std::filesystem::path expectedPath = dir / "main.expected";
    REQUIRE_MESSAGE(readFileBytes(expectedPath, expected),
                    ("Missing pinned expectation " + expectedPath.string()).c_str());

    const std::string hostGlobals = (dir / "host.globals").string();
    REQUIRE(std::filesystem::exists(hostGlobals));

    for (const bool infer : {true, false}) {
        const std::string mode = infer ? " (inference on)" : " (--no-infer)";
        std::filesystem::path exePath = std::filesystem::temp_directory_path() /
                                        (infer ? "pixi_oracle.exe" : "pixi_oracle_ni.exe");
        std::error_code ec;
        std::filesystem::remove(exePath, ec);

        std::string errOut;
        int status = bronze::cli::runBuild(casePath.string(), exePath.string(), &errOut, infer,
                                           /*timings=*/false, /*emitObj=*/false, hostGlobals);
        REQUIRE_MESSAGE(status == 0,
                        ("Bronze failed to build pixi" + mode + ": " + errOut).c_str());
        REQUIRE(std::filesystem::exists(exePath));

        RunResult run = runWithTimeout(exePath.string(), /*gcStress=*/false);
        CHECK_MESSAGE(!run.timedOut, ("pixi case did not finish within the timeout" + mode).c_str());
        if (run.ran) {
            CHECK_MESSAGE(run.exitCode == 0,
                          ("pixi exited with code " + std::to_string(run.exitCode) + mode).c_str());
            CHECK_MESSAGE(expected == run.output,
                          ("pixi output differs from the pinned expectation" + mode).c_str());
        }

        // Same executable, every allocation now moving the whole live set. A
        // whole-library import under that regime measures in minutes, not the
        // 15 s a case gets — the budget is sized to the run, not the run
        // trimmed to the budget (the byte-compare is unchanged either way).
        RunResult stressed =
            runWithTimeout(exePath.string(), /*gcStress=*/true, /*timeoutMs=*/300000);
        CHECK_MESSAGE(!stressed.timedOut,
                      ("pixi case did not finish within the timeout (gc-stress" + mode + ")").c_str());
        if (stressed.ran) {
            CHECK_MESSAGE(stressed.exitCode == 0,
                          ("pixi exited with code " + std::to_string(stressed.exitCode) +
                           " (gc-stress" + mode + ")").c_str());
            CHECK_MESSAGE(expected == stressed.output,
                          ("pixi output differs from the pinned expectation (gc-stress" + mode + ")").c_str());
        }
        std::filesystem::remove(exePath, ec);
    }
}
