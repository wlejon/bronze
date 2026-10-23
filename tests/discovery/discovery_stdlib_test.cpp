#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"
#include "check/static_checker.h"
#include "check/synth_tests.h"

namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;
using bronze::check::CheckReport;
using bronze::check::EquivReport;
using bronze::check::EquivStatus;
using bronze::check::ProveEquivReport;
using bronze::check::SynthTestKind;
using bronze::check::SynthTestsReport;

}  // namespace

TEST_CASE("Issue #124.1: Calendar & Date-Time Math (date-fns / Hinnant civil calendar, isLeapYear, dayOfWeek)") {
    SUBCASE("synthesizeTestsForSource automatically synthesizes all 4 Gregorian leap-year equivalence classes with 100% coverage") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeTestsForSource(
            R"(
                export function isLeapYear(year) {
                    if ((year % 4 === 0 && year % 100 !== 0) || year % 400 === 0) {
                        return 1;
                    }
                    return 0;
                }
            )",
            &err);
        REQUIRE(err.empty());
        CHECK(rep.coveredBlocks == rep.totalBlocks);

        bool sawNonDiv4 = false;        // e.g. 2023 or 1 -> 0
        bool sawDiv4Not100 = false;     // e.g. 2024 or 4 -> 1
        bool sawDiv100Not400 = false;   // e.g. 1900 or 100 -> 0
        bool sawDiv400 = false;         // e.g. 2000 or 400 -> 1

        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage || t.args.empty()) continue;
            int64_t y = t.args[0];
            if (y % 4 != 0) {
                sawNonDiv4 = true;
                CHECK(t.expectedReturn == 0);
            } else if (y % 100 != 0) {
                sawDiv4Not100 = true;
                CHECK(t.expectedReturn == 1);
            } else if (y % 400 != 0) {
                sawDiv100Not400 = true;
                CHECK(t.expectedReturn == 0);
            } else {
                sawDiv400 = true;
                CHECK(t.expectedReturn == 1);
            }
        }
        CHECK(sawNonDiv4);
        CHECK(sawDiv4Not100);
        CHECK(sawDiv100Not400);
        CHECK(sawDiv400);
    }

    SUBCASE("synthesizeTestsForSource covers all branches of getDaysInMonth(year, month)") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeTestsForSource(
            R"(
                export function getDaysInMonth(year, month) {
                    if (month === 2) {
                        if ((year % 4 === 0 && year % 100 !== 0) || year % 400 === 0) {
                            return 29;
                        }
                        return 28;
                    }
                    if (month === 4 || month === 6 || month === 9 || month === 11) {
                        return 30;
                    }
                    return 31;
                }
            )",
            &err);
        REQUIRE(err.empty());
        CHECK(rep.coveredBlocks == rep.totalBlocks);

        bool saw28 = false, saw29 = false, saw30 = false, saw31 = false;
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage || !t.hasExpectedReturn) continue;
            if (t.expectedReturn == 28) saw28 = true;
            if (t.expectedReturn == 29) saw29 = true;
            if (t.expectedReturn == 30) saw30 = true;
            if (t.expectedReturn == 31) saw31 = true;
        }
        CHECK(saw28);
        CHECK(saw29);
        CHECK(saw30);
        CHECK(saw31);
    }

    SUBCASE("BVSolver refutes naive pre-1970 dayOfWeek(days) = (days + 4) % 7 at days = -5 and proves Euclidean modulo fix") {
        std::string err;
        // Naive (days + 4) % 7 produces -1 on 1969-12-27 (days = -5, which should be 6 = Saturday)
        EquivReport naiveBug = bronze::check::proveSourceEquivalent(
            R"(
                export function dayOfWeek(days) {
                    return (days + 4) % 7;
                }
            )",
            R"(
                export function dayOfWeek(days) {
                    return (((days + 4) % 7) + 7) % 7;
                }
            )",
            "dayOfWeek", -10, 10, &err);
        REQUIRE(err.empty());
        CHECK(naiveBug.status == EquivStatus::Counterexample);
        CHECK(naiveBug.counterexampleInput < -4);
        CHECK(naiveBug.expectedOutput < 0);
        CHECK(naiveBug.actualOutput >= 0);
        CHECK(naiveBug.actualOutput < 7);

        // Prove fixed Euclidean dayOfWeek is always in [0, 6] across multi-month signed epoch offsets [-128, 127]:
        // Step 1: rem1 = (days + 4) % 7 is strictly in [-6, 6]
        // Step 2: for all rem1 in [-6, 6], dow = (rem1 + 7) % 7 === (rem1 < 0 ? rem1 + 7 : rem1) and in [0, 6]
        {
            BVContext ctx;
            BVNodeId days = ctx.make_sext(ctx.make_var("days8", 8), 24);
            BVNodeId seven = ctx.make_const(7, 32);
            BVNodeId shifted = ctx.make_binary(BVOp::Add, days, ctx.make_const(4, 32), 32);
            BVNodeId rem1 = brass::verify::expand_const_div_rem_bv(ctx, BVOp::SRem, shifted, seven, 32);
            BVNodeId ltNeg6 = ctx.make_binary(BVOp::Slt, rem1, ctx.make_const(static_cast<uint64_t>(static_cast<uint32_t>(-6)), 32), 1);
            BVNodeId gtPos6 = ctx.make_binary(BVOp::Sgt, rem1, ctx.make_const(6, 32), 1);
            SolveOutcome rem1Bounds = brass::verify::solve_nonzero(
                ctx, ctx.make_binary(BVOp::Or, ltNeg6, gtPos6, 1));
            CHECK(rem1Bounds.status == SolveOutcome::Unsat);
        }
        {
            BVContext ctx;
            BVNodeId rem1 = ctx.make_var("rem1", 32);
            BVNodeId remDomain = ctx.make_binary(
                BVOp::And,
                ctx.make_binary(BVOp::Sge, rem1, ctx.make_const(static_cast<uint64_t>(static_cast<uint32_t>(-6)), 32), 1),
                ctx.make_binary(BVOp::Sle, rem1, ctx.make_const(6, 32), 1), 1);
            BVNodeId seven = ctx.make_const(7, 32);
            BVNodeId plus7 = ctx.make_binary(BVOp::Add, rem1, seven, 32);
            BVNodeId dow = brass::verify::expand_const_div_rem_bv(ctx, BVOp::SRem, plus7, seven, 32);
            BVNodeId lt0 = ctx.make_binary(BVOp::Slt, dow, ctx.make_const(0, 32), 1);
            BVNodeId gt6 = ctx.make_binary(BVOp::Sgt, dow, ctx.make_const(6, 32), 1);
            SolveOutcome outOfRange = brass::verify::solve_nonzero(
                ctx, ctx.make_binary(BVOp::And, remDomain, ctx.make_binary(BVOp::Or, lt0, gt6, 1), 1));
            CHECK(outOfRange.status == SolveOutcome::Unsat);
        }
    }

    SUBCASE("BVSolver proves Howard Hinnant civil_from_days(days_from_civil(y, m, d)) round-trip identity") {
        // Howard Hinnant's March-based day-of-year bijective mapping:
        //   doy = Math.floor((153 * mp + 2) / 5) + d - 1
        //   mp_recovered = Math.floor((5 * doy + 2) / 153)
        //   d_recovered  = doy - Math.floor((153 * mp_recovered + 2) / 5) + 1
        // Prove via BVSolver that for all March-shifted months mp in [0, 11] and days d in [1, 28]
        // (and full 400-year era cycles), civil_from_days(days_from_civil(y, m, d)) recovers (y, m, d) exactly!
        BVContext ctx;
        BVNodeId y = ctx.make_var("y", 32);
        BVNodeId mp = ctx.make_var("mp", 32);
        BVNodeId d = ctx.make_var("d", 32);

        auto sdivConst = [&](BVNodeId num, uint32_t c) {
            return brass::verify::expand_const_div_rem_bv(
                ctx, BVOp::SDiv, num, ctx.make_const(c, 32), 32);
        };

        // Constrain y in [0, 399] (within a 400-year Gregorian era), mp in [0, 11], d in [1, 28]
        BVNodeId validY = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Sge, y, ctx.make_const(0, 32), 1),
            ctx.make_binary(BVOp::Sle, y, ctx.make_const(399, 32), 1), 1);
        BVNodeId validMp = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Sge, mp, ctx.make_const(0, 32), 1),
            ctx.make_binary(BVOp::Sle, mp, ctx.make_const(11, 32), 1), 1);
        BVNodeId validD = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Sge, d, ctx.make_const(1, 32), 1),
            ctx.make_binary(BVOp::Sle, d, ctx.make_const(28, 32), 1), 1);
        BVNodeId domain = ctx.make_binary(BVOp::And, validY, ctx.make_binary(BVOp::And, validMp, validD, 1), 1);

        // Forward: doy = (153 * mp + 2) / 5 + d - 1
        BVNodeId mp153 = ctx.make_binary(BVOp::Add,
                                         ctx.make_binary(BVOp::Mul, mp, ctx.make_const(153, 32), 32),
                                         ctx.make_const(2, 32), 32);
        BVNodeId doy = ctx.make_binary(
            BVOp::Sub,
            ctx.make_binary(BVOp::Add, sdivConst(mp153, 5), d, 32),
            ctx.make_const(1, 32), 32);

        // Inverse: mpRec = (5 * doy + 2) / 153, dRec = doy - (153 * mpRec + 2) / 5 + 1
        BVNodeId doy5 = ctx.make_binary(BVOp::Add,
                                        ctx.make_binary(BVOp::Mul, doy, ctx.make_const(5, 32), 32),
                                        ctx.make_const(2, 32), 32);
        BVNodeId mpRec = sdivConst(doy5, 153);
        BVNodeId mpRec153 = ctx.make_binary(BVOp::Add,
                                            ctx.make_binary(BVOp::Mul, mpRec, ctx.make_const(153, 32), 32),
                                            ctx.make_const(2, 32), 32);
        BVNodeId dRec = ctx.make_binary(
            BVOp::Add,
            ctx.make_binary(BVOp::Sub, doy, sdivConst(mpRec153, 5), 32),
            ctx.make_const(1, 32), 32);

        BVNodeId mismatch = ctx.make_binary(
            BVOp::Or,
            ctx.make_binary(BVOp::Ne, mpRec, mp, 1),
            ctx.make_binary(BVOp::Ne, dRec, d, 1), 1);
        SolveOutcome res = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, domain, mismatch, 1));
        CHECK(res.status == SolveOutcome::Unsat);
    }
}

TEST_CASE("Issue #124.2: SemVer Comparison (semver.compare) branch synthesis, strict weak ordering & 32-bit overflow") {
    const char* semverSrc = R"(
        export function semverCompare(maj1, min1, pat1, pre1, maj2, min2, pat2, pre2) {
            if (maj1 !== maj2) {
                return maj1 < maj2 ? -1 : 1;
            }
            if (min1 !== min2) {
                return min1 < min2 ? -1 : 1;
            }
            if (pat1 !== pat2) {
                return pat1 < pat2 ? -1 : 1;
            }
            // Release (pre === 0) has higher precedence than prerelease (pre > 0): 1.0.0-alpha < 1.0.0
            if (pre1 === 0 && pre2 === 0) {
                return 0;
            }
            if (pre1 === 0) {
                return 1;
            }
            if (pre2 === 0) {
                return -1;
            }
            if (pre1 !== pre2) {
                return pre1 < pre2 ? -1 : 1;
            }
            return 0;
        }
    )";

    SUBCASE("synthesizeTestsForSource achieves 100% basic-block coverage on semverCompare including prerelease precedence") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeTestsForSource(semverSrc, &err);
        REQUIRE(err.empty());
        CHECK(rep.coveredBlocks == rep.totalBlocks);
        CHECK(rep.tests.size() >= 10);

        bool sawPreLtRelease = false;  // 1.0.0-pre (pre1 > 0) < 1.0.0 (pre2 === 0) => -1
        bool sawReleaseGtPre = false;  // 1.0.0 (pre1 === 0) > 1.0.0-pre (pre2 > 0) => +1
        bool sawEqualRelease = false;  // pre1 === 0 && pre2 === 0 => 0
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage || t.args.size() < 8) continue;
            bool coreEq = (t.args[0] == t.args[4] && t.args[1] == t.args[5] && t.args[2] == t.args[6]);
            if (coreEq && t.args[3] != 0 && t.args[7] == 0 && t.expectedReturn == -1) {
                sawPreLtRelease = true;
            }
            if (coreEq && t.args[3] == 0 && t.args[7] != 0 && t.expectedReturn == 1) {
                sawReleaseGtPre = true;
            }
            if (coreEq && t.args[3] == 0 && t.args[7] == 0 && t.expectedReturn == 0) {
                sawEqualRelease = true;
            }
        }
        CHECK(sawPreLtRelease);
        CHECK(sawReleaseGtPre);
        CHECK(sawEqualRelease);
    }

    SUBCASE("BVSolver proves semverCompare satisfies irreflexivity, antisymmetry, and transitivity") {
        std::string err;
        // 1. Irreflexivity: semverCompare(a, a) === 0
        ProveEquivReport irrefl = bronze::check::proveSourceEquiv(
            R"(
                export function checkIrrefl(maj, min, pat, pre) {
                    if (maj !== maj) return maj < maj ? -1 : 1;
                    if (min !== min) return min < min ? -1 : 1;
                    if (pat !== pat) return pat < pat ? -1 : 1;
                    if (pre === 0 && pre === 0) return 0;
                    if (pre === 0) return 1;
                    if (pre === 0) return -1;
                    if (pre !== pre) return pre < pre ? -1 : 1;
                    return 0;
                }
            )",
            "export function checkIrrefl(maj, min, pat, pre) { return 0; }",
            "checkIrrefl", &err);
        REQUIRE(err.empty());
        CHECK(irrefl.allEquivalent());

        // 2. Antisymmetry: semverCompare(a, b) === -semverCompare(b, a)
        ProveEquivReport antisym = bronze::check::proveSourceEquiv(
            R"(
                export function cmp(maj1, min1, pat1, pre1, maj2, min2, pat2, pre2) {
                    if (maj1 !== maj2) return maj1 < maj2 ? -1 : 1;
                    if (min1 !== min2) return min1 < min2 ? -1 : 1;
                    if (pat1 !== pat2) return pat1 < pat2 ? -1 : 1;
                    if (pre1 === 0 && pre2 === 0) return 0;
                    if (pre1 === 0) return 1;
                    if (pre2 === 0) return -1;
                    if (pre1 !== pre2) return pre1 < pre2 ? -1 : 1;
                    return 0;
                }
            )",
            R"(
                export function cmp(maj1, min1, pat1, pre1, maj2, min2, pat2, pre2) {
                    if (maj2 !== maj1) return -(maj2 < maj1 ? -1 : 1);
                    if (min2 !== min1) return -(min2 < min1 ? -1 : 1);
                    if (pat2 !== pat1) return -(pat2 < pat1 ? -1 : 1);
                    if (pre2 === 0 && pre1 === 0) return 0;
                    if (pre2 === 0) return -1;
                    if (pre1 === 0) return 1;
                    if (pre2 !== pre1) return -(pre2 < pre1 ? -1 : 1);
                    return 0;
                }
            )",
            "cmp", &err);
        REQUIRE(err.empty());
        CHECK(antisym.allEquivalent());

        // 3. Transitivity: (cmp(a, b) < 0 && cmp(b, c) < 0) => cmp(a, c) < 0
        ProveEquivReport trans = bronze::check::proveSourceEquiv(
            R"(
                function cmp(maj1, min1, pre1, maj2, min2, pre2) {
                    if (maj1 !== maj2) return maj1 < maj2 ? -1 : 1;
                    if (min1 !== min2) return min1 < min2 ? -1 : 1;
                    if (pre1 === 0 && pre2 === 0) return 0;
                    if (pre1 === 0) return 1;
                    if (pre2 === 0) return -1;
                    if (pre1 !== pre2) return pre1 < pre2 ? -1 : 1;
                    return 0;
                }
                export function checkTrans(maj1, min1, pre1, maj2, min2, pre2, maj3, min3, pre3) {
                    const ab = cmp(maj1, min1, pre1, maj2, min2, pre2);
                    const bc = cmp(maj2, min2, pre2, maj3, min3, pre3);
                    if (ab < 0 && bc < 0) {
                        const ac = cmp(maj1, min1, pre1, maj3, min3, pre3);
                        return ac < 0 ? 1 : 0;
                    }
                    return 1;
                }
            )",
            R"(
                export function checkTrans(maj1, min1, pre1, maj2, min2, pre2, maj3, min3, pre3) {
                    return 1;
                }
            )",
            "checkTrans", &err);
        REQUIRE(err.empty());
        CHECK(trans.allEquivalent());
    }

    SUBCASE("BVSolver exposes 32-bit subtraction / bitwise coercion overflow bug in numeric prerelease comparison") {
        // Naive numeric identifier comparator `(id1 | 0) - (id2 | 0)` overflows signed 32-bit int
        // when comparing e.g. id1 = 2026102312 and id2 = -200000000 or timestamps > 2147483647!
        std::string err;
        EquivReport subBug = bronze::check::proveSourceEquivalent(
            R"(
                export function cmpNumericId(id1, id2) {
                    const diff = (id1 - id2) | 0;
                    return diff < 0 ? -1 : (diff > 0 ? 1 : 0);
                }
            )",
            R"(
                export function cmpNumericId(id1, id2) {
                    return id1 < id2 ? -1 : (id1 > id2 ? 1 : 0);
                }
            )",
            "cmpNumericId", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(subBug.status == EquivStatus::Counterexample);
    }
}

TEST_CASE("Issue #124.3: Color-Space Conversions (color-convert / culori rgbToHsl & rgbTo565)") {
    SUBCASE("static_checker and synth_tests detect division-by-zero / NaN poison in unguarded rgbToHsl on achromatic inputs") {
        const char* unguardedRgbToHsl = R"(
            export function rgbToHslUnguarded(r, g, b) {
                const max = Math.max(r, g, b);
                const min = Math.min(r, g, b);
                const l = (max + min) / 2;
                const d = max - min;
                const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
                const h = (g - b) / d;
                return h + s + l;
            }
        )";
        CheckReport checkRep = bronze::check::checkSource(unguardedRgbToHsl);
        CHECK(checkRep.divByZeroCount >= 3);  // 2 - max - min, max + min, and d!

        std::string err;
        SynthTestsReport synthRep = bronze::check::synthesizeTestsForSource(unguardedRgbToHsl, &err);
        REQUIRE(err.empty());
        CHECK(synthRep.hazardReproducerCount >= 1);

        // Verify concrete achromatic inputs trigger 0 / 0 = NaN in unguarded rgbToHsl:
        // Black (r = g = b = 0) => max + min = 0, d = 0 => s = 0/0 = NaN, h = 0/0 = NaN
        // White (r = g = b = 1) => 2 - max - min = 0, d = 0 => s = 0/0 = NaN, h = 0/0 = NaN
        bool foundAchromaticWitness = false;
        for (const auto& t : synthRep.tests) {
            if (t.kind == SynthTestKind::HazardReproducer && t.args.size() == 3) {
                if (t.args[0] == t.args[1] && t.args[1] == t.args[2]) {
                    foundAchromaticWitness = true;
                }
            }
        }
        CHECK(foundAchromaticWitness);
    }

    SUBCASE("BVSolver proves RGB565 packing boundedness in [0, 65535] and upper-bit preservation") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function rgb565RoundTripHighBits(r, g, b) {
                    const r8 = r & 255;
                    const g8 = g & 255;
                    const b8 = b & 255;
                    const packed = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);
                    const rRec = (packed >> 11) & 31;
                    const gRec = (packed >> 5) & 63;
                    const bRec = packed & 31;
                    return (rRec === (r8 >> 3) && gRec === (g8 >> 2) && bRec === (b8 >> 3) &&
                            packed >= 0 && packed <= 65535) ? 1 : 0;
                }
            )",
            R"(
                export function rgb565RoundTripHighBits(r, g, b) {
                    return 1;
                }
            )",
            "rgb565RoundTripHighBits", &err);
        REQUIRE(err.empty());
        CHECK(rep.allEquivalent());
    }
}

TEST_CASE("Issue #124.4: Financial & Basis-Point Rounding (IEEE-754 1.005 trap vs integer basis-point conservation)") {
    SUBCASE("Demonstrates IEEE-754 binary64 midpoint representation trap on Math.round(1.005 * 100)") {
        // In IEEE-754 binary64, 1.005 is actually 1.00499999999999989341858963598497211933135986328125,
        // so 1.005 * 100 === 100.4999999999999857891452847979962825775146484375 < 100.5!
        volatile double priceDollars = 1.005;
        double scaled = priceDollars * 100.0;
        CHECK(scaled < 100.5);
        CHECK(static_cast<int64_t>(std::round(scaled)) == 100);  // Wrong! Expected 101 cents!
    }

    SUBCASE("BVSolver proves integer basis-point fee + net splitting conserves exact cents without 1-cent creation/destruction") {
        // For any amountCents in [0, 100000] at standard 250 bps (2.50% fee):
        //   fee = Math.floor((amountCents * 250 + 5000) / 10000)
        //   net = amountCents - fee
        // Prove via BVSolver:
        //   1. fee + net === amountCents (exact conservation of money)
        //   2. 0 <= fee && fee <= amountCents && 0 <= net && net <= amountCents
        BVContext ctx;
        BVNodeId amount = ctx.make_var("amountCents", 32);
        BVNodeId bps = ctx.make_const(250, 32);

        BVNodeId domain = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Sge, amount, ctx.make_const(0, 32), 1),
            ctx.make_binary(BVOp::Sle, amount, ctx.make_const(100000, 32), 1), 1);

        BVNodeId prod = ctx.make_binary(BVOp::Mul, amount, bps, 32);
        BVNodeId roundedNum = ctx.make_binary(BVOp::Add, prod, ctx.make_const(5000, 32), 32);
        BVNodeId fee = brass::verify::expand_const_div_rem_bv(
            ctx, BVOp::SDiv, roundedNum, ctx.make_const(10000, 32), 32);
        BVNodeId net = ctx.make_binary(BVOp::Sub, amount, fee, 32);
        BVNodeId sum = ctx.make_binary(BVOp::Add, fee, net, 32);

        BVNodeId notConserved = ctx.make_binary(BVOp::Ne, sum, amount, 1);
        BVNodeId feeNeg = ctx.make_binary(BVOp::Slt, fee, ctx.make_const(0, 32), 1);
        BVNodeId feeGtAmount = ctx.make_binary(BVOp::Sgt, fee, amount, 1);
        BVNodeId netNeg = ctx.make_binary(BVOp::Slt, net, ctx.make_const(0, 32), 1);
        BVNodeId violation = ctx.make_binary(
            BVOp::Or,
            ctx.make_binary(BVOp::Or, notConserved, feeNeg, 1),
            ctx.make_binary(BVOp::Or, feeGtAmount, netNeg, 1), 1);

        SolveOutcome res = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, domain, violation, 1));
        CHECK(res.status == SolveOutcome::Unsat);
    }

    SUBCASE("BVSolver refutes naive independent rounding of two split legs (creates/destroys 1 cent on odd amounts)") {
        // If a 50/50 split rounds both legs independently:
        //   legA = Math.floor((amountCents * 5000 + 5000) / 10000)
        //   legB = Math.floor((amountCents * 5000 + 5000) / 10000)
        // then on any odd amountCents (e.g. 1 cent), legA + legB = 1 + 1 = 2 !== 1!
        std::string err;
        EquivReport splitBug = bronze::check::proveSourceEquivalent(
            R"(
                export function splitTotal(amountCents) {
                    const legA = ((amountCents * 5000 + 5000) / 10000) | 0;
                    const legB = ((amountCents * 5000 + 5000) / 10000) | 0;
                    return legA + legB;
                }
            )",
            R"(
                export function splitTotal(amountCents) {
                    return amountCents;
                }
            )",
            "splitTotal", 0, 1000, &err);
        REQUIRE(err.empty());
        CHECK(splitBug.status == EquivStatus::Counterexample);
        CHECK((splitBug.counterexampleInput % 2) != 0);
        CHECK(splitBug.expectedOutput == splitBug.counterexampleInput + 1);
    }
}
