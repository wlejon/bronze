#include <doctest/doctest.h>

#include "check/cegis_superopt.h"
#include "check/js_source_rewriter.h"

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
        CHECK(rep.rewrittenSource.find("for (let __i = 0") != std::string::npos);
        // The chain survives only as the fallback for a receiver that is not a
        // plain Array, which the fused loop's semantics depend on (#162).
        const auto guard = rep.rewrittenSource.find(
            "if (!Array.isArray(items) || Object.getPrototypeOf(items) !== Array.prototype) return items.map(");
        REQUIRE(guard != std::string::npos);
        const auto loop = rep.rewrittenSource.find("for (let __i = 0");
        CHECK(rep.rewrittenSource.find(".map(", guard) < loop);
        CHECK(rep.rewrittenSource.find(".map(", loop) == std::string::npos);
        CHECK(rep.rewrittenSource.find(".filter(", loop) == std::string::npos);
        CHECK(rep.rewrittenSource.find(".reduce(", loop) == std::string::npos);
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

namespace {

// Rewrites with only the AST passes, so a site that must not fire cannot be
// masked by the CEGIS superoptimizer rewriting the function some other way.
bronze::check::JsRewriteReport rewriteSitesOnly(const char* src, bool requireProof = true) {
    JsRewriteOptions opts;
    opts.enableCegisSuperopt = false;
    opts.requireFormalProof = requireProof;
    return rewriteJsSource(src, opts);
}

bool hasRule(const bronze::check::JsRewriteReport& rep, const char* rule) {
    for (const auto& r : rep.rewrites) {
        if (r.ruleName == rule) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("js_source_rewriter (#162): sites are found in the parsed program, never in its text") {
    SUBCASE("the issue's example: the string keeps its text, the call is rewritten") {
        auto rep = rewriteJsSource(
            R"js(function f(x){ const s = "Math.pow(x, 2)"; return s + Math.pow(x, 2); })js");
        CHECK(rep.error.empty());
        CHECK(rep.rewrittenSource ==
              R"js(function f(x){ const s = "Math.pow(x, 2)"; return s + (x * x); })js");
        REQUIRE(rep.rewrites.size() == 1);
        CHECK(rep.rewrites[0].ruleName == "strength-reduction-pow");
        CHECK(rep.rewrites[0].functionName == "f");
    }

    SUBCASE("no pattern fires inside a string literal") {
        const char* src = R"(function f(x) {
            let u = x & 255;
            const s = "Math.max(0, x & 255) Math.min(255, x & 255) if (u < 0) return -1; "
                    + 'Math.floor((x & 255) / 8) Math.trunc((x & 255) / 8) Math.pow(u, 3) Math.imul(x, 8) '
                    + "u * u * u + 3 * (u * u) + 3 * u + 1 "
                    + "return items.map(v => v * 2).filter(w => w > 10).reduce((a, b) => a + b, 0);";
            return s.length + u;
        })";
        auto rep = rewriteSitesOnly(src);
        CHECK(rep.rewrites.empty());
        CHECK(rep.rewrittenSource == src);
    }

    SUBCASE("no pattern fires inside a comment") {
        const char* src = R"(function f(x) {
            let u = x & 255;
            // Math.max(0, x & 255) Math.min(255, x & 255) Math.pow(u, 2) Math.imul(x, 8)
            /* if (u < 0) return -1;  Math.floor((x & 255) / 8)  u * u * u + 3 * (u * u) + 3 * u + 1
               let s = 0; for (let i = 0; i < n; i++) { s += i; } return s; */
            return u;
        })";
        auto rep = rewriteSitesOnly(src);
        CHECK(rep.rewrites.empty());
        CHECK(rep.rewrittenSource == src);
    }

    SUBCASE("no pattern fires in a template literal's text; a real call in its substitution does") {
        const char* src =
            "function f(x) { const t = `Math.pow(x, 2) Math.max(0, x & 255) Math.imul(x, 8) ${Math.pow(x, 2)}`; "
            "return t; }";
        auto rep = rewriteSitesOnly(src, /*requireProof=*/false);
        REQUIRE(rep.rewrites.size() == 1);
        CHECK(rep.rewrittenSource ==
              "function f(x) { const t = `Math.pow(x, 2) Math.max(0, x & 255) Math.imul(x, 8) ${(x * x)}`; "
              "return t; }");
    }

    SUBCASE("a shadowed Math is not the global and nothing on it is rewritten") {
        const char* local = R"(function f(x) {
            const Math = { pow: (a, b) => 0, max: (a, b) => 0, min: (a, b) => 0, floor: (a) => 0, imul: (a, b) => 0 };
            return Math.pow(x, 2) + Math.max(0, x & 255) + Math.min(255, x & 255) +
                   Math.floor((x & 255) / 8) + Math.imul(x, 8);
        })";
        auto rep = rewriteSitesOnly(local, /*requireProof=*/false);
        CHECK(rep.rewrites.empty());
        CHECK(rep.rewrittenSource == local);

        const char* param = "function f(Math, x) { return Math.pow(x, 2) + Math.imul(x, 8); }";
        rep = rewriteSitesOnly(param, false);
        CHECK(rep.rewrites.empty());

        const char* patched = "Math.pow = (a, b) => 0;\nfunction f(x) { return Math.pow(x, 2); }";
        rep = rewriteSitesOnly(patched, false);
        CHECK(rep.rewrites.empty());
    }

    SUBCASE("the masked variable's facts hold only for that binding, unassigned") {
        // Reassigned before the `if`: the mask says nothing about the new value.
        const char* reassigned = R"(function f(x) {
            let u = x & 255;
            u = x;
            if (u < 0) return -1;
            return u;
        })";
        auto rep = rewriteSitesOnly(reassigned, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));
        CHECK(rep.rewrittenSource == reassigned);

        // Updated in place anywhere in the program, even after the `if`.
        const char* updated = R"(function f(x) {
            let u = x & 255;
            if (u > 255) return 999;
            u++;
            return u;
        })";
        rep = rewriteSitesOnly(updated, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));

        // A different binding of the same name in an inner block.
        const char* shadowed = R"(function f(x) {
            const u = x & 255;
            { const u = x; if (u < 0) return -1; }
            return u;
        })";
        rep = rewriteSitesOnly(shadowed, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));

        // Out of the declaring block's scope: the `u` there is another binding.
        const char* outOfScope = R"(function f(x) {
            { const u = x & 255; }
            if (u < 0) return -1;
            return 0;
        })";
        rep = rewriteSitesOnly(outOfScope, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));

        // Read inside a nested function, which may run before the initializer.
        const char* nested = R"(function f(x) {
            const u = x & 255;
            function g() { if (u < 0) return -1; return 0; }
            return g();
        })";
        rep = rewriteSitesOnly(nested, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));

        // A mask that is not a non-negative int32 bounds nothing.
        const char* signBit = R"(function f(x) {
            const u = x & 0x80000000;
            if (u < 0) return -1;
            return Math.max(0, x & 0x80000000);
        })";
        rep = rewriteSitesOnly(signBit, false);
        CHECK(rep.rewrites.empty());

        // The positive case, for contrast: a const binding, used later in its list.
        const char* sound = R"(function f(x) {
            const u = x & 255;
            if (u < 0) return -1;
            if (u > 300) { return 999; }
            if (u > 200) return 7;
            return u;
        })";
        rep = rewriteSitesOnly(sound);
        CHECK(rep.allProvenEquivalent);
        REQUIRE(rep.rewrites.size() == 2);
        CHECK(rep.rewrittenSource.find("u < 0") == std::string::npos);
        CHECK(rep.rewrittenSource.find("u > 300") == std::string::npos);
        CHECK(rep.rewrittenSource.find("if (u > 200) return 7;") != std::string::npos);
    }

    SUBCASE("a dead branch with an else, or holding a var, is left alone") {
        const char* src = R"(function f(x) {
            const u = x & 255;
            if (u < 0) return -1; else return u;
        })";
        auto rep = rewriteSitesOnly(src, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));

        const char* hoists = R"(function f(x) {
            const u = x & 255;
            if (u < 0) { var w = 1; }
            return w === undefined ? u : 0;
        })";
        rep = rewriteSitesOnly(hoists, false);
        CHECK_FALSE(hasRule(rep, "smt-dead-branch-elim"));
    }

    SUBCASE("division by 2^k needs an int32 operand; trunc also needs it non-negative") {
        auto rep = rewriteSitesOnly("function f(x) { return Math.floor(x / 4); }", false);
        CHECK(rep.rewrites.empty());
        rep = rewriteSitesOnly("function f(x) { return Math.trunc((x | 0) / 4); }", false);
        CHECK(rep.rewrites.empty());
        rep = rewriteSitesOnly("function f(x) { return Math.floor((x & 4095) / 4) + Math.trunc((x & 1023) / 4); }");
        INFO(rep.summary);
        CHECK(rep.allProvenEquivalent);
        CHECK(rep.rewrittenSource == "function f(x) { return ((x & 4095) >> 2) + ((x & 1023) >> 2); }");
        // floor(-1 / 4) is -1 and so is -1 >> 2: the site is sound for any int32.
        // (The SMT gate models `/` as truncating division and refuses it; that
        // is the gate being conservative, so this one is checked without it.)
        rep = rewriteSitesOnly("function f(x) { return Math.floor((x | 0) / 4); }", false);
        CHECK(rep.rewrittenSource == "function f(x) { return ((x | 0) >> 2); }");
    }

    SUBCASE("Horner's form needs an integer bound that keeps every product exact") {
        auto rep = rewriteSitesOnly(
            "function h(x) { return x * x * x + 3 * (x * x) + 3 * x + 1; }", false);
        CHECK_FALSE(hasRule(rep, "horner-polynomial"));
    }

    SUBCASE("deforestation needs callbacks that only compute") {
        const char* effectful = R"(function g(items) {
            return items.map(x => log(x)).filter(y => y > 10).reduce((acc, z) => acc + z, 0);
        })";
        auto rep = rewriteSitesOnly(effectful, false);
        CHECK(rep.rewrites.empty());

        const char* readsOuter = R"(function g(items, k) {
            return items.map(x => x * 2).filter(y => y > k).reduce((acc, z) => acc + z, 0);
        })";
        rep = rewriteSitesOnly(readsOuter, false);
        CHECK(rep.rewrites.empty());
    }

    SUBCASE("a replacement after an ASI-ended statement cannot turn into a call") {
        const char* src = "function f(x) {\n    let a = x\n    Math.pow(x, 2)\n    return a;\n}";
        auto rep = rewriteSitesOnly(src, false);
        REQUIRE(rep.rewrites.size() == 1);
        CHECK(rep.rewrittenSource == "function f(x) {\n    let a = x\n    ;(x * x)\n    return a;\n}");
    }
}
