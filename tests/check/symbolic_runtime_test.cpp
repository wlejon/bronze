#include <doctest/doctest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "check/cegis_optimize.h"
#include "check/fuzz_fn.h"
#include "check/layout_solver.h"
#include "check/prove_fn.h"
#include "check/solve_fn.h"

namespace {

using bronze::check::FuzzAuditReport;
using bronze::check::FuzzOptions;
using bronze::check::LayoutBoxSpec;
using bronze::check::LayoutSolution;
using bronze::check::LayoutSpec;
using bronze::check::OptimizeOptions;
using bronze::check::OptimizeResult;
using bronze::check::ProofCertificateReport;
using bronze::check::ProveOptions;
using bronze::check::SolveOptions;
using bronze::check::SolveResult;
using bronze::check::SolveStatus;

}  // namespace

TEST_CASE("solve_fn (#128): run JS functions backwards via Brass bitvector SMT") {
    SUBCASE("solves arrow predicate (x, y) => (x ^ y) === 82 && x + y === 100") {
        SolveResult res = bronze::check::solveJsFunction(
            "(x, y) => (x ^ y) === 82 && (x + y) === 100 && x > 0 && y > 0");
        REQUIRE(res.sat());
        REQUIRE(res.args.size() == 2);
        int64_t x = res.args[0];
        int64_t y = res.args[1];
        CHECK(((x ^ y) == 82));
        CHECK(((x + y) == 100));
        CHECK(res.modelMap.at("x") == x);
        CHECK(res.modelMap.at("y") == y);
    }

    SUBCASE("inverts Murmur3-style 32-bit hash mixer to find exact preimage") {
        // Compute concrete target for x = 1337 first
        const char* mixSrc = R"(
            function mix(h) {
                let x = h | 0;
                x = Math.imul(x ^ (x >>> 16), 0x7feb352d);
                x = Math.imul(x ^ (x >>> 15), 0x846ca68b);
                x = (x ^ (x >>> 16)) | 0;
                return x;
            }
        )";
        int64_t targetVal = 0;
        REQUIRE(bronze::check::evalConcreteJsFunction(mixSrc, {1337}, &targetVal));

        SolveOptions opts;
        opts.matchReturnValue = true;
        opts.targetReturn = targetVal;
        SolveResult inv = bronze::check::solveJsFunction(mixSrc, opts);
        REQUIRE(inv.sat());
        REQUIRE(inv.args.size() == 1);
        CHECK(inv.args[0] == 1337);
        CHECK(inv.returnValue == targetVal);
    }

    SUBCASE("returns UNSAT when predicate is mathematically impossible") {
        SolveResult res = bronze::check::solveJsFunction(
            "x => ((x & 1) === 0) && ((x & 1) === 1)");
        CHECK(res.unsat());
        CHECK(res.status == SolveStatus::Unsat);
    }

    SUBCASE("solveAllJsFunction enumerates distinct solutions with blocking clauses") {
        SolveOptions opts;
        opts.domainMin = 1;
        opts.domainMax = 20;
        auto solutions = bronze::check::solveAllJsFunction(
            "(a, b) => a + b === 10 && a < b", 4, opts);
        REQUIRE(solutions.size() == 4);
        std::unordered_set<int64_t> seenA;
        for (const auto& sol : solutions) {
            REQUIRE(sol.size() == 2);
            CHECK(sol[0] + sol[1] == 10);
            CHECK(sol[0] < sol[1]);
            CHECK(seenA.insert(sol[0]).second);
        }
    }
}

TEST_CASE("cegis_optimize (#129): CEGIS synthesis, counterexample patching, and equivalence") {
    SUBCASE("synthesizes closed-form Gauss summation from O(n) loop") {
        OptimizeOptions opts;
        opts.domainMin = 0;
        opts.domainMax = 25;
        OptimizeResult res = bronze::check::optimizeJsFunction(R"(
            function sumTo(n) {
                let s = 0;
                for (let i = 0; i < n; i = i + 1) {
                    s = (s + i) | 0;
                }
                return s;
            }
        )", opts);
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(res.optimizedSource.find("((n * (n - 1)) / 2)") != std::string::npos);
    }

    SUBCASE("iterative CEGIS repairs candidate that fails on boundary counterexample") {
        OptimizeOptions opts;
        opts.domainMin = -100;
        opts.domainMax = 100;
        // Candidate works for x != 0, but returns 999 at x === 0 whereas ref returns 0.
        opts.candidates = {
            "function f(x) { if (x === 0) return 999; return (x << 3); }"
        };
        opts.autoPatchCounterexamples = true;

        OptimizeResult res = bronze::check::optimizeJsFunction(
            "function f(x) { return (x * 8) | 0; }", opts);
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(!res.counterexampleHistory.empty());
        CHECK(res.counterexampleHistory[0].args[0] == 0);
        CHECK(res.counterexampleHistory[0].expectedOutput == 0);
        CHECK(res.counterexampleHistory[0].actualOutput == 999);
    }
}

TEST_CASE("fuzz_fn (#133): white-box concolic path exploration & hazard witness generation") {
    SUBCASE("synthesizes 100% branch-covering test vectors and detects division-by-zero witness") {
        FuzzAuditReport rep = bronze::check::fuzzJsFunction(R"(
            function processPacket(tag, denom) {
                if (tag === 0x1337) {
                    if (denom === 0) {
                        return (100 / denom) | 0;
                    }
                    return 42;
                }
                return 1;
            }
        )");
        CHECK(rep.branchCoverage == doctest::Approx(1.0));
        CHECK(rep.coveredBranches == rep.totalBranches);
        CHECK(!rep.testVectors.empty());
        CHECK(!rep.safe);
        REQUIRE(!rep.hazards.empty());
        bool foundDivZeroWitness = false;
        for (const auto& h : rep.hazards) {
            if (h.witnessArgs.size() == 2 && h.witnessArgs[0] == 0x1337 && h.witnessArgs[1] == 0) {
                foundDivZeroWitness = true;
            }
        }
        CHECK(foundDivZeroWitness);
    }

    SUBCASE("certifies safe branching function with zero hazards") {
        FuzzAuditReport rep = bronze::check::fuzzJsFunction(R"(
            function clampByte(x) {
                if (x < 0) return 0;
                if (x > 255) return 255;
                return x;
            }
        )");
        CHECK(rep.safe);
        CHECK(rep.hazards.empty());
        CHECK(rep.branchCoverage == doctest::Approx(1.0));
        CHECK(rep.testVectors.size() >= 3);
    }
}

TEST_CASE("prove_fn (#135): unified formal proof certificate & zero-guard reflection") {
    SUBCASE("emits verified certificate for deterministic, hazard-free function with round-trip inverse") {
        ProveOptions opts;
        opts.domainMin = -10000;
        opts.domainMax = 10000;
        opts.equivalentToSource = "x => ((x << 1) ^ (x >> 31))";
        opts.inverseFnSource = "z => ((z >>> 1) ^ -(z & 1))";

        ProofCertificateReport cert = bronze::check::proveJsFunction(
            "function zigzagEncode(x) { return (x << 1) ^ (x >> 31); }", opts);
        CHECK(cert.verified);
        CHECK(cert.hazardFree);
        CHECK(cert.deterministic);
        CHECK(cert.proofsValid);
        CHECK(cert.equivalent);
        CHECK(cert.roundTripProven);
        CHECK(cert.unboxedF64Ops > 0);
        CHECK(cert.sha256.size() == 64);
        CHECK(cert.certificateJson.find("\"verified\":true") != std::string::npos);
    }

    SUBCASE("flags non-deterministic Math.random() in lockstep audit") {
        ProofCertificateReport cert = bronze::check::proveJsFunction(
            "function roll() { return Math.random() > 0.5 ? 1 : 0; }");
        CHECK(!cert.deterministic);
        CHECK(!cert.lockstepHazards.empty());
        CHECK(!cert.verified);
    }
}

TEST_CASE("layout_solver (#137): non-convex 2D/3D layout solver with non-overlap, grid snap, and aspect ratio") {
    SUBCASE("solves declarative 2D UI layout with non-overlap, 8px grid snap, and 16:9 aspect ratio") {
        LayoutSpec spec;
        spec.containerW = 800;
        spec.containerH = 600;
        spec.nonOverlap = true;
        spec.gap = 8;
        spec.globalSnapGrid = 8;

        LayoutBoxSpec sidebar;
        sidebar.id = "sidebar";
        sidebar.minW = 160;
        sidebar.maxW = 240;
        sidebar.minH = 400;
        sidebar.maxH = 600;
        spec.boxes.push_back(sidebar);

        LayoutBoxSpec video;
        video.id = "video";
        video.minW = 160;
        video.maxW = 480;
        video.minH = 90;
        video.maxH = 360;
        video.aspectNum = 16;
        video.aspectDen = 9;
        spec.boxes.push_back(video);

        spec.constraints = {
            "sidebar.x == 0",
            "sidebar.y == 0",
            "video.x >= sidebar.right + 8"
        };

        LayoutSolution sol = bronze::check::solveLayoutSpec(spec);
        REQUIRE(sol.sat());
        const auto& sb = sol.boxes.at("sidebar");
        const auto& vd = sol.boxes.at("video");
        CHECK(sb.x == 0);
        CHECK(sb.y == 0);
        CHECK((sb.w % 8) == 0);
        CHECK((vd.x % 8) == 0);
        CHECK((vd.w % 8) == 0);
        CHECK((vd.h % 8) == 0);
        CHECK(vd.w * 9 == vd.h * 16);
        CHECK(vd.x >= sb.x + sb.w + 8);
        CHECK(vd.x + vd.w <= 800);
        CHECK(vd.y + vd.h <= 600);
    }

    SUBCASE("solves variable-list + JS predicate layout") {
        SolveOptions opts;
        opts.domainMin = 0;
        opts.domainMax = 500;
        LayoutSolution sol = bronze::check::solveLayoutPredicate(
            {"a_x", "a_w", "b_x", "b_w"},
            "(a_x, a_w, b_x, b_w) => a_x === 16 && a_w === 120 && b_x >= a_x + a_w + 16 && b_w === 200 && (b_x & 7) === 0",
            opts);
        REQUIRE(sol.sat());
        CHECK(sol.boxes.at("a").x == 16);
        CHECK(sol.boxes.at("a").w == 120);
        CHECK(sol.boxes.at("b").x >= 152);
        CHECK((sol.boxes.at("b").x % 8) == 0);
        CHECK(sol.boxes.at("b").w == 200);
    }
}

TEST_CASE("proof_to_codegen (#152): TypedArray integral-index & in-bounds proofs unlock zero-check IL/MIR lowering") {
    std::string src = R"(
        function typedDot() {
            const a = new Float64Array(4);
            let sum = 0;
            for (let i = 0; i < a.length; i++) {
                a[i] = i + 1;
                sum += a[i];
            }
            return sum;
        }
    )";
    ProofCertificateReport cert = bronze::check::proveJsFunction(src);
    CHECK(cert.verified);
    CHECK(cert.proofsValid);
    CHECK(cert.hazardFree);
    CHECK(cert.deterministic);
    // Both ElemSetTyped and ElemGetTyped carry receiver proof + integral-index proof + in-bounds proof (6 guards).
    CHECK(cert.provenGuards >= 6);
}

