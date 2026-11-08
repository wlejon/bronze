#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "check/il_interp.h"
#include "check/prove_equiv.h"

using bronze::check::EquivStatus;
using bronze::check::ProveEquivReport;

namespace {

struct GeneratedPair {
    std::string oldSrc;
    std::string newSrc;
    uint32_t arity = 2;
    bool expectEquivalent = false;
    std::string label;
};

std::string randomExpr(std::mt19937_64& rng, int depth, bool allowDiv) {
    if (depth <= 0) {
        std::uniform_int_distribution<int> leafDist(0, 5);
        switch (leafDist(rng)) {
            case 0: return "(a | 0)";
            case 1: return "(b | 0)";
            case 2: return "1";
            case 3: return "-1";
            case 4: return "7";
            default: return "16";
        }
    }
    std::uniform_int_distribution<int> opDist(0, allowDiv ? 8 : 7);
    int choice = opDist(rng);
    std::string lhs = randomExpr(rng, depth - 1, false);
    std::string rhs = randomExpr(rng, depth - 1, false);
    switch (choice) {
        case 0: return "((" + lhs + " + " + rhs + ") | 0)";
        case 1: return "((" + lhs + " - " + rhs + ") | 0)";
        case 2: return "(" + lhs + " & " + rhs + ")";
        case 3: return "(" + lhs + " | " + rhs + ")";
        case 4: return "(" + lhs + " ^ " + rhs + ")";
        case 5: return "(" + lhs + " << (" + rhs + " & 7))";
        case 6: return "(" + lhs + " >> (" + rhs + " & 7))";
        case 7: return "(" + lhs + " >>> (" + rhs + " & 7))";
        default: return "((" + lhs + " / 3) | 0)";
    }
}

GeneratedPair generateFuzzPair(std::mt19937_64& rng, size_t idx) {
    GeneratedPair out;
    out.arity = 2;
    uint32_t kind = static_cast<uint32_t>(idx % 8);

    std::string baseExpr = randomExpr(rng, 2, true);
    switch (kind) {
        case 0: {
            // Equivalent bitwise/arithmetic identity rewrite
            out.oldSrc = "export function f(a, b) { const x = " + baseExpr + "; return ((x & b) + (x | b)) | 0; }";
            out.newSrc = "export function f(a, b) { const x = " + baseExpr + "; return (x + (b | 0)) | 0; }";
            out.expectEquivalent = true;
            out.label = "bitwise_add_identity";
            break;
        }
        case 1: {
            // Equivalent throw-guard + shift rewrite
            out.oldSrc = "export function f(a, b) { if ((a | 0) < 0) throw new RangeError('neg'); return ((" +
                         baseExpr + ") * 2) | 0; }";
            out.newSrc = "export function f(a, b) { if ((a | 0) < 0) throw new Error('neg'); return ((" +
                         baseExpr + ") << 1) | 0; }";
            out.expectEquivalent = true;
            out.label = "matched_throw_guard_shift";
            break;
        }
        case 2: {
            // Mutation: operator swap (+ vs -)
            out.oldSrc = "export function f(a, b) { return ((" + baseExpr + ") + (b | 0)) | 0; }";
            out.newSrc = "export function f(a, b) { return ((" + baseExpr + ") - (b | 0)) | 0; }";
            out.expectEquivalent = false;
            out.label = "mutation_op_swap";
            break;
        }
        case 3: {
            // Mutation: signed vs unsigned right shift (>> vs >>>)
            out.oldSrc = "export function f(a, b) { return (a | 0) >> ((b & 7) + 1); }";
            out.newSrc = "export function f(a, b) { return (a | 0) >>> ((b & 7) + 1); }";
            out.expectEquivalent = false;
            out.label = "mutation_sign_vs_zero_shift";
            break;
        }
        case 4: {
            // Mutation: off-by-one branch bound (< vs <=)
            int32_t thresh = static_cast<int32_t>(idx % 17) - 8;
            std::string tStr = std::to_string(thresh);
            out.oldSrc = "export function f(a, b) { if ((a | 0) < " + tStr + ") return " + baseExpr + "; return b | 0; }";
            out.newSrc = "export function f(a, b) { if ((a | 0) <= " + tStr + ") return " + baseExpr + "; return b | 0; }";
            out.expectEquivalent = false;
            out.label = "mutation_off_by_one_branch";
            break;
        }
        case 5: {
            // Mutation: throw insertion on one side only
            out.oldSrc = "export function f(a, b) { if ((a | 0) < 0) throw new Error('bad'); return " + baseExpr + "; }";
            out.newSrc = "export function f(a, b) { return " + baseExpr + "; }";
            out.expectEquivalent = false;
            out.label = "mutation_one_sided_throw";
            break;
        }
        case 6: {
            // Mutation: off-by-one loop bound (< n vs <= n)
            out.oldSrc = "export function f(a, b) { let s = 0; const n = (a & 3); for (let i = 0; i < n; i = (i + 1) | 0) s = (s + (b | 0) + 1) | 0; return s; }";
            out.newSrc = "export function f(a, b) { let s = 0; const n = (a & 3); for (let i = 0; i <= n; i = (i + 1) | 0) s = (s + (b | 0) + 1) | 0; return s; }";
            out.expectEquivalent = false;
            out.label = "mutation_off_by_one_loop";
            break;
        }
        default: {
            // Mutation: sign negation (-x vs ~x)
            out.oldSrc = "export function f(a, b) { const x = " + baseExpr + "; return (-x) | 0; }";
            out.newSrc = "export function f(a, b) { const x = " + baseExpr + "; return (~x) | 0; }";
            out.expectEquivalent = false;
            out.label = "mutation_neg_vs_bitnot";
            break;
        }
    }
    return out;
}

void verifyPairAgainstConcreteExecution(const GeneratedPair& pair, std::mt19937_64& rng) {
    bronze::il::Module oldMod, newMod;
    std::string err;
    REQUIRE(bronze::check::lowerSingleSourceForEquiv(pair.oldSrc, "fuzz_old.js", oldMod, &err));
    REQUIRE(bronze::check::lowerSingleSourceForEquiv(pair.newSrc, "fuzz_new.js", newMod, &err));

    ProveEquivReport rep = bronze::check::proveModuleEquiv(oldMod, newMod, "f");
    REQUIRE(rep.functions.size() == 1);
    const auto& fnRes = rep.functions[0];

    const bronze::il::Function* oldFn = nullptr;
    const bronze::il::Function* newFn = nullptr;
    for (const auto& f : oldMod.functions) if (f.name == "f" || f.displayName == "f") oldFn = &f;
    for (const auto& f : newMod.functions) if (f.name == "f" || f.displayName == "f") newFn = &f;
    REQUIRE(oldFn != nullptr);
    REQUIRE(newFn != nullptr);

    if (pair.expectEquivalent) {
        CHECK_MESSAGE(fnRes.status == EquivStatus::Equivalent,
                      "Expected EQUIVALENT on " << pair.label << " (detail: " << fnRes.detail << "):\n"
                      << pair.oldSrc << "\nvs\n" << pair.newSrc);
    }

    if (fnRes.status == EquivStatus::Counterexample) {
        auto oldRun = bronze::check::runIlFunctionConcrete(oldMod, *oldFn, fnRes.paramNames, fnRes.counterexampleArgs);
        auto newRun = bronze::check::runIlFunctionConcrete(newMod, *newFn, fnRes.paramNames, fnRes.counterexampleArgs);
        REQUIRE(oldRun.completed);
        REQUIRE(newRun.completed);
        CHECK_MESSAGE(bronze::check::concreteResultsDiverge(oldRun, newRun),
                      "Counterexample did not diverge in concrete execution for " << pair.label);
    }

    if (fnRes.status == EquivStatus::Equivalent) {
        static constexpr int64_t kFixedInputs[] = {
            0, 1, -1, 2, -2, 3, -3, 7, -7, 8, -8, 15, 16, 31, 32, -32, 64, 127, -128, INT32_MAX, INT32_MIN
        };
        std::vector<std::string> pNames{"a", "b"};
        for (int64_t a : kFixedInputs) {
            for (int64_t b : kFixedInputs) {
                auto oldRun = bronze::check::runIlFunctionConcrete(oldMod, *oldFn, pNames, {a, b});
                auto newRun = bronze::check::runIlFunctionConcrete(newMod, *newFn, pNames, {a, b});
                REQUIRE(oldRun.completed);
                REQUIRE(newRun.completed);
                if (bronze::check::concreteResultsDiverge(oldRun, newRun)) {
                    FAIL("SOUNDNESS FAILURE: Prover reported EQUIVALENT but concrete execution diverged at a="
                         << a << ", b=" << b << " for:\n" << pair.oldSrc << "\nvs\n" << pair.newSrc);
                }
            }
        }
        std::uniform_int_distribution<int32_t> randDist(INT32_MIN, INT32_MAX);
        for (int r = 0; r < 24; ++r) {
            int64_t a = randDist(rng);
            int64_t b = randDist(rng);
            auto oldRun = bronze::check::runIlFunctionConcrete(oldMod, *oldFn, pNames, {a, b});
            auto newRun = bronze::check::runIlFunctionConcrete(newMod, *newFn, pNames, {a, b});
            REQUIRE(oldRun.completed);
            REQUIRE(newRun.completed);
            if (bronze::check::concreteResultsDiverge(oldRun, newRun)) {
                FAIL("SOUNDNESS FAILURE: Prover reported EQUIVALENT but random concrete execution diverged at a="
                     << a << ", b=" << b << " for:\n" << pair.oldSrc << "\nvs\n" << pair.newSrc);
            }
        }
    }
}

}  // namespace

TEST_CASE("prover_soundness: throw flag semantics, self-replay, and unknown op audit") {
    SUBCASE("one-sided throw is refuted with a counterexample, never reported EQUIVALENT") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function checked(n) {
                    if ((n | 0) < 0) throw new RangeError("negative");
                    return (n | 0) + 1;
                }
            )",
            R"(
                export function checked(n) {
                    return (n | 0) + 1;
                }
            )",
            "checked", &err);
        REQUIRE(err.empty());
        REQUIRE(rep.functions.size() == 1);
        CHECK(rep.functions[0].status == EquivStatus::Counterexample);
        REQUIRE(!rep.functions[0].counterexampleArgs.empty());
        CHECK(rep.functions[0].counterexampleArgs[0] < 0);
    }

    SUBCASE("inlined helper throw is carried through caller equivalence") {
        std::string err;
        ProveEquivReport eqRep = bronze::check::proveSourceEquiv(
            R"(
                function assertNonNeg(x) { if ((x | 0) < 0) throw new Error("neg"); }
                export function doublePos(x) {
                    assertNonNeg(x);
                    return ((x | 0) * 2) | 0;
                }
            )",
            R"(
                export function doublePos(x) {
                    if ((x | 0) < 0) throw new RangeError("neg");
                    return (x | 0) << 1;
                }
            )",
            "doublePos", &err);
        REQUIRE(err.empty());
        REQUIRE(eqRep.functions.size() == 1);
        CHECK(eqRep.functions[0].status == EquivStatus::Equivalent);

        ProveEquivReport neqRep = bronze::check::proveSourceEquiv(
            R"(
                function assertNonNeg(x) { if ((x | 0) < 0) throw new Error("neg"); }
                export function doublePos(x) {
                    assertNonNeg(x);
                    return (x | 0) * 2;
                }
            )",
            R"(
                export function doublePos(x) {
                    return (x | 0) << 1;
                }
            )",
            "doublePos", &err);
        REQUIRE(err.empty());
        REQUIRE(neqRep.functions.size() == 1);
        CHECK(neqRep.functions[0].status == EquivStatus::Counterexample);
    }

    SUBCASE("unhandled IL constructs produce UNKNOWN with a reason, never a guess") {
        std::string err;
        // 1. instanceof on unknown parameter object
        ProveEquivReport instRep = bronze::check::proveSourceEquiv(
            R"(
                function Widget() {}
                export function checkInst(x) {
                    return (x instanceof Widget) ? 1 : 0;
                }
            )",
            R"(
                export function checkInst(x) {
                    return 1;
                }
            )",
            "checkInst", &err);
        REQUIRE(err.empty());
        REQUIRE(instRep.functions.size() == 1);
        CHECK(instRep.functions[0].status == EquivStatus::Unknown);
        CHECK(!instRep.functions[0].detail.empty());

        // 2. parseInt with radix on symbolic input
        ProveEquivReport parseRep = bronze::check::proveSourceEquiv(
            R"(
                export function parseHex(s) {
                    return parseInt(s, 16);
                }
            )",
            R"(
                export function parseHex(s) {
                    return s;
                }
            )",
            "parseHex", &err);
        REQUIRE(err.empty());
        REQUIRE(parseRep.functions.size() == 1);
        CHECK(parseRep.functions[0].status == EquivStatus::Unknown);

        // 3. Math.sin transcendental function
        ProveEquivReport sinRep = bronze::check::proveSourceEquiv(
            R"(
                export function trig(x) { return Math.sin(x); }
            )",
            R"(
                export function trig(x) { return x; }
            )",
            "trig", &err);
        REQUIRE(err.empty());
        REQUIRE(sinRep.functions.size() == 1);
        CHECK(sinRep.functions[0].status == EquivStatus::Unknown);
    }
}

TEST_CASE("prover_fuzzer: deterministic mutation and concrete execution oracle") {
    std::mt19937_64 rng(0xB902E5EED1234567ULL);
    const char* longEnv = std::getenv("BRONZE_FUZZ_LONG");
    const size_t numPairs = (longEnv && longEnv[0] != '\0' && longEnv[0] != '0') ? 1500 : 120;

    for (size_t i = 0; i < numPairs; ++i) {
        GeneratedPair pair = generateFuzzPair(rng, i);
        verifyPairAgainstConcreteExecution(pair, rng);
    }
}
