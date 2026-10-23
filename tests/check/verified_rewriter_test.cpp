#include <doctest/doctest.h>

#include "check/auto_kernel_rewriter.h"
#include "check/cegis_superopt.h"
#include "check/js_source_rewriter.h"

using bronze::check::analyzeAndRewriteLoopToKernel;
using bronze::check::CegisSuperoptOptions;
using bronze::check::JsRewriteOptions;
using bronze::check::rewriteJsSource;
using bronze::check::superoptimizeJsFunction;

TEST_CASE("cegis_superopt (#142): concrete-pruned enumerative CEGIS synthesizes Hacker's Delight idioms") {
    SUBCASE("synthesizes branchless abs(x) from branchy if/else") {
        auto res = superoptimizeJsFunction(R"(
            function branchyAbs(x) {
                if (x < 0) {
                    return -x;
                }
                return x;
            }
        )");
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(res.synthesizedExpression == "((x ^ (x >> 31)) - (x >> 31))");
        CHECK(res.stats.candidatesTested > 1);
        CHECK(res.stats.concretePruned > 0);
        CHECK(res.stats.smtQueries >= 1);
        CHECK(res.stats.costAfter < res.stats.costBefore);
    }

    SUBCASE("synthesizes branchless ReLU max(x, 0) via sign-mask") {
        auto res = superoptimizeJsFunction(R"(
            function relu(x) {
                if (x < 0) return 0;
                return x;
            }
        )");
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(res.synthesizedExpression == "(x & ~(x >> 31))");
        CHECK(res.stats.concretePruned > 0);
    }

    SUBCASE("synthesizes fused shift-and-mask from redundant chain") {
        auto res = superoptimizeJsFunction(R"(
            function extractByte(x) {
                let a = x >> 8;
                let b = a & 65535;
                return b & 255;
            }
        )");
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(res.synthesizedExpression == "((x >> 8) & 255)");
    }

    SUBCASE("synthesizes min(x, y) from branchy 2-input comparison") {
        auto res = superoptimizeJsFunction(R"(
            function min2(x, y) {
                if (x < y) {
                    return x;
                } else {
                    return y;
                }
            }
        )");
        CHECK(res.verified);
        CHECK(res.optimized);
        CHECK(!res.synthesizedExpression.empty());
        CHECK(res.stats.concretePruned > 5);
    }
}

TEST_CASE("js_source_rewriter (#143): SMT dead-guard elimination, strength reduction, Horner, and deforestation") {
    SUBCASE("eliminates dead guards, reduces Math.floor/pow, and factors cubic polynomial into Horner form") {
        auto rep = rewriteJsSource(R"(
            function polySoup(x) {
                let u = x & 255;
                if (u < 0) return -1;
                if (u > 255) return 999;
                let a = Math.max(0, x & 255);
                let b = Math.floor((x & 255) / 8);
                let p = Math.pow(u, 3) + 3 * Math.pow(u, 2) + 3 * u + 1;
                return a + b + p;
            }
        )");
        CHECK(rep.allProvenEquivalent);
        CHECK(rep.rewrites.size() >= 4);
        CHECK(rep.rewrittenSource.find("Math.max") == std::string::npos);
        CHECK(rep.rewrittenSource.find("Math.floor") == std::string::npos);
        CHECK(rep.rewrittenSource.find("Math.pow") == std::string::npos);
        CHECK(rep.rewrittenSource.find(">> 3") != std::string::npos);
        CHECK(rep.rewrittenSource.find("((u + 3) * u + 3) * u + 1") != std::string::npos);
        CHECK(rep.estimatedOpsAfter < rep.estimatedOpsBefore);
    }

    SUBCASE("fuses .map().filter().reduce() pipeline into a single zero-allocation loop") {
        auto rep = rewriteJsSource(R"(
            function sumFilteredSquares(items) {
                return items.map(x => x * 2).filter(y => y > 10).reduce((acc, z) => acc + z, 0);
            }
        )");
        CHECK(rep.allProvenEquivalent);
        REQUIRE(rep.rewrites.size() == 1);
        CHECK(rep.rewrites[0].ruleName == "loop-deforestation");
        CHECK(rep.rewrittenSource.find(".map(") == std::string::npos);
        CHECK(rep.rewrittenSource.find(".filter(") == std::string::npos);
        CHECK(rep.rewrittenSource.find(".reduce(") == std::string::npos);
        CHECK(rep.rewrittenSource.find("for (let __i = 0") != std::string::npos);
    }

    SUBCASE("reduces O(n) arithmetic series loop to closed-form Gauss expression") {
        auto rep = rewriteJsSource(R"(
            function sumSeries(n) {
                let s = 0;
                for (let i = 0; i < n; i++) {
                    s += i;
                }
                return s;
            }
        )");
        CHECK(rep.allProvenEquivalent);
        REQUIRE(!rep.rewrites.empty());
        CHECK(rep.rewrites[0].ruleName == "closed-form-series");
        CHECK(rep.rewrittenSource.find("((n * (n - 1)) / 2)") != std::string::npos);
    }
}

TEST_CASE("auto_kernel_rewriter (#144): SMT loop-carried dependency prover and @kernel parallelizer") {
    SUBCASE("proves independent SAXPY TypedArray loop race-free and emits @kernel") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function saxpy(out, x, y, a, n) {
                for (let i = 0; i < n; i++) {
                    out[i] = a * x[i] + y[i];
                }
            }
        )");
        CHECK(rep.foundLoop);
        CHECK(rep.parallelizable);
        CHECK(rep.provenNoWaw);
        CHECK(rep.provenNoRaw);
        CHECK(rep.provenNoWar);
        CHECK(rep.provenInBounds);
        CHECK(rep.hazards.empty());
        CHECK(rep.kernelSource.find("// @kernel") != std::string::npos);
        CHECK(rep.kernelSource.find("global_tid_x()") != std::string::npos);
    }

    SUBCASE("refutes prefix-sum loop with concrete RAW dependency witness (i1=0, i2=1, idx=0)") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function prefixSum(a, x, n) {
                for (let i = 0; i < n; i++) {
                    a[i] = a[i - 1] + x[i];
                }
            }
        )");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        CHECK_FALSE(rep.provenNoRaw);
        REQUIRE(!rep.hazards.empty());
        CHECK(rep.hazards[0].hazardKind == "RAW");
        CHECK(rep.hazards[0].arrayName == "a");
        CHECK(rep.hazards[0].iterB == rep.hazards[0].iterA + 1);
    }

    SUBCASE("refutes constant-index store loop with WAW race witness") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function raceStore(out, x, n) {
                for (let i = 0; i < n; i++) {
                    out[0] = x[i];
                }
            }
        )");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        CHECK_FALSE(rep.provenNoWaw);
        REQUIRE(!rep.hazards.empty());
        CHECK(rep.hazards[0].hazardKind == "WAW");
        CHECK(rep.hazards[0].conflictIndex == 0);
    }
}
