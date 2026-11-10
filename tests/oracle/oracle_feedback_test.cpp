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
// `harvest` is `--harvest-out`.
int buildCase(const std::filesystem::path& entry, const std::filesystem::path& exe, std::string* err,
              const std::string& out, const std::string& in, const std::string& harvest = {}) {
    return bronze::cli::runBuild(entry.string(), exe.string(), err, true, false, false, {}, false, nullptr, {}, {},
                                 false, true, {}, false, {}, {}, false, {}, {}, {}, {}, false, false, {}, false, {},
                                 false, out, in, harvest);
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

// The harvest made adversarial: every site holds one way, so it reads as
// monomorphic with every threshold met; its receiver's keys are rotated, and
// a method site's callee is another site's.
bronze::spec::TypeProfile adversarial(const bronze::spec::Harvest& real) {
    std::set<std::string> calleeSet;
    for (const auto& [index, site] : real.ics) {
        for (const auto& way : site.ways) {
            if (site.method && way.callee != "*other*") calleeSet.insert(way.callee);
        }
    }
    const std::vector<std::string> callees(calleeSet.begin(), calleeSet.end());
    auto otherCallee = [&](const std::string& own) {
        if (callees.empty()) return std::string("nothing@00000000");
        auto it = std::upper_bound(callees.begin(), callees.end(), own);
        return it == callees.end() ? callees.front() : *it;
    };
    bronze::spec::Harvest out = real;
    for (auto& [index, site] : out.ics) {
        if (site.ways.empty()) continue;
        bronze::spec::HarvestWay way = site.ways.front();
        way.receiver = rotatedShape(way.receiver);
        if (site.method) {
            way.form = "direct";
            way.callee = otherCallee(way.callee);
        }
        site.ways = {way};
        site.full = false;
        site.mega = false;
    }
    bronze::spec::TypeProfile profile;
    profile.setHarvest(std::move(out));
    return profile;
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

// BRONZE_ORACLE_FEEDBACK_CASES=<id substring>[,...] runs the cases whose id
// contains one of them: each suite is three builds a case.
std::vector<oracle::OracleCase> selectedCases(const std::filesystem::path& casesDir) {
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
    return cases;
}

// The case's source and pinned bytes, or false when it cannot be held to
// them (no pinned bytes, or it reads the clock or draws numbers).
bool deterministicCase(const oracle::OracleCase& c, std::string& expected) {
    std::string code;
    std::filesystem::path expectedPath = c.entry;
    expectedPath.replace_extension(".expected");
    return oracle::readFileBytes(c.entry, code) && oracle::readFileBytes(expectedPath, expected) &&
           !oracle::readsTheClock(code) && code.find("Math.random") == std::string::npos;
}

}  // namespace

TEST_CASE("Oracle feedback suite") {
    const std::filesystem::path casesDir = oracle::findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    std::vector<oracle::OracleCase> cases = selectedCases(casesDir);
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
                if (!deterministicCase(c, res.expected)) {
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

// The inline-cache harvest (docs/type-feedback.md, "Harvest"), the same three
// steps from an ordinary build: built with `--harvest-out` and run (the pinned
// bytes, and the harvest written at exit), built with `--feedback-in` on the
// harvest, and on an adversarial harvest made from it (one way a site, its
// keys rotated, a method site calling another site's callee). The two
// rebuilds must print the pinned bytes and exit as the case does.
TEST_CASE("Oracle harvest suite") {
    const std::filesystem::path casesDir = oracle::findCasesDirectory();
    REQUIRE_MESSAGE(!casesDir.empty(), "Oracle test cases directory not found");
    std::vector<oracle::OracleCase> cases = selectedCases(casesDir);
    REQUIRE(!cases.empty());
    static std::vector<FeedbackCase> results;
    static std::vector<size_t> harvestedSites;
    static std::once_flag once;
    std::call_once(once, [&] {
        results.resize(cases.size());
        harvestedSites.resize(cases.size());
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t i; (i = next.fetch_add(1)) < cases.size();) {
                const oracle::OracleCase& c = cases[i];
                FeedbackCase& res = results[i];
                if (!deterministicCase(c, res.expected)) {
                    res.skip = true;
                    continue;
                }
                const std::filesystem::path dir = bronze_test::tempDir();
                const std::filesystem::path harvest = dir / (c.id + "_oracle_harvest.tfb");
                const std::filesystem::path advHarvest = dir / (c.id + "_oracle_harvest_adv.tfb");
                const std::filesystem::path exe = dir / (c.id + "_oracle_hv.exe");
                std::error_code ec;
                std::filesystem::remove(harvest, ec);
                oracle::removeProgram(exe, ec);
                res.buildStatus = buildCase(c.entry, exe, &res.buildErr, {}, {}, harvest.string());
                if (res.buildStatus != 0) continue;
                res.instrumented = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                std::string err;
                const auto real = bronze::spec::TypeProfile::load(harvest.string(), &err);
                if (!real || real->harvest() == nullptr) continue;
                res.profiled = true;
                harvestedSites[i] = real->harvest()->ics.size();
                {
                    std::ofstream adv(advHarvest, std::ios::binary);
                    adv << adversarial(*real->harvest()).serialize();
                }
                res.ownStatus = buildCase(c.entry, exe, &res.ownErr, {}, harvest.string());
                if (res.ownStatus == 0) res.own = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                res.advStatus = buildCase(c.entry, exe, &res.advErr, {}, advHarvest.string());
                if (res.advStatus == 0) res.adv = runBuilt(exe);
                oracle::removeProgram(exe, ec);
                std::filesystem::remove(harvest, ec);
                std::filesystem::remove(advHarvest, ec);
            }
        };
        std::vector<std::thread> threads;
        for (unsigned int t = 0; t < oracle::getWorkerJobCount(); ++t) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    });

    size_t harvested = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const FeedbackCase& res = results[i];
        if (res.skip) continue;
        harvested += harvestedSites[i] != 0 ? 1 : 0;
        SUBCASE(cases[i].id.c_str()) {
            REQUIRE_MESSAGE(res.buildStatus == 0, ("harvesting build failed: " + res.buildErr));
            CHECK_MESSAGE(res.instrumented.output == res.expected,
                          ("harvesting build differs from the pinned bytes\n" + res.instrumented.errors));
            if (res.profiled) {
                REQUIRE_MESSAGE(res.ownStatus == 0, ("build on its own harvest failed: " + res.ownErr));
                CHECK_MESSAGE(!res.own.timedOut, "built on its own harvest, timed out");
                CHECK_MESSAGE(res.own.exitCode == res.instrumented.exitCode, "built on its own harvest, exit code");
                CHECK_MESSAGE(res.own.output == res.expected,
                              ("built on its own harvest, differs from the pinned bytes\n" + res.own.errors));
                REQUIRE_MESSAGE(res.advStatus == 0, ("build on the adversarial harvest failed: " + res.advErr));
                CHECK_MESSAGE(!res.adv.timedOut, "built on the adversarial harvest, timed out");
                CHECK_MESSAGE(res.adv.exitCode == res.instrumented.exitCode,
                              "built on the adversarial harvest, exit code");
                CHECK_MESSAGE(res.adv.output == res.expected,
                              ("built on the adversarial harvest, differs from the pinned bytes\n" +
                               res.adv.errors));
            }
        }
    }
    // A harvest with no site in it in every case would mean the rebuilds
    // took nothing from it.
    CHECK(harvested > 0);
}
