#include <doctest/doctest.h>

#include "check/js_source_rewriter.h"
#include "check/loop_recurrence_synth.h"
#include "check/prove_equiv.h"
#include "check/prove_loop_induction.h"

using namespace bronze::check;

TEST_CASE("Inductive loop prover: unbounded Gauss sum and odd-number sum") {
    const char* gaussLoop = R"(
        function sumGauss(n) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i;
            }
            return s;
        }
    )";
    const char* gaussClosed = R"(
        function sumGauss(n) {
            return (n <= 0) ? 0 : ((n * (n - 1)) / 2);
        }
    )";

    auto rep1 = proveLoopClosedFormInductive(gaussLoop, gaussClosed, -100, 10000);
    CHECK(rep1.status == EquivStatus::Equivalent);
    CHECK(rep1.isEquivalent());

    auto rep2 = proveSourceEquivalent(gaussLoop, gaussClosed, "sumGauss", -100, 10000);
    CHECK(rep2.status == EquivStatus::Equivalent);

    const char* oddLoop = R"(
        function sumOdd(n) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += 2 * i + 1;
            }
            return s;
        }
    )";
    const char* oddClosed = R"(
        function sumOdd(n) {
            return (n <= 0) ? 0 : (n * n);
        }
    )";

    auto rep3 = proveLoopClosedFormInductive(oddLoop, oddClosed, 0, 10000);
    CHECK(rep3.status == EquivStatus::Equivalent);

    auto rep4 = proveSourceEquivalent(oddLoop, oddClosed, "sumOdd", 0, 10000);
    CHECK(rep4.status == EquivStatus::Equivalent);
}

TEST_CASE("Inductive loop prover: affine and quadratic series over unbounded domains") {
    const char* affineLoop = R"(
        function sumAffine(n) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += 3 * i + 2;
            }
            return s;
        }
    )";
    const char* affineClosed = R"(
        function sumAffine(n) {
            return (n <= 0) ? 0 : (((3 * n * (n - 1)) / 2) + 2 * n);
        }
    )";

    auto repAffine = proveLoopClosedFormInductive(affineLoop, affineClosed, 0, 10000);
    CHECK(repAffine.status == EquivStatus::Equivalent);

    const char* sqLoop = R"(
        function sumSquares(n) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i * i;
            }
            return s;
        }
    )";
    const char* sqClosed = R"(
        function sumSquares(n) {
            return (n <= 0) ? 0 : ((n * (n - 1) * (2 * n - 1)) / 6);
        }
    )";

    auto repSq = proveLoopClosedFormInductive(sqLoop, sqClosed, 0, 500);
    CHECK(repSq.status == EquivStatus::Equivalent);

    auto repSq2 = proveSourceEquivalent(sqLoop, sqClosed, "sumSquares", 0, 500);
    CHECK(repSq2.status == EquivStatus::Equivalent);
}

TEST_CASE("Inductive loop prover: refutes buggy closed-form candidate with counterexample") {
    const char* gaussLoop = R"(
        function sumGauss(n) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i;
            }
            return s;
        }
    )";
    const char* buggyClosed = R"(
        function sumGauss(n) {
            return (n <= 0) ? 0 : ((n * (n + 1)) / 2);
        }
    )";

    auto rep = proveLoopClosedFormInductive(gaussLoop, buggyClosed, 0, 10000);
    CHECK(rep.status == EquivStatus::Counterexample);
    CHECK(rep.hasCounterexample());
    CHECK(rep.expectedOutput != rep.actualOutput);
}

TEST_CASE("Inductive loop prover: relational loop-vs-loop equivalence") {
    const char* loopOld = R"(
        function weightedSum(n, k) {
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i * 4 + i * k;
            }
            return s;
        }
    )";
    const char* loopNew = R"(
        function weightedSum(n, k) {
            const step = k + 4;
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i * step;
            }
            return s;
        }
    )";

    auto rep = proveSourceEquivalent(loopOld, loopNew, "weightedSum", 0, 10000);
    CHECK(rep.status == EquivStatus::Equivalent);

    const char* loopBuggy = R"(
        function weightedSum(n, k) {
            const step = k + 3;
            let s = 0;
            for (let i = 0; i < n; i++) {
                s += i * step;
            }
            return s;
        }
    )";

    auto repBuggy = proveSourceEquivalent(loopOld, loopBuggy, "weightedSum", 0, 10000);
    CHECK(repBuggy.status == EquivStatus::Counterexample);
    CHECK(repBuggy.hasCounterexample());
}

TEST_CASE("Newton finite-difference recurrence synthesis and rewriteJsSource integration") {
    {
        const char* src = R"(function sumOdd(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s += 2 * i + 1;
    }
    return s;
})";
        auto rep = rewriteJsSource(src);
        CHECK(!rep.rewrites.empty());
        CHECK(rep.allProvenEquivalent);
        CHECK(rep.rewrittenSource.find("return (n <= 0) ? 0 : (n * n);") != std::string::npos);
    }

    {
        const char* src = R"(function sumAffine(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s += 3 * i + 2;
    }
    return s;
})";
        auto rep = rewriteJsSource(src);
        CHECK(!rep.rewrites.empty());
        CHECK(rep.allProvenEquivalent);
        CHECK(rep.rewrittenSource.find("return (n <= 0) ? 0 :") != std::string::npos);
    }

    {
        const char* src = R"(function sumSquares(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s += i * i;
    }
    return s;
})";
        auto rep = rewriteJsSource(src);
        CHECK(!rep.rewrites.empty());
        CHECK(rep.allProvenEquivalent);
        CHECK(rep.rewrittenSource.find("((n * (n - 1) * (2 * n - 1)) / 6)") != std::string::npos);
    }
}

TEST_CASE("Symbolic FP and safe-integer predicates in proveSourceEquivalent") {
    const char* fnA = R"(
        function normInt(x) {
            if (Number.isFinite(x) && Number.isSafeInteger(x) && !Number.isNaN(x)) {
                return Math.fround(x) + 1;
            }
            return 0;
        }
    )";
    const char* fnB = R"(
        function normInt(x) {
            return x + 1;
        }
    )";

    auto rep = proveSourceEquivalent(fnA, fnB, "normInt", -1000, 1000);
    CHECK(rep.status == EquivStatus::Equivalent);
}

TEST_CASE("Bounded loop unrolling: BOUNDED verdict for multi-loop, nested loops, and complex while loops") {
    SUBCASE("multi-loop (setup loop then main loop): BOUNDED equivalence and counterexample") {
        const char* multiA = R"(
            export function twoPhase(n, m, x) {
                let s = 0;
                for (let i = 0; i < (n | 0); i = (i + 1) | 0) {
                    s = (s + ((x | 0) * 2)) | 0;
                }
                for (let j = 0; j < (m | 0); j = (j + 1) | 0) {
                    s = (s + (j ^ 3)) | 0;
                }
                return s;
            }
        )";
        const char* multiB = R"(
            export function twoPhase(n, m, x) {
                let s = 0;
                for (let i = 0; i < (n | 0); i = (i + 1) | 0) {
                    s = (s + ((x | 0) << 1)) | 0;
                }
                for (let j = 0; j < (m | 0); j = (j + 1) | 0) {
                    s = (s + (j ^ 3)) | 0;
                }
                return s;
            }
        )";
        const char* multiBuggy = R"(
            export function twoPhase(n, m, x) {
                let s = 0;
                for (let i = 0; i < (n | 0); i = (i + 1) | 0) {
                    s = (s + ((x | 0) << 1)) | 0;
                }
                for (let j = 0; j <= (m | 0); j = (j + 1) | 0) {
                    s = (s + (j ^ 3)) | 0;
                }
                return s;
            }
        )";

        ProveEquivReport rep = proveSourceEquiv(multiA, multiB, "twoPhase");
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Bounded);
        CHECK(rep.functions[0].bounds.maxLoopIterations > 0);
        CHECK(rep.renderText().find("BOUNDED (loop <= ") != std::string::npos);
        CHECK(rep.renderJson().find("\"status\":\"BOUNDED\"") != std::string::npos);
        CHECK(rep.renderJson().find("\"loopTripCount\":") != std::string::npos);

        ProveEquivReport repBug = proveSourceEquiv(multiA, multiBuggy, "twoPhase");
        REQUIRE(repBug.functions.size() == 1);
        CHECK(repBug.functions[0].status == EquivStatus::Counterexample);
    }

    SUBCASE("nested loops (depth 2): BOUNDED equivalence and counterexample") {
        const char* nestedA = R"(
            export function gridSum(rows, cols, k) {
                let total = 0;
                for (let r = 0; r < (rows | 0); r = (r + 1) | 0) {
                    for (let c = 0; c < (cols | 0); c = (c + 1) | 0) {
                        total = (total + ((r + c) * 2) + (k | 0)) | 0;
                    }
                }
                return total;
            }
        )";
        const char* nestedB = R"(
            export function gridSum(rows, cols, k) {
                let total = 0;
                for (let r = 0; r < (rows | 0); r = (r + 1) | 0) {
                    for (let c = 0; c < (cols | 0); c = (c + 1) | 0) {
                        total = (total + ((r + c) << 1) + (k | 0)) | 0;
                    }
                }
                return total;
            }
        )";
        const char* nestedBuggy = R"(
            export function gridSum(rows, cols, k) {
                let total = 0;
                for (let r = 0; r < (rows | 0); r = (r + 1) | 0) {
                    for (let c = 0; c < (cols | 0); c = (c + 1) | 0) {
                        total = (total + ((r - c) << 1) + (k | 0)) | 0;
                    }
                }
                return total;
            }
        )";

        ProveEquivReport rep = proveSourceEquiv(nestedA, nestedB, "gridSum");
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Bounded);
        CHECK(rep.functions[0].bounds.maxLoopIterations > 0);

        ProveEquivReport repBug = proveSourceEquiv(nestedA, nestedBuggy, "gridSum");
        REQUIRE(repBug.functions.size() == 1);
        CHECK(repBug.functions[0].status == EquivStatus::Counterexample);
    }

    SUBCASE("while loop with complex exit conditions and break: BOUNDED equivalence and counterexample") {
        const char* whileA = R"(
            export function scanSteps(x, limit) {
                let v = x | 0;
                let steps = 0;
                const lim = limit | 0;
                while (v > 0 && steps < lim) {
                    if ((v & 3) === 2) break;
                    v = (v - 1) | 0;
                    steps = (steps + 1) | 0;
                }
                return ((steps * 2) + v) | 0;
            }
        )";
        const char* whileB = R"(
            export function scanSteps(x, limit) {
                let v = x | 0;
                let steps = 0;
                const lim = limit | 0;
                while (steps < lim && v > 0) {
                    if ((v & 3) === 2) return ((steps << 1) + v) | 0;
                    v = (v + -1) | 0;
                    steps = (steps + 1) | 0;
                }
                return (v + (steps << 1)) | 0;
            }
        )";
        const char* whileBuggy = R"(
            export function scanSteps(x, limit) {
                let v = x | 0;
                let steps = 0;
                const lim = limit | 0;
                while (v > 0 && steps < lim) {
                    if ((v & 3) === 3) break;
                    v = (v - 1) | 0;
                    steps = (steps + 1) | 0;
                }
                return ((steps * 2) + v) | 0;
            }
        )";

        ProveEquivReport rep = proveSourceEquiv(whileA, whileB, "scanSteps");
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Bounded);
        CHECK(rep.functions[0].bounds.maxLoopIterations == 8);

        ProveEquivReport repBug = proveSourceEquiv(whileA, whileBuggy, "scanSteps");
        REQUIRE(repBug.functions.size() == 1);
        CHECK(repBug.functions[0].status == EquivStatus::Counterexample);
    }
}
