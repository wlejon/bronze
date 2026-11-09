// The bro-facing verifiers (solve, prove, fuzz, optimize, superoptimize,
// layout) must never answer wrongly: a construct the model does not have is
// refused by name, and one it can model cheaply is modelled. Each case here
// returned a wrong answer without an error before.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "check/cegis_optimize.h"
#include "check/cegis_superopt.h"
#include "check/fuzz_fn.h"
#include "check/layout_solver.h"
#include "check/prove_equiv.h"
#include "check/prove_fn.h"
#include "check/solve_fn.h"
#include "support/sha256.h"

namespace {

using bronze::check::ConcreteResultKind;
using bronze::check::EquivStatus;
using bronze::check::LayoutBoxSpec;
using bronze::check::LayoutSolution;
using bronze::check::LayoutSpec;
using bronze::check::ProofCertificateReport;
using bronze::check::ProveOptions;
using bronze::check::SolveOptions;
using bronze::check::SolveResult;
using bronze::check::SolveStatus;

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

SolveResult solveRefused(const char* src, const SolveOptions& opts = {}) {
    SolveResult res = bronze::check::solveJsFunction(src, opts);
    CHECK(res.status == SolveStatus::Error);
    CHECK(contains(res.error, "function not symbolically decidable: "));
    return res;
}

}  // namespace

TEST_CASE("solve refuses string operations on its int32 parameters") {
    // String(0).length is 1, so `=== 2` was answered with x = 0.
    CHECK(contains(solveRefused("(x) => String(x).length === 2").error, "string conversion of a number"));
    CHECK(contains(solveRefused("(x) => `${x}`.length === 2").error, "string conversion of a number"));
    CHECK(contains(solveRefused("(x) => ('a' + x).length === 2").error, "string concatenation with a number"));
    CHECK(contains(solveRefused("(x) => ('a' + x) === 'a1'").error, "string concatenation with a number"));
    // `.length` of a number is undefined; the heap model gave it a free unknown.
    SolveResult len = solveRefused("(x, y) => y.length === 3 && x === 1");
    CHECK(contains(len.error, "parameter y is read as a string, array or object (y.length)"));
    SolveOptions bounded;
    bounded.domainMin = 0;
    bounded.domainMax = 100;
    solveRefused("(x) => String(x).length === 1 && x > 9", bounded);

    // Strings the function builds from literals are modelled, and stay solvable.
    SolveResult lit = bronze::check::solveJsFunction("(x) => ('ab' + 'c').length === x");
    REQUIRE(lit.sat());
    CHECK(lit.args[0] == 3);
}

TEST_CASE("free variables are refused by name, never made unknowns") {
    // `const k = 4; x => x === k` was unsat: k was an unconstrained token.
    SolveResult res = solveRefused("(x) => x === k");
    CHECK(contains(res.error, "free variable k: the function's source is analysed, not its closure"));
    CHECK(contains(solveRefused("(x) => typeof k === 'number' && x === 1").error, "free variable k"));

    ProveOptions eq;
    eq.equivalentToSource = "(x) => x + 3";
    ProofCertificateReport p = bronze::check::proveJsFunction("(x) => x + k", eq);
    CHECK(contains(p.error, "free variable k"));
    ProveOptions eqFree;
    eqFree.equivalentToSource = "(x) => x + k";
    CHECK(contains(bronze::check::proveJsFunction("(x) => x + 3", eqFree).error,
                   "equivTo function: free variable k"));

    CHECK(contains(bronze::check::fuzzJsFunction("(x) => x > k ? 1 : 0").error, "free variable k"));

    bronze::check::OptimizeResult opt = bronze::check::optimizeJsFunction("(x) => x * k");
    CHECK_FALSE(opt.verified);
    CHECK(contains(opt.summary, "free variable k"));
    bronze::check::OptimizeOptions withCand;
    withCand.candidates = {"(x) => x + k"};
    bronze::check::OptimizeResult optCand = bronze::check::optimizeJsFunction("(x) => x + 3", withCand);
    CHECK(optCand.optimizedSource.find("k") == std::string::npos);

    bronze::check::CegisSuperoptResult so = bronze::check::superoptimizeJsFunction("(x) => x * k");
    CHECK_FALSE(so.verified);
    CHECK(contains(so.summary, "free variable k"));

    auto eqRep = bronze::check::proveSourceEquivalent("export function f(x) { return x + k; }",
                                                      "export function f(x) { return x + 3; }", "f");
    CHECK(eqRep.status == EquivStatus::Unknown);
    CHECK(contains(eqRep.detail, "free variable k"));
}

TEST_CASE("solve compares the return against a target at full width") {
    // x >>> 0 reaches 4294967295; the target used to be truncated to int32.
    SolveOptions opts;
    opts.matchReturnValue = true;
    opts.targetReturn = 4294967295LL;
    SolveResult ushr = bronze::check::solveJsFunction("(x) => x >>> 0", opts);
    REQUIRE(ushr.sat());
    CHECK(ushr.args[0] == -1);
    CHECK(ushr.returnValue == 4294967295LL);

    opts.targetReturn = 3000000000LL;
    CHECK(bronze::check::solveJsFunction("(x) => x", opts).unsat());
    SolveResult twice = bronze::check::solveJsFunction("(x) => x * 2", opts);
    REQUIRE(twice.sat());
    CHECK(twice.args[0] == 1500000000);
}

TEST_CASE("layout predicate parameters bind by name") {
    LayoutSpec spec;
    spec.containerW = 100;
    spec.containerH = 50;
    LayoutBoxSpec a;
    a.id = "a";
    a.minW = a.maxW = 20;
    a.minH = a.maxH = 20;
    LayoutBoxSpec b = a;
    b.id = "b";
    spec.boxes = {a, b};

    // The second parameter was bound to a.y, its position, not to b.x.
    spec.predicateFnSource = "(a_x, b_x) => b_x === a_x + 30";
    LayoutSolution sol = bronze::check::solveLayoutSpec(spec);
    REQUIRE(sol.sat());
    CHECK(sol.boxes.at("b").x == sol.boxes.at("a").x + 30);

    spec.predicateFnSource = "(b_y, a_x) => b_y === 25 && a_x === 7";
    sol = bronze::check::solveLayoutSpec(spec);
    REQUIRE(sol.sat());
    CHECK(sol.boxes.at("b").y == 25);
    CHECK(sol.boxes.at("a").x == 7);

    spec.predicateFnSource = "(a_x, c_x) => c_x === a_x";
    sol = bronze::check::solveLayoutSpec(spec);
    CHECK(sol.status == SolveStatus::Error);
    CHECK(contains(sol.error, "predicate parameter 'c_x' names no box variable"));

    spec.predicateFnSource = "(a_z) => a_z === 0";
    CHECK(bronze::check::solveLayoutSpec(spec).status == SolveStatus::Error);

    spec.predicateFnSource = "(a_x) => a_x === gap";
    sol = bronze::check::solveLayoutSpec(spec);
    CHECK(sol.status == SolveStatus::Error);
    CHECK(contains(sol.error, "free variable gap"));
}

TEST_CASE("equivalence tells booleans, numbers and undefined apart") {
    auto prove = [](const char* a, const char* b) {
        return bronze::check::proveSourceEquivalent(a, b, "f");
    };
    auto boolVsNum = prove("export function f(x) { return x > 3; }",
                           "export function f(x) { return x > 3 ? 1 : 0; }");
    CHECK(boolVsNum.status == EquivStatus::Counterexample);
    CHECK(contains(boolVsNum.detail, "returns a boolean and the new one a number"));

    CHECK(prove("export function f(x) { return x > 3; }",
                "export function f(x) { return !(x <= 3); }").status == EquivStatus::Equivalent);
    CHECK(prove("export function f(x) { if (x > 3) return true; return x < -2; }",
                "export function f(x) { return x > 3 || x < -2; }").status == EquivStatus::Equivalent);
    // Booleanness on one path only: still a counterexample, at an input of that path.
    auto mixed = prove("export function f(x) { return x > 3 ? true : 0; }",
                       "export function f(x) { return x > 3 ? 1 : 0; }");
    REQUIRE(mixed.status == EquivStatus::Counterexample);
    CHECK(mixed.counterexampleArgs[0] > 3);
    // A missing return is undefined, not 0.
    auto undef = prove("export function f(x) { if (x > 0) return 1; }",
                       "export function f(x) { return x > 0 ? 1 : 0; }");
    REQUIRE(undef.status == EquivStatus::Counterexample);
    CHECK(undef.counterexampleArgs[0] <= 0);
    // Boolean(x) is ToBoolean, not the number passed through.
    CHECK(prove("export function f(x) { return Boolean(x); }",
                "export function f(x) { return x !== 0; }").status == EquivStatus::Equivalent);

    ProveOptions eq;
    eq.equivalentToSource = "(x) => x > 3 ? 1 : 0";
    eq.synthesizeTests = false;
    ProofCertificateReport rep = bronze::check::proveJsFunction("(x) => x > 3", eq);
    CHECK_FALSE(rep.equivalent);
    CHECK_FALSE(rep.verified);
    CHECK(contains(rep.equivalenceDetail, "boolean"));
}

TEST_CASE("evalConcreteJsFunction reports the result's kind and the parameter count") {
    int64_t v = 0;
    ConcreteResultKind kind = ConcreteResultKind::Number;
    size_t params = 0;
    REQUIRE(bronze::check::evalConcreteJsFunction("(x, y) => x > y", {5, 3}, &v, nullptr, "", &kind, &params));
    CHECK(v == 1);
    CHECK(kind == ConcreteResultKind::Boolean);
    CHECK(params == 2);
    REQUIRE(bronze::check::evalConcreteJsFunction("(x) => x > 3 ? 1 : 0", {5}, &v, nullptr, "", &kind));
    CHECK(kind == ConcreteResultKind::Number);
    REQUIRE(bronze::check::evalConcreteJsFunction("(x) => { if (x > 0) return 1; }", {-1}, &v, nullptr, "", &kind));
    CHECK(kind == ConcreteResultKind::Nullish);
    // A parameter read as a string is no int32 function: the caller runs the original.
    CHECK_FALSE(bronze::check::evalConcreteJsFunction("(s) => s.length", {4}, &v));
}

TEST_CASE("prove's sha256 is the SHA-256 of the normalized source") {
    ProofCertificateReport rep = bronze::check::proveJsFunction("(x) => x");
    CHECK(rep.sha256 == bronze::support::sha256Hex(
                            bronze::check::normalizeJsFunctionSource("(x) => x", nullptr, "__prove_target")));
}
