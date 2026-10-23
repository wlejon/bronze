#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "check/static_checker.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace {

bronze::check::CheckReport runStaticCheck(const char* src) {
    bronze::SourceSet sources;
    const auto& buf = sources.add("check_test.js", src);
    bronze::DiagnosticSink diags;
    auto tokens = bronze::Lexer(buf, diags).lex();
    REQUIRE_FALSE(diags.hasErrors());
    auto mod = bronze::Parser(std::move(tokens), diags).parseModule("check_test.js");
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod != nullptr);

    bronze::DiagnosticSink inferDiags;
    auto inferred = bronze::types::inferModule(*mod, inferDiags);
    REQUIRE(inferred.has_value());
    bronze::DiagnosticSink lowerDiags;
    auto ilMod = bronze::lower::lowerModule(*mod, lowerDiags, &*inferred, nullptr, &sources);
    return bronze::check::checkModule(*mod, &*inferred, ilMod ? &*ilMod : nullptr, nullptr);
}

}  // namespace

TEST_CASE("static checker: null and undefined dereference detection and narrowing") {
    SUBCASE("unguarded nullable DOM query reports null-deref") {
        auto report = runStaticCheck(R"(
            const el = document.getElementById("header");
            el.textContent = "title";
        )");
        CHECK(report.nullDerefCount == 1);
        CHECK(report.outOfBoundsCount == 0);
        CHECK(report.nanPoisonCount == 0);
    }

    SUBCASE("if(el) truthiness guard suppresses null-deref") {
        auto report = runStaticCheck(R"(
            const el = document.getElementById("header");
            if (el) {
                el.textContent = "title";
            }
        )");
        CHECK(report.empty());
    }

    SUBCASE("early return if(!el) dominates subsequent dereference") {
        auto report = runStaticCheck(R"(
            function updateHeader() {
                const el = document.querySelector(".header");
                if (!el) return;
                el.setAttribute("data-ready", "1");
            }
        )");
        CHECK(report.empty());
    }

    SUBCASE("optional chaining and logical-and short-circuit are safe") {
        auto report = runStaticCheck(R"(
            const el = document.getElementById("header");
            const txt = el?.textContent;
            const sub = el?.firstElementChild.tagName;
            const ok = el && el.getAttribute("id");
        )");
        CHECK(report.empty());
    }

    SUBCASE("parenthesized optional chain ends short-circuit and reports null-deref") {
        auto report = runStaticCheck(R"(
            const el = document.getElementById("header");
            const bad = (el?.firstElementChild).tagName;
        )");
        CHECK(report.nullDerefCount == 1);
    }

    SUBCASE("branch join with null reports null-deref unless guarded") {
        auto badReport = runStaticCheck(R"(
            function pick(flag) {
                const box = flag ? { v: 42 } : null;
                return box.v;
            }
        )");
        CHECK(badReport.nullDerefCount == 1);

        auto goodReport = runStaticCheck(R"(
            function pick(flag) {
                const box = flag ? { v: 42 } : null;
                if (box !== null) {
                    return box.v;
                }
                return 0;
            }
        )");
        CHECK(goodReport.empty());
    }
}

TEST_CASE("static checker: out-of-bounds array and TypedArray index detection") {
    SUBCASE("negative index and >= length constant index on array literal") {
        auto negReport = runStaticCheck(R"(
            const arr = [10, 20, 30];
            const x = arr[-1];
        )");
        CHECK(negReport.outOfBoundsCount == 1);

        auto oobReport = runStaticCheck(R"(
            const arr = [10, 20, 30];
            const x = arr[3];
        )");
        CHECK(oobReport.outOfBoundsCount == 1);

        auto validReport = runStaticCheck(R"(
            const arr = [10, 20, 30];
            const x = arr[0] + arr[2];
        )");
        CHECK(validReport.empty());
    }

    SUBCASE("TypedArray out-of-bounds read and write") {
        auto report = runStaticCheck(R"(
            const ta = new Float64Array(4);
            ta[0] = 1.0;
            ta[3] = 2.0;
            ta[4] = 3.0;
        )");
        CHECK(report.outOfBoundsCount == 1);
    }

    SUBCASE("arr[arr.length] and off-by-one <= loop bound") {
        auto lenIdxReport = runStaticCheck(R"(
            const arr = [1, 2, 3];
            const last = arr[arr.length];
        )");
        CHECK(lenIdxReport.outOfBoundsCount == 1);

        auto loopBadReport = runStaticCheck(R"(
            const arr = [1, 2, 3, 4];
            let sum = 0;
            for (let i = 0; i <= arr.length; ++i) {
                sum += arr[i];
            }
        )");
        CHECK(loopBadReport.outOfBoundsCount == 1);

        auto loopGoodReport = runStaticCheck(R"(
            const arr = [1, 2, 3, 4];
            let sum = 0;
            for (let i = 0; i < arr.length; ++i) {
                sum += arr[i];
            }
        )");
        CHECK(loopGoodReport.empty());

        auto loopGuardedReport = runStaticCheck(R"(
            const arr = [1, 2, 3, 4];
            let sum = 0;
            for (let i = 0; i <= arr.length; ++i) {
                if (i === arr.length) break;
                sum += arr[i];
            }
        )");
        CHECK(loopGuardedReport.empty());
    }
}

TEST_CASE("static checker: NaN poisoning hazard detection") {
    SUBCASE("uninitialized binding in numeric arithmetic") {
        auto report = runStaticCheck(R"(
            let x;
            const y = x + 10;
        )");
        CHECK(report.nanPoisonCount == 1);
    }

    SUBCASE("string concatenation with undefined is not flagged as NaN") {
        auto report = runStaticCheck(R"(
            let x;
            const s = "value: " + x;
        )");
        CHECK(report.nanPoisonCount == 0);
    }

    SUBCASE("Math domain error and 0/0 propagation into arithmetic and TypedArray store") {
        auto mathReport = runStaticCheck(R"(
            const a = Math.sqrt(-4);
            const b = Math.acos(2);
        )");
        CHECK(mathReport.nanPoisonCount == 2);

        auto divZeroReport = runStaticCheck(R"(
            const ratio = 0 / 0;
            const scaled = ratio * 100;
        )");
        CHECK(divZeroReport.nanPoisonCount == 1);

        auto taStoreReport = runStaticCheck(R"(
            const ta = new Float64Array(2);
            let uninit;
            ta[0] = uninit;
        )");
        CHECK(taStoreReport.nanPoisonCount == 1);
    }
}
