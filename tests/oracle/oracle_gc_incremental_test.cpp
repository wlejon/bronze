// The oracle cases under incremental marking (`oracle-gc-incremental`): each
// case built once, with inference, and its program run under
// BRASS_GC_STRESS=incremental, which must print the pinned bytes. The stress keeps a marking cycle open
// almost all the time: every fourth GC point starts one, the steps between
// are zero-budget mark slices and minor collections inside it, and the
// eighth slice finishes it, deciding the young generation in place (brass
// src/gc/heap_mark_incremental.cpp). That is the mode real programs collect
// in, and what it holds the runtime to is what the main suite's
// every-allocation stress (BRONZE_GC_STRESS=1, a minor or full collection
// with the mutator stopped) never exercises: write barriers that shade, the
// root remembered sets, and edges only a full trace follows (an object's
// prototype, through its shape's root) staying reachable when the mutator
// moves them during a cycle.
//
// The run is the only one: the main suite already holds the plain run of
// the same build to the pinned bytes.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "../test_temp_dir.h"
#include "cli/driver.h"
#include "oracle_cases.h"
#include "run_process.h"

namespace {

struct IncrementalCase {
    std::string expected;
    bool skip = false;  // no pinned bytes
    int buildStatus = -1;
    std::string buildErr;
    oracle::RunResult run;
};

}  // namespace

TEST_CASE("Oracle incremental-gc suite") {
    const std::filesystem::path casesDir = oracle::findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    const std::vector<oracle::OracleCase> cases = oracle::casesIn(casesDir);
    REQUIRE(!cases.empty());
    // Once for the whole test case: doctest enters it again for every
    // subcase below.
    static std::vector<IncrementalCase> results;
    static std::once_flag once;
    std::call_once(once, [&] {
        results.resize(cases.size());
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t i; (i = next.fetch_add(1)) < cases.size();) {
                const oracle::OracleCase& c = cases[i];
                IncrementalCase& res = results[i];
                std::filesystem::path expectedPath = c.entry;
                expectedPath.replace_extension(".expected");
                if (!oracle::readFileBytes(expectedPath, res.expected)) {
                    res.skip = true;
                    continue;
                }
                const std::filesystem::path exe = bronze_test::tempDir() / (c.id + "_oracle_inc.exe");
                std::error_code ec;
                oracle::removeProgram(exe, ec);
                res.buildStatus = bronze::cli::runBuild(c.entry.string(), exe.string(), &res.buildErr, true);
                if (res.buildStatus == 0) {
                    res.run = oracle::runCommand(oracle::quoted(exe.string()), oracle::GcStress::Incremental);
                }
                oracle::removeProgram(exe, ec);
            }
        };
        std::vector<std::thread> threads;
        for (unsigned int t = 0; t < oracle::getWorkerJobCount(); ++t) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    });

    for (size_t i = 0; i < cases.size(); ++i) {
        const IncrementalCase& res = results[i];
        // A case with no pinned bytes is the main suite's to report.
        if (res.skip) continue;
        SUBCASE(cases[i].id.c_str()) {
            REQUIRE_MESSAGE(res.buildStatus == 0, ("build failed: " + res.buildErr));
            CHECK_MESSAGE(!res.run.timedOut, "did not finish within the timeout (incremental gc)");
            if (res.run.ran) {
                CHECK_MESSAGE(res.run.output == res.expected,
                              ("differs from the pinned bytes under incremental gc\n--- exit code " +
                               std::to_string(res.run.exitCode) + ", stderr ---\n" + res.run.errors));
            }
        }
    }
}
