// The module suite (`oracle-module`): every case built to an ahead-of-time
// module through the evaluator, loaded and run (tests/aot_module/run_host.cpp),
// plain and under gc-stress, each run held to the pinned bytes.
//
// A module is what a host installs for an app's scripts (eval.h,
// "Ahead-of-time modules"). Its compile is the evaluator's — script semantics,
// the eval transform, the host's open world — with the whole-program optimizer
// on, a pairing no other suite runs: `bronze build` has the optimizer without
// the evaluator's front end, `bronze run` the front end without the optimizer.
//
// The runner links the shared runtime, which not every configuration builds;
// ctest names it in BRONZE_MODULE_RUN_HOST (tests/aot_module/CMakeLists.txt),
// and without it the suite has nothing to run.

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "oracle_cases.h"
#include "run_process.h"

namespace {

using oracle::RunResult;

struct ModuleCaseResult {
    oracle::OracleCase oracleCase;
    bool expectedReadOk = false;
    std::string expected;
    RunResult run;
    RunResult runGc;
};

std::string runnerPath() {
#ifdef _WIN32
    char* env = nullptr;
    size_t len = 0;
    std::string path;
    if (_dupenv_s(&env, &len, "BRONZE_MODULE_RUN_HOST") == 0 && env != nullptr) path = env;
    free(env);
    return path;
#else
    const char* env = std::getenv("BRONZE_MODULE_RUN_HOST");
    return env ? env : "";
#endif
}

std::string summary(const RunResult& r) {
    return "\n--- exit " + std::to_string(r.exitCode) + ", stdout ---\n" + r.output + "\n--- stderr ---\n" + r.errors;
}

}  // namespace

TEST_CASE("Oracle module test suite") {
    const std::string runner = runnerPath();
    if (runner.empty()) {
        MESSAGE("BRONZE_MODULE_RUN_HOST is not set: no module runner in this build");
        return;
    }
    REQUIRE_MESSAGE(std::filesystem::exists(runner), ("module runner not found at " + runner).c_str());
    std::filesystem::path casesDir = oracle::findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    auto caseFiles = oracle::casesIn(casesDir);
    REQUIRE_MESSAGE(!caseFiles.empty(), "No .js test cases found in cases directory");

    static std::vector<ModuleCaseResult> results;
    static std::once_flag resultsOnce;
    std::call_once(resultsOnce, [&] {
        results.resize(caseFiles.size());
        std::atomic<size_t> next{0};
        auto worker = [&] {
            while (true) {
                const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= caseFiles.size()) break;
                ModuleCaseResult& res = results[idx];
                res.oracleCase = caseFiles[idx];
                std::filesystem::path expectedPath = res.oracleCase.entry;
                expectedPath.replace_extension(".expected");
                res.expectedReadOk = oracle::readFileBytes(expectedPath, res.expected);
                if (!res.expectedReadOk) continue;
                const std::string cmd = oracle::quoted(runner) + " " + oracle::quoted(res.oracleCase.entry.string());
                res.run = oracle::runCommand(cmd, /*gcStress=*/false);
                res.runGc = oracle::runCommand(cmd, /*gcStress=*/true);
            }
        };
        std::vector<std::thread> threads;
        const unsigned int jobs = oracle::getWorkerJobCount();
        threads.reserve(jobs);
        for (unsigned int i = 0; i < jobs; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    });

    for (const auto& res : results) {
        SUBCASE(res.oracleCase.id.c_str()) {
            REQUIRE_MESSAGE(res.expectedReadOk, ("Missing pinned expectation for " + res.oracleCase.id).c_str());
            CHECK_MESSAGE(!res.run.timedOut, ("Module run did not finish within the timeout: " + res.oracleCase.id).c_str());
            if (res.run.ran) {
                CHECK_MESSAGE(res.expected == res.run.output,
                              ("Module output differs from the pinned expectation for " + res.oracleCase.id +
                               summary(res.run)).c_str());
            }
            CHECK_MESSAGE(!res.runGc.timedOut,
                          ("Module run did not finish within the timeout (gc-stress): " + res.oracleCase.id).c_str());
            if (res.runGc.ran) {
                CHECK_MESSAGE(res.expected == res.runGc.output,
                              ("Module output differs from the pinned expectation for " + res.oracleCase.id +
                               " (gc-stress)" + summary(res.runGc)).c_str());
            }
        }
    }
}
