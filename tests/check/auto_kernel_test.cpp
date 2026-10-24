#include <doctest/doctest.h>

#include <string>

#include "check/auto_kernel_rewriter.h"

using bronze::check::analyzeAndRewriteLoopToKernel;
using bronze::check::AutoKernelReport;

namespace {

bool hasHazard(const AutoKernelReport& rep, const std::string& kind) {
    for (const auto& h : rep.hazards) {
        if (h.hazardKind == kind) return true;
    }
    return false;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("auto_kernel (#161): the whole body is analysed, nested blocks included") {
    SUBCASE("a store after a nested block races, and no kernel is emitted") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){for(let i=0;i<n;i++){ if(a[i]>0){ o[i]=1; } o[i+1]=a[i]; }}");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        CHECK_FALSE(rep.provenNoWaw);
        CHECK(rep.kernelSource.empty());
        bool sawRace = false;
        for (const auto& h : rep.hazards) {
            if (h.hazardKind == "WAW" && h.arrayName == "o" &&
                (h.writeIndexExpr == "i+1" || h.readIndexExpr == "i+1")) {
                sawRace = true;
                CHECK(h.iterA != h.iterB);
            }
        }
        CHECK(sawRace);
    }

    SUBCASE("a race inside a nested block after the first `}` is found") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function k(a, o, n) {
                for (let i = 0; i < n; i++) {
                    if (a[i] > 0) { o[i] = 1; } else { o[i] = 2; }
                    { if (a[i] < 0) { o[0] = 3; } }
                }
            }
        )");
        CHECK_FALSE(rep.parallelizable);
        CHECK(hasHazard(rep, "WAW"));
    }

    SUBCASE("the emitted kernel holds every statement of a nested body") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function k(a, o, n) {
                for (let i = 0; i < n; i++) {
                    if (a[i] > 0) { o[i] = 1; } else { o[i] = Math.abs(a[i]); }
                    o[i] += a[i] * 2;
                }
            }
        )");
        CHECK(rep.parallelizable);
        CHECK(contains(rep.kernelSource, "if (a[i] > 0) { o[i] = 1; } else { o[i] = Math.abs(a[i]); }"));
        CHECK(contains(rep.kernelSource, "o[i] += a[i] * 2;"));
        CHECK(contains(rep.kernelSource, "let i = global_tid_x();"));
        CHECK(contains(rep.kernelSource, "if (i < n) {"));
    }
}

TEST_CASE("auto_kernel (#161): loop shapes the regexes could not see") {
    SUBCASE("a braceless loop") {
        auto rep = analyzeAndRewriteLoopToKernel("function k(a,o,n){ for(let i=0;i<n;i++) o[i]=a[i]*2; }");
        CHECK(rep.foundLoop);
        CHECK(rep.parallelizable);
        CHECK(contains(rep.kernelSource, "o[i]=a[i]*2;"));
    }

    SUBCASE("a braceless loop whose body is an if/else") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){ for(let i=0;i<n;i++) if (a[i] > 0) o[i]=a[i]; else o[i]=(0 - a[i]); }");
        CHECK(rep.parallelizable);
        CHECK(contains(rep.kernelSource, "if (a[i] > 0) o[i]=a[i]; else o[i]=(0 - a[i]);"));
    }

    SUBCASE("a start of 1, with the step spelled `i += 1`") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function diff(a, o, n) {
                for (let i = 1; i < n; i += 1) {
                    o[i] = a[i] - a[i - 1];
                }
            }
        )");
        CHECK(rep.foundLoop);
        CHECK(rep.parallelizable);
        CHECK(rep.provenInBounds);  // a[i - 1] is a[0] at the first iteration
        CHECK(contains(rep.kernelSource, "if (i >= 1 && i < n) {"));
    }

    SUBCASE("start 0 makes the same a[i - 1] read negative") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function diff(a,o,n){ for(let i=0;i<n;i++){ o[i]=a[i]-a[i-1]; } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK(hasHazard(rep, "OUT_OF_BOUNDS"));
    }

    SUBCASE("a reversed read o[i] = a[n-1-i] is affine and parallel") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function rev(a,o,n){ for(let i=0;i<n;i++){ o[i]=a[n-1-i]; } }");
        CHECK(rep.parallelizable);
        CHECK(rep.provenInBounds);
        CHECK(contains(rep.kernelSource, "o[i]=a[n-1-i];"));
    }

    SUBCASE("an in-place reverse o[i] = o[n-1-i] is refuted") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function rev(o,n){ for(let i=0;i<n;i++){ o[i]=o[n-1-i]; } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK((hasHazard(rep, "RAW") || hasHazard(rep, "WAR")));
    }

    SUBCASE("a copy of the induction variable carries the dependency") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ for(let i=0;i<n;i++){ const t=i; o[t+1]=o[t]; } }");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        CHECK_FALSE(rep.provenNoRaw);
        REQUIRE(hasHazard(rep, "RAW"));
        for (const auto& h : rep.hazards) {
            if (h.hazardKind != "RAW") continue;
            CHECK(h.arrayName == "o");
            CHECK(h.iterB == h.iterA + 1);
        }
    }

    SUBCASE("a copy of the induction variable with no hazard is parallel") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){ for(let i=0;i<n;i++){ const t = 2 * i; o[t] = a[i]; o[t + 1] = a[i]; } }");
        CHECK(rep.parallelizable);
    }

    SUBCASE("a reassigned local is not an affine index") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){ for(let i=0;i<n;i++){ let t=i; t = t + 1; o[t]=a[i]; } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK(hasHazard(rep, "NON_AFFINE"));
    }

    SUBCASE("o[idx[i]] is a store whose index is not affine") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function scatter(a,o,idx,n){ for(let i=0;i<n;i++){ o[idx[i]]=a[i]; } }");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        REQUIRE(!rep.hazards.empty());
        CHECK(rep.hazards[0].hazardKind == "NON_AFFINE");
        CHECK(rep.hazards[0].arrayName == "o");
        CHECK(rep.hazards[0].writeIndexExpr == "idx[i]");
        CHECK(contains(rep.summary, "Non-affine write index `idx[i]`"));
        CHECK(rep.kernelSource.empty());
    }

    SUBCASE("a bound of `o.length` checks indices against it") {
        auto ok = analyzeAndRewriteLoopToKernel(
            "function k(a,o){ for(let i=0;i<o.length;i++){ o[i]=a[i]; } }");
        CHECK(ok.parallelizable);
        CHECK(contains(ok.kernelSource, "if (i < o.length) {"));
        auto past = analyzeAndRewriteLoopToKernel(
            "function k(a,o){ for(let i=0;i<o.length;i++){ o[i+1]=a[i]; } }");
        CHECK_FALSE(past.parallelizable);
        CHECK(hasHazard(past, "OUT_OF_BOUNDS"));
    }

    SUBCASE("`<=` covers the last iteration") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){ for(let i=0;i<=n;i++){ o[i]=a[i]; } }");
        CHECK(rep.parallelizable);
        CHECK(contains(rep.kernelSource, "if (i <= n) {"));
        auto war = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ for(let i=0;i<=n;i++){ o[i]=o[i+1]; } }");
        CHECK_FALSE(war.parallelizable);
        CHECK(hasHazard(war, "WAR"));
        CHECK_FALSE(hasHazard(war, "RAW"));
    }
}

TEST_CASE("auto_kernel (#161): effects are rejected from the AST, not from substrings") {
    SUBCASE("a call") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n,f){ for(let i=0;i<n;i++){ o[i]=f(a[i]); } }");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        REQUIRE(!rep.hazards.empty());
        CHECK(rep.hazards[0].hazardKind == "SIDE_EFFECT");
        CHECK(contains(rep.summary, "calls `f`"));
    }

    SUBCASE("an impure Math call") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ for(let i=0;i<n;i++){ o[i]=Math.random(); } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK(hasHazard(rep, "SIDE_EFFECT"));
    }

    SUBCASE("a shadowed Math") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n,Math){ for(let i=0;i<n;i++){ o[i]=Math.abs(i); } }");
        CHECK_FALSE(rep.parallelizable);
    }

    SUBCASE("`new`, `throw` and a method push inside strings are not effects") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ for(let i=0;i<n;i++){ const s = \"new throw .push(\"; o[i]=i; } }");
        CHECK(rep.parallelizable);
    }

    SUBCASE("`new`, `throw`, `return`, `break`") {
        for (const char* body : {"const t = new Float32Array(1); o[i] = i;", "if (i > 3) throw 1; o[i] = i;",
                                 "if (i > 3) return; o[i] = i;", "if (i > 3) break; o[i] = i;",
                                 "if (i > 3) continue; o[i] = i;"}) {
            auto rep = analyzeAndRewriteLoopToKernel(
                std::string("function k(o,n){ for(let i=0;i<n;i++){ ") + body + " } }");
            CAPTURE(body);
            CHECK(rep.foundLoop);
            CHECK_FALSE(rep.parallelizable);
            CHECK(hasHazard(rep, "SIDE_EFFECT"));
        }
    }

    SUBCASE("a break inside a nested loop stays in the body") {
        auto rep = analyzeAndRewriteLoopToKernel(R"(
            function k(a, o, n) {
                for (let i = 0; i < n; i++) {
                    let s = 0;
                    for (let j = 0; j < 4; j++) { if (j > a[i]) break; s += j; }
                    o[i] = s;
                }
            }
        )");
        CHECK(rep.parallelizable);
    }

    SUBCASE("property writes and writes to outer bindings") {
        auto prop = analyzeAndRewriteLoopToKernel(
            "function k(o,n,st){ for(let i=0;i<n;i++){ st.count = i; o[i] = i; } }");
        CHECK_FALSE(prop.parallelizable);
        CHECK(hasHazard(prop, "SIDE_EFFECT"));
        auto outer = analyzeAndRewriteLoopToKernel(
            "function k(a,n){ let s = 0; for(let i=0;i<n;i++){ s += a[i]; a[i] = s; } return s; }");
        CHECK_FALSE(outer.parallelizable);
        CHECK(hasHazard(outer, "SIDE_EFFECT"));
        auto param = analyzeAndRewriteLoopToKernel(
            "function k(a,n,m){ for(let i=0;i<n;i++){ m = a[i]; a[i] = m; } }");
        CHECK_FALSE(param.parallelizable);
    }

    SUBCASE("assigning the induction variable") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ for(let i=0;i<n;i++){ o[i] = 1; i++; } }");
        CHECK(rep.foundLoop);
        CHECK_FALSE(rep.parallelizable);
        CHECK(contains(rep.summary, "induction variable"));
    }

    SUBCASE("reading an outer local the kernel would not have") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,n){ const c = 3; for(let i=0;i<n;i++){ o[i] = c; } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK(contains(rep.summary, "`c`"));
    }

    SUBCASE("a negative start cannot be a thread index") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(o,lo,n){ for(let i=lo;i<n;i++){ o[i - lo] = 1; } }");
        CHECK_FALSE(rep.parallelizable);
        CHECK(hasHazard(rep, "UNSUPPORTED"));
    }
}

TEST_CASE("auto_kernel: aliasing between array names is proven, not assumed") {
    SUBCASE("same-index map holds even if the arrays are one array") {
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){for(let i=0;i<n;i++){ o[i]=a[i]*2; }}");
        CHECK(rep.parallelizable);
        CHECK(rep.requiresDistinct.empty());
        CHECK(contains(rep.summary, "even if the arrays are one array"));
    }

    SUBCASE("a shifted read needs the arrays to differ, and says so") {
        // k(x, x, n): iteration i writes x[i], iteration i+1 reads it.
        auto rep = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){for(let i=0;i<n;i++){ o[i]=a[i+1]; }}");
        CHECK(rep.parallelizable);
        REQUIRE(rep.requiresDistinct.size() == 1);
        CHECK(rep.requiresDistinct[0] == std::make_pair(std::string("a"), std::string("o")));
        CHECK(contains(rep.summary, "only if these are different arrays"));
        CHECK(contains(rep.toJson(), "\"requiresDistinct\": [[\"a\", \"o\"]]"));
    }

    SUBCASE("two outputs written at one index alias safely, a reversed one does not") {
        auto same = analyzeAndRewriteLoopToKernel(
            "function k(a,b,n){for(let i=0;i<n;i++){ a[i]=1; b[i]=2; }}");
        CHECK(same.requiresDistinct.empty());
        auto rev = analyzeAndRewriteLoopToKernel(
            "function k(a,o,n){for(let i=0;i<n;i++){ o[i]=a[n-1-i]; }}");
        CHECK(rev.parallelizable);
        CHECK(rev.requiresDistinct.size() == 1);
    }
}
