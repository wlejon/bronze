#include <doctest/doctest.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "check/deterministic_check.h"
#include "check/prove_equiv.h"
#include "check/synth_tests.h"

namespace {

using bronze::check::CheckDiagnostic;
using bronze::check::DeterministicHazardKind;
using bronze::check::EquivStatus;
using bronze::check::ProveEquivReport;
using bronze::check::SynthTestKind;
using bronze::check::SynthTestsReport;

}  // namespace

TEST_CASE("prove-equiv: formal bitvector equivalence proofs and counterexamples") {
    SUBCASE("proves strength reduction x * 8 === x << 3") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            "export function scale(x) { return x * 8; }",
            "export function scale(x) { return x << 3; }",
            "", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].name == "scale");
        CHECK(rep.functions[0].status == EquivStatus::Equivalent);
        CHECK(rep.allEquivalent());
    }

    SUBCASE("proves De Morgan's law ~(a & b) === (~a) | (~b)") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            "export function demorgan(a, b) { return ~(a & b); }",
            "export function demorgan(a, b) { return (~a) | (~b); }",
            "", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Equivalent);
        CHECK(rep.allEquivalent());
    }

    SUBCASE("proves branching abs equivalent to Math.abs") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function myAbs(x) {
                    if (x < 0) {
                        return -x;
                    } else {
                        return x;
                    }
                }
            )",
            R"(
                export function myAbs(x) {
                    return Math.abs(x);
                }
            )",
            "myAbs", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Equivalent);
    }

    SUBCASE("proves bounded loop unrolling equivalent to closed-form expression") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function quad(x) {
                    let s = 0;
                    for (let i = 0; i < 4; i = i + 1) {
                        s = s + x;
                    }
                    return s;
                }
            )",
            R"(
                export function quad(x) {
                    return x * 4;
                }
            )",
            "quad", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Equivalent);
    }

    SUBCASE("finds concrete counterexample on off-by-one bug in branching abs") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function buggyAbs(x) {
                    if (x <= 0) {
                        return -x - 1;
                    } else {
                        return x;
                    }
                }
            )",
            R"(
                export function buggyAbs(x) {
                    return Math.abs(x);
                }
            )",
            "buggyAbs", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Counterexample);
        CHECK(rep.hasCounterexample());
        REQUIRE(rep.functions[0].counterexampleArgs.size() == 1);
        int64_t x = rep.functions[0].counterexampleArgs[0];
        CHECK(x <= 0);
        CHECK(rep.functions[0].oldReturnValue != rep.functions[0].newReturnValue);

        std::string text = rep.renderText();
        CHECK(text.find("COUNTEREXAMPLE") != std::string::npos);
        std::string json = rep.renderJson();
        CHECK(json.find("\"status\":\"COUNTEREXAMPLE\"") != std::string::npos);
    }

    SUBCASE("finds concrete counterexample when shift amount is wrong (x * 8 vs x << 2)") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            "export function f(x) { return x * 8; }",
            "export function f(x) { return x << 2; }",
            "f", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Counterexample);
        REQUIRE(rep.functions[0].counterexampleArgs.size() == 1);
        int64_t x = rep.functions[0].counterexampleArgs[0];
        CHECK(x != 0);
        CHECK(rep.functions[0].oldReturnValue == x * 8);
        CHECK(rep.functions[0].newReturnValue == (x << 2));
    }
}

TEST_CASE("check --deterministic: lockstep multiplayer & replay verifier") {
    SUBCASE("detects Math.random, Date.now, performance.now, transcendental Math, and unsorted keys") {
        std::vector<CheckDiagnostic> diags;
        std::string err;
        bool ok = bronze::check::checkDeterministicSource(
            R"(
                function stepSimulation(entities) {
                    const r = Math.random();
                    const t1 = Date.now();
                    const t2 = performance.now();
                    const d = new Date();
                    const angle = Math.sin(r) + Math.cos(t1);
                    const keys = Object.keys(entities);
                    for (const k in entities) {
                        entities[k] = angle;
                    }
                    return keys.length + d;
                }
            )",
            diags, &err);
        REQUIRE(ok);
        REQUIRE(err.empty());

        uint32_t prngCount = 0;
        uint32_t clockCount = 0;
        uint32_t floatCount = 0;
        uint32_t orderCount = 0;
        for (const auto& d : diags) {
            switch (d.kind) {
                case DeterministicHazardKind::UnseededPrng: ++prngCount; break;
                case DeterministicHazardKind::WallClockTime: ++clockCount; break;
                case DeterministicHazardKind::TranscendentalFloatDrift: ++floatCount; break;
                case DeterministicHazardKind::ObjectIterationOrder: ++orderCount; break;
                case DeterministicHazardKind::CrossArchBitwiseDrift: break;
                case DeterministicHazardKind::ExternalStateAccess: break;
            }
        }
        CHECK(prngCount == 1);
        CHECK(clockCount == 3);
        CHECK(floatCount == 2);
        CHECK(orderCount == 2);
    }

    SUBCASE("passes deterministic fixed-point simulation with xorshift32 and sorted keys") {
        std::vector<CheckDiagnostic> diags;
        std::string err;
        bool ok = bronze::check::checkDeterministicSource(
            R"(
                function xorshift32(seed) {
                    let x = seed | 0;
                    x = x ^ (x << 13);
                    x = x ^ (x >>> 17);
                    x = x ^ (x << 5);
                    return x | 0;
                }

                function stepLockstep(state, posX, velX, dtFixed) {
                    const sortedKeys = Object.keys(state).sort();
                    const nextPos = (posX + ((velX * dtFixed) >> 8)) | 0;
                    return xorshift32(nextPos + sortedKeys.length);
                }
            )",
            diags, &err);
        REQUIRE(ok);
        REQUIRE(err.empty());
        CHECK(diags.empty());
    }

    SUBCASE("rejects Math/Date aliases, transitive non-deterministic helpers, and mutable outer state (#156)") {
        {
            std::vector<CheckDiagnostic> diags;
            std::string err;
            bool ok = bronze::check::checkDeterministicSource(
                R"(
                    const M = Math;
                    function tickAlias() {
                        const r = M.random;
                        return M.random() + r();
                    }
                )",
                diags, &err);
            REQUIRE(ok);
            REQUIRE(err.empty());
            bool foundPrng = false;
            for (const auto& d : diags) {
                if (d.kind == DeterministicHazardKind::UnseededPrng && d.functionName == "tickAlias") {
                    foundPrng = true;
                }
            }
            CHECK(foundPrng);
        }
        {
            std::vector<CheckDiagnostic> diags;
            std::string err;
            bool ok = bronze::check::checkDeterministicSource(
                R"(
                    function h() {
                        return Date.now();
                    }
                    function tick(s) {
                        return s + h();
                    }
                )",
                diags, &err);
            REQUIRE(ok);
            REQUIRE(err.empty());
            bool hFlagged = false;
            bool tickFlagged = false;
            for (const auto& d : diags) {
                if (d.kind == DeterministicHazardKind::WallClockTime) {
                    if (d.functionName == "h") hFlagged = true;
                    if (d.functionName == "tick") tickFlagged = true;
                }
            }
            CHECK(hFlagged);
            CHECK(tickFlagged);
        }
        {
            std::vector<CheckDiagnostic> diags;
            std::string err;
            bool ok = bronze::check::checkDeterministicSource(
                R"(
                    let count = 0;
                    function tick(s) {
                        count++;
                        return s + count;
                    }
                )",
                diags, &err);
            REQUIRE(ok);
            REQUIRE(err.empty());
            bool foundExternal = false;
            for (const auto& d : diags) {
                if (d.kind == DeterministicHazardKind::ExternalStateAccess && d.functionName == "tick") {
                    foundExternal = true;
                }
            }
            CHECK(foundExternal);
        }
        {
            std::vector<CheckDiagnostic> diags;
            std::string err;
            bool ok = bronze::check::checkDeterministicSource(
                R"(
                    "use deterministic";
                    function stepDeterministicPhysics(x, y, z) {
                        const s = Math.sin(x) + Math.cos(y);
                        const a = Math.atan2(y, x) + Math.exp(z);
                        const p = Math.pow(x, 2) + (x ** 3) + Math.hypot(x, y) + Math.cbrt(z);
                        return s * a + p;
                    }
                )",
                diags, &err);
            REQUIRE(ok);
            REQUIRE(err.empty());
            CHECK(diags.empty());
        }
    }
}

TEST_CASE("synth-tests: symbolic path-covering and hazard-reproducing test synthesis") {
    SUBCASE("synthesizes concrete inputs covering all branches of nested conditionals") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeSourceTests(
            R"(
                export function classify(x, y) {
                    if (x > 50) {
                        if (y < -5) {
                            return 10;
                        } else {
                            return 20;
                        }
                    } else if (x === 7) {
                        return 30;
                    } else {
                        return 40;
                    }
                }
            )",
            &err);
        REQUIRE(err.empty());
        CHECK(rep.coveredBlocks == rep.totalBlocks);
        CHECK(rep.coveredBlocks >= 7);

        bool saw10 = false, saw20 = false, saw30 = false, saw40 = false;
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage) continue;
            if (!t.hasExpectedReturn) continue;
            if (t.expectedReturn == 10) {
                saw10 = true;
                REQUIRE(t.args.size() == 2);
                CHECK(t.args[0] > 50);
                CHECK(t.args[1] < -5);
            } else if (t.expectedReturn == 20) {
                saw20 = true;
                REQUIRE(t.args.size() == 2);
                CHECK(t.args[0] > 50);
                CHECK(t.args[1] >= -5);
            } else if (t.expectedReturn == 30) {
                saw30 = true;
                REQUIRE(t.args.size() == 2);
                CHECK(t.args[0] == 7);
            } else if (t.expectedReturn == 40) {
                saw40 = true;
                REQUIRE(t.args.size() == 2);
                CHECK(t.args[0] <= 50);
                CHECK(t.args[0] != 7);
            }
        }
        CHECK(saw10);
        CHECK(saw20);
        CHECK(saw30);
        CHECK(saw40);

        std::string jsSuite = rep.renderJs();
        CHECK(jsSuite.find("assertEq(classify(") != std::string::npos);
        std::string jsonSuite = rep.renderJson();
        CHECK(jsonSuite.find("\"coveredBlocks\":") != std::string::npos);
    }

    SUBCASE("synthesizes concrete hazard reproducers for division by zero, OOB index, and throw") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeSourceTests(
            R"(
                export function riskyDivide(a, b) {
                    if (a > 10) {
                        return 100 / (b - 3);
                    }
                    return a;
                }

                export function riskyLookup(idx) {
                    const table = [11, 22, 33];
                    return table[idx];
                }

                export function riskySecret(code) {
                    if (code === 1337) {
                        throw new Error("secret trap");
                    }
                    return code + 1;
                }
            )",
            &err);
        REQUIRE(err.empty());
        CHECK(rep.hazardReproducerCount >= 3);

        bool foundDivZero = false;
        bool foundOob = false;
        bool foundThrow = false;
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::HazardReproducer) continue;
            if (t.functionName == "riskyDivide") {
                foundDivZero = true;
                REQUIRE(t.args.size() == 2);
                CHECK(t.args[0] > 10);
                CHECK(t.args[1] == 3);
            } else if (t.functionName == "riskyLookup") {
                foundOob = true;
                REQUIRE(t.args.size() == 1);
                CHECK((t.args[0] < 0 || t.args[0] >= 3));
            } else if (t.functionName == "riskySecret") {
                foundThrow = true;
                REQUIRE(t.args.size() == 1);
                CHECK(t.args[0] == 1337);
            }
        }
        CHECK(foundDivZero);
        CHECK(foundOob);
        CHECK(foundThrow);
    }

    SUBCASE("a read through an array literal returns the element, not 0") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeSourceTests(
            R"(
                export function gate(x) {
                    const t = [5, 6, 7, 8];
                    t[2] = 70;
                    const v = t[x & 3];
                    if (v > 6) return v;
                    return -v;
                }
            )",
            &err);
        REQUIRE(err.empty());
        const int64_t table[] = {5, 6, 70, 8};
        size_t checked = 0;
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage || !t.hasExpectedReturn) continue;
            REQUIRE(t.args.size() == 1);
            int64_t v = table[t.args[0] & 3];
            CHECK(t.expectedReturn == (v > 6 ? v : -v));
            ++checked;
        }
        CHECK(checked >= 2);
    }
}

TEST_CASE("CLI file entry points: runProveEquiv, runDeterministicCheckFile, and runSynthTests") {
    const std::string oldFile = "/tmp/bronze_prove_old_test.js";
    const std::string newFile = "/tmp/bronze_prove_new_test.js";
    const std::string badFile = "/tmp/bronze_prove_bad_test.js";
    const std::string outTestFile = "/tmp/bronze_synth_out_test.js";

    {
        FILE* f = std::fopen(oldFile.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("export function compute(x, y) { return (x * 8) + (~(x & y)); }\n", f);
        std::fclose(f);
    }
    {
        FILE* f = std::fopen(newFile.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("export function compute(x, y) { return (x << 3) + ((~x) | (~y)); }\n", f);
        std::fclose(f);
    }
    {
        FILE* f = std::fopen(badFile.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("export function compute(x, y) { return (x << 2) + ((~x) | (~y)); }\n", f);
        std::fclose(f);
    }

    std::ostringstream out, err;
    int rc = bronze::check::runProveEquiv(oldFile, newFile, "compute", false, out, err);
    CHECK(rc == 0);
    CHECK(out.str().find("compute: EQUIVALENT") != std::string::npos);

    out.str("");
    err.str("");
    rc = bronze::check::runProveEquiv(oldFile, badFile, "compute", true, out, err);
    CHECK(rc == 1);
    CHECK(out.str().find("\"status\":\"COUNTEREXAMPLE\"") != std::string::npos);

    std::string detOut;
    rc = bronze::check::runDeterministicCheckFile(newFile, &detOut);
    CHECK(rc == 0);
    CHECK(detOut.find("0 lockstep hazards found") != std::string::npos);

    out.str("");
    err.str("");
    rc = bronze::check::runSynthTests(oldFile, outTestFile, false, out, err);
    CHECK(rc == 0);

    std::remove(oldFile.c_str());
    std::remove(newFile.c_str());
    std::remove(badFile.c_str());
    std::remove(outTestFile.c_str());
}

