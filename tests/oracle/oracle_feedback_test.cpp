// The type-feedback suite (`oracle-feedback`, docs/type-feedback.md): a
// profile changes how a build lays out and guards its sites, never what the
// program prints. Each case is
//
//   1. built with `--feedback-out` and run, which must print the pinned bytes
//      and writes the case's own profile;
//   2. built with `--feedback-in` on that profile and run;
//   3. built with `--feedback-in` on an ADVERSARIAL profile made from it, in
//      which every value site saw only Numbers, every property site one shape
//      whose keys are rotated (so the slot it suggests for a key is wrong),
//      and every call site one callee, another site's where the program has
//      two — each with enough executions that every claim and hint is taken —
//      and run.
//
// Steps 2 and 3 must print the pinned bytes and exit as the case does: a
// wrong profile may only cost speed. A case whose instrumented run writes no
// profile (it leaves through `_exit`, or dies) is checked at step 1 only.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "../test_temp_dir.h"
#include "cli/driver.h"
#include "opt/spec/profile.h"
#include "oracle_cases.h"
#include "run_process.h"

namespace {

using oracle::RunResult;

// `bronze build` with inference, writing an instrumented build when `out` is
// set and reading a profile when `in` is.
int buildCase(const std::filesystem::path& entry, const std::filesystem::path& exe, std::string* err,
              const std::string& out, const std::string& in) {
    return bronze::cli::runBuild(entry.string(), exe.string(), err, true, false, false, {}, false, nullptr, {}, {},
                                 false, true, {}, false, {}, {}, false, {}, {}, {}, {}, false, false, {}, false, {},
                                 false, out, in);
}

RunResult runBuilt(const std::filesystem::path& exe) {
    return oracle::runCommand(oracle::quoted(exe.string()), /*gcStress=*/false, oracle::kRunTimeoutMs);
}

// Enough executions that every threshold of feedback_plan.h is met.
constexpr uint64_t kAdversarialCount = 1000;

// `P{a,b,c}` -> `P{c,a,b}`: the same keys, every one at another slot. A
// shape of one key gains a key in front of it; a receiver named by kind
// becomes a plain object's.
std::string rotatedShape(const std::string& name) {
    const size_t open = name.find('{');
    if (open == std::string::npos || name.back() != '}') return "Object{x,y}";
    const std::string body = name.substr(open + 1, name.size() - open - 2);
    if (body.empty() || body == "dict") return name.substr(0, open) + "{x,y}";
    const size_t comma = body.rfind(',');
    if (comma == std::string::npos) return name.substr(0, open + 1) + "__adversarial," + body + "}";
    return name.substr(0, open + 1) + body.substr(comma + 1) + "," + body.substr(0, comma) + "}";
}

bronze::spec::TypeProfile adversarial(const bronze::spec::TypeProfile& real) {
    std::set<std::string> calleeSet;
    for (const auto& [key, fn] : real.functions()) {
        for (const auto& [ord, site] : fn.sites) {
            for (const auto& [name, n] : site.targets) {
                if (name != "*other*") calleeSet.insert(name);
            }
        }
    }
    const std::vector<std::string> callees(calleeSet.begin(), calleeSet.end());
    // Another site's callee: the next name after this site's own.
    auto otherCallee = [&](const std::string& own) {
        if (callees.empty()) return std::string("nothing@00000000");
        auto it = std::upper_bound(callees.begin(), callees.end(), own);
        return it == callees.end() ? callees.front() : *it;
    };
    bronze::spec::TypeProfile out;
    for (const auto& [key, fn] : real.functions()) {
        bronze::spec::FunctionFeedback copy = fn;
        for (auto& [ord, site] : copy.sites) {
            if (site.kind == bronze::il::FeedbackKind::Entry) continue;
            const std::string shape = site.shapes.empty() ? std::string() : site.shapes.front().first;
            const std::string callee = site.targets.empty() ? std::string() : site.targets.front().first;
            site.total = kAdversarialCount;
            site.tags = {};
            site.tags[BRONZE_FEEDBACK_TAG_F64] = kAdversarialCount;
            if (site.kind == bronze::il::FeedbackKind::Shape || site.kind == bronze::il::FeedbackKind::Method) {
                site.shapes = {{rotatedShape(shape), kAdversarialCount}};
            }
            if (site.kind == bronze::il::FeedbackKind::Target || site.kind == bronze::il::FeedbackKind::Method) {
                site.targets = {{otherCallee(callee), kAdversarialCount}};
            }
        }
        out.put(std::move(copy));
    }
    return out;
}

struct FeedbackCase {
    std::string expected;
    bool skip = false;  // no pinned bytes, or the case reads the clock or draws numbers
    std::string buildErr;
    int buildStatus = -1;
    RunResult instrumented;
    bool profiled = false;
    std::string ownErr, advErr;
    int ownStatus = -1, advStatus = -1;
    RunResult own, adv;
};

}  // namespace

TEST_CASE("Oracle feedback suite") {
    const std::filesystem::path casesDir = oracle::findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    // BRONZE_ORACLE_FEEDBACK_CASES=<id substring>[,...] runs the cases whose id
    // contains one of them: the suite is three builds a case.
    std::vector<oracle::OracleCase> cases;
    const char* only = std::getenv("BRONZE_ORACLE_FEEDBACK_CASES");
    for (auto& c : oracle::casesIn(casesDir)) {
        bool keep = only == nullptr;
        for (std::string_view rest = only ? only : ""; !keep && !rest.empty();) {
            const size_t comma = std::min(rest.find(','), rest.size());
            if (comma != 0 && c.id.find(rest.substr(0, comma)) != std::string::npos) keep = true;
            rest = rest.substr(std::min(comma + 1, rest.size()));
        }
        if (keep) cases.push_back(std::move(c));
    }
    REQUIRE(!cases.empty());
    // Once for the whole test case: doctest enters it again for every
    // subcase below.
    static std::vector<FeedbackCase> results;
    static std::once_flag once;
    std::call_once(once, [&] {
        results.resize(cases.size());
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t i; (i = next.fetch_add(1)) < cases.size();) {
                const oracle::OracleCase& c = cases[i];
                FeedbackCase& res = results[i];
                std::string code;
                std::filesystem::path expectedPath = c.entry;
                expectedPath.replace_extension(".expected");
                if (!oracle::readFileBytes(c.entry, code) || !oracle::readFileBytes(expectedPath, res.expected) ||
                    oracle::readsTheClock(code) || code.find("Math.random") != std::string::npos) {
                    res.skip = true;
                    continue;
                }
                const std::filesystem::path dir = bronze_test::tempDir();
                const std::filesystem::path profile = dir / (c.id + "_oracle.tfb");
                const std::filesystem::path advProfile = dir / (c.id + "_oracle_adv.tfb");
                const std::filesystem::path exe = dir / (c.id + "_oracle_fb.exe");
                std::error_code ec;
                std::filesystem::remove(profile, ec);
                oracle::removeProgram(exe, ec);
                res.buildStatus = buildCase(c.entry, exe, &res.buildErr, profile.string(), {});
                if (res.buildStatus != 0) continue;
                res.instrumented = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                std::string err;
                const auto real = bronze::spec::TypeProfile::load(profile.string(), &err);
                if (!real) continue;
                res.profiled = true;
                {
                    std::ofstream adv(advProfile, std::ios::binary);
                    adv << adversarial(*real).serialize();
                }
                res.ownStatus = buildCase(c.entry, exe, &res.ownErr, {}, profile.string());
                if (res.ownStatus == 0) res.own = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                res.advStatus = buildCase(c.entry, exe, &res.advErr, {}, advProfile.string());
                if (res.advStatus == 0) res.adv = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                std::filesystem::remove(profile, ec);
                std::filesystem::remove(advProfile, ec);
            }
        };
        std::vector<std::thread> threads;
        for (unsigned int t = 0; t < oracle::getWorkerJobCount(); ++t) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    });

    size_t profiled = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const FeedbackCase& res = results[i];
        if (res.skip) continue;
        profiled += res.profiled ? 1 : 0;
        SUBCASE(cases[i].id.c_str()) {
            REQUIRE_MESSAGE(res.buildStatus == 0, ("instrumented build failed: " + res.buildErr));
            CHECK_MESSAGE(res.instrumented.output == res.expected,
                          ("instrumented build differs from the pinned bytes\n" + res.instrumented.errors));
            if (res.profiled) {
                REQUIRE_MESSAGE(res.ownStatus == 0, ("build on its own profile failed: " + res.ownErr));
                CHECK_MESSAGE(!res.own.timedOut, "built on its own profile, timed out");
                CHECK_MESSAGE(res.own.exitCode == res.instrumented.exitCode, "built on its own profile, exit code");
                CHECK_MESSAGE(res.own.output == res.expected,
                              ("built on its own profile, differs from the pinned bytes\n" + res.own.errors));
                REQUIRE_MESSAGE(res.advStatus == 0, ("build on the adversarial profile failed: " + res.advErr));
                CHECK_MESSAGE(!res.adv.timedOut, "built on the adversarial profile, timed out");
                CHECK_MESSAGE(res.adv.exitCode == res.instrumented.exitCode,
                              "built on the adversarial profile, exit code");
                CHECK_MESSAGE(res.adv.output == res.expected,
                              ("built on the adversarial profile, differs from the pinned bytes\n" +
                               res.adv.errors));
            }
        }
    }
    // Most cases exit normally and write a profile; none writing one would
    // mean steps 2 and 3 never ran.
    CHECK(profiled > 0);
}
