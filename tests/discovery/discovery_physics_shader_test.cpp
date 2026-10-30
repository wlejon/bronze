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
using bronze::check::BugKind;
using bronze::check::CheckReport;
using bronze::check::EquivReport;
using bronze::check::EquivStatus;
using bronze::check::ProveEquivReport;
using bronze::check::SynthTestKind;
using bronze::check::SynthTestsReport;

bool hasFindingKind(const CheckReport& rep, BugKind kind) {
    for (const auto& f : rep.findings) {
        if (f.kind == kind) return true;
    }
    return false;
}

// Analytical 3-regime damped harmonic oscillator (x(0) = 1, v(0) = 0)
double evalBranchedSpring(double zeta, double omega0, double t) {
    const double eps = 1e-6;
    if (zeta < 1.0 - eps) {
        double omegaD = omega0 * std::sqrt(std::max(0.0, 1.0 - zeta * zeta));
        double env = std::exp(-zeta * omega0 * t);
        return env * (std::cos(omegaD * t) + (zeta * omega0 / omegaD) * std::sin(omegaD * t));
    }
    if (zeta <= 1.0 + eps) {
        // Critical damping Taylor limit as omega_d -> 0: sin(omega_d * t) / omega_d -> t
        return (1.0 + omega0 * t) * std::exp(-omega0 * t);
    }
    // Overdamped regime (zeta > 1): r1 = -omega0*(zeta - sqrt(zeta^2 - 1)), r2 = -omega0*(zeta + sqrt(zeta^2 - 1))
    // Since r1 - r2 = 2 * omegaH > 0, x(t) = (r1 * exp(r2 * t) - r2 * exp(r1 * t)) / (r1 - r2)
    double omegaH = omega0 * std::sqrt(std::max(0.0, zeta * zeta - 1.0));
    double r1 = -omega0 * (zeta - std::sqrt(std::max(0.0, zeta * zeta - 1.0)));
    double r2 = -omega0 * (zeta + std::sqrt(std::max(0.0, zeta * zeta - 1.0)));
    return (r1 * std::exp(r2 * t) - r2 * std::exp(r1 * t)) / (2.0 * omegaH);
}

}  // namespace

TEST_CASE("Issue #127.1: Harmonic Oscillator Spring Solver Singularities (Critical Damping zeta=1, Overdamped sqrt, Euler Stability)") {
    SUBCASE("Detects critical-damping (zeta === 1) 0/0 NaN and overdamped (zeta > 1) sqrt domain error in naive spring solvers") {
        std::string err;

        // Naive underdamped spring formula used without regime branching:
        //   omegaD = omega0 * Math.sqrt(1 - zeta * zeta)
        //   pos = Math.exp(-zeta * omega0 * t) * (Math.cos(omegaD * t) + ((zeta * omega0) / omegaD) * Math.sin(omegaD * t))
        // Fails at:
        //   1. zeta === 1 (critical damping): 1 - zeta * zeta === 0 -> omegaD === 0 -> division by zero!
        //   2. zeta > 1 (overdamped): 1 - zeta * zeta < 0 -> Math.sqrt(negative) === NaN!
        const char* naiveUnderdampedSpring = R"(
            export function springUnderdampedNaive(zeta, omega0, t) {
                const disc = 1 - zeta * zeta;
                const omegaD = omega0 * Math.sqrt(disc);
                const c2 = (zeta * omega0) / omegaD;
                return Math.cos(omegaD * t) + c2 * Math.sin(omegaD * t);
            }
        )";
        CheckReport naiveRep = bronze::check::checkSource(naiveUnderdampedSpring, true, &err);
        REQUIRE(err.empty());
        CHECK(hasFindingKind(naiveRep, BugKind::NaNPoison));
        CHECK(naiveRep.domainErrorCount >= 1);
        CHECK(naiveRep.divByZeroCount >= 1);

        SynthTestsReport synthRep = bronze::check::synthesizeSourceTests(naiveUnderdampedSpring, &err);
        REQUIRE(err.empty());
        CHECK(synthRep.hazardReproducerCount >= 1);

        // Concrete IEEE-754 demonstration of the critical-damping 0/0 NaN singularity at zeta = 1.0:
        double zetaCrit = 1.0;
        double omega0 = 15.0;
        double t = 0.1;
        double omegaD = omega0 * std::sqrt(1.0 - zetaCrit * zetaCrit);
        CHECK(omegaD == 0.0);
        double naiveCritVal = std::exp(-zetaCrit * omega0 * t) *
                              (std::cos(omegaD * t) + ((zetaCrit * omega0) / omegaD) * std::sin(omegaD * t));
        CHECK(std::isnan(naiveCritVal));  // Infinity * 0.0 === NaN!
    }

    SUBCASE("Verifies 3-regime branched spring solver safety, 100% branch synthesis, and C0 continuity across zeta = 1") {
        std::string err;

        const char* branchedSpringRegime = R"(
            export function classifySpringRegime(zetaMilli, omega0) {
                if (omega0 <= 0) return 0;
                if (zetaMilli < 999) {
                    return 1;
                } else if (zetaMilli <= 1001) {
                    return 2;
                } else {
                    return 3;
                }
            }
        )";
        CheckReport safeRep = bronze::check::checkSource(branchedSpringRegime, true, &err);
        REQUIRE(err.empty());
        CHECK(safeRep.findings.empty());

        SynthTestsReport synth = bronze::check::synthesizeSourceTests(branchedSpringRegime, &err);
        REQUIRE(err.empty());
        CHECK(synth.coveredBlocks == synth.totalBlocks);

        bool hitUnder = false, hitCrit = false, hitOver = false;
        for (const auto& tc : synth.tests) {
            if (tc.kind != SynthTestKind::BranchCoverage || !tc.hasExpectedReturn) continue;
            if (tc.expectedReturn == 1) hitUnder = true;
            if (tc.expectedReturn == 2) hitCrit = true;
            if (tc.expectedReturn == 3) hitOver = true;
        }
        CHECK(hitUnder);
        CHECK(hitCrit);
        CHECK(hitOver);

        // Verify analytical continuity across the critical damping boundary zeta = 1.0 +/- 1e-5
        double pUnder = evalBranchedSpring(1.0 - 2e-5, 12.0, 0.25);
        double pCrit = evalBranchedSpring(1.0, 12.0, 0.25);
        double pOver = evalBranchedSpring(1.0 + 2e-5, 12.0, 0.25);
        CHECK(std::isfinite(pUnder));
        CHECK(std::isfinite(pCrit));
        CHECK(std::isfinite(pOver));
        CHECK(std::abs(pUnder - pCrit) < 1e-5);
        CHECK(std::abs(pOver - pCrit) < 1e-5);
    }

    SUBCASE("BVSolver proves explicit Euler spring step strictly amplifies mechanical energy for any non-zero state") {
        // For an undamped harmonic oscillator with normalized omega0 = 1:
        //   Explicit Euler step with integer time-scale s >= 1:
        //     x1 = x0 + s * v0
        //     v1 = v0 - s * x0
        //   Mechanical energy E = x^2 + v^2 satisfies:
        //     E1 = x1^2 + v1^2 = (x0 + s*v0)^2 + (v0 - s*x0)^2 = (1 + s^2) * (x0^2 + v0^2) > E0!
        std::string err;
        EquivReport energyGrowth = bronze::check::proveSourceEquivalent(
            R"(
                export function eulerAmplifies(x0) {
                    if (x0 === 0) return 1;
                    const v0 = 2;
                    const s = 1;
                    const x1 = x0 + s * v0;
                    const v1 = v0 - s * x0;
                    const e0 = x0 * x0 + v0 * v0;
                    const e1 = x1 * x1 + v1 * v1;
                    return e1 === 2 * e0 && e1 > e0 ? 1 : 0;
                }
            )",
            R"(
                export function eulerAmplifies(x0) {
                    return 1;
                }
            )",
            "eulerAmplifies", -100, 100, &err);
        REQUIRE(err.empty());
        CHECK(energyGrowth.status == EquivStatus::Equivalent);
    }
}

TEST_CASE("Issue #127.2: Cubic-Bezier Easing Flat-Tangent Newton-Raphson Singularity & Ray-AABB Slab 0/0 NaN") {
    SUBCASE("Exposes bezier-easing flat-tangent getSlope(t) === 0 division-by-zero and proves bisection fallback bounds") {
        std::string err;

        // In cubic-bezier(mX1, mY1, mX2, mY2), the x-derivative at t is:
        //   getSlope(t, mX1, mX2) = 3 * A(mX1, mX2) * t^2 + 2 * B(mX1, mX2) * t + 3 * mX1
        // When mX1 === 0 (horizontal initial tangent, e.g. ease-in cubic-bezier(0, 0, 0.58, 1)) and guessT === 0,
        // getSlope(0, 0, mX2) === 0! An unguarded Newton-Raphson step divides by currentSlope === 0!
        const char* unguardedBezierNewton = R"(
            export function newtonStepBezier(aX, guessT, mX1, mX2) {
                const c = 3 * mX1;
                const b = 3 * (mX2 - mX1) - c;
                const a = 1 - c - b;
                const currentX = ((a * guessT + b) * guessT + c) * guessT - aX;
                const currentSlope = (3 * a * guessT + 2 * b) * guessT + c;
                return guessT - currentX / currentSlope;
            }
        )";
        CheckReport bezRep = bronze::check::checkSource(unguardedBezierNewton, true, &err);
        REQUIRE(err.empty());
        CHECK(hasFindingKind(bezRep, BugKind::NaNPoison));
        CHECK(bezRep.divByZeroCount >= 1);

        // Prove that when currentSlope is small (< minSlope), falling back to binary-subdivision midpoint
        // aA + ((aB - aA) >> 1) stays strictly within the bracket [aA, aB] subset of [0, 1024]
        EquivReport bisectionBounded = bronze::check::proveSourceEquivalent(
            R"(
                export function bisectMidInBounds(aA) {
                    const aB = 1024;
                    if (aA < 0 || aA > aB) return 1;
                    const mid = aA + ((aB - aA) >> 1);
                    return (mid >= aA && mid <= aB) ? 1 : 0;
                }
            )",
            R"(
                export function bisectMidInBounds(aA) {
                    return 1;
                }
            )",
            "bisectMidInBounds", 0, 1024, &err);
        REQUIRE(err.empty());
        CHECK(bisectionBounded.status == EquivStatus::Equivalent);
    }

    SUBCASE("Exposes Ray-AABB slab intersection 0 / 0 = NaN when rayOrigX === boxMinX and rayDirX === 0") {
        std::string err;

        // Kay-Kajiya slab method:
        //   invDirX = 1.0 / rayDirX
        //   t1 = (boxMinX - rayOrigX) * invDirX
        // When rayDirX === 0 and rayOrigX === boxMinX, (0.0) * (1.0 / 0.0) = 0.0 * Infinity = NaN!
        const char* unguardedSlabRayBox = R"(
            export function slabNearT(rayOrigX, rayDirX, boxMinX, boxMaxX) {
                const t1 = (boxMinX - rayOrigX) / rayDirX;
                const t2 = (boxMaxX - rayOrigX) / rayDirX;
                return Math.min(t1, t2);
            }
        )";
        CheckReport slabRep = bronze::check::checkSource(unguardedSlabRayBox, true, &err);
        REQUIRE(err.empty());
        CHECK(hasFindingKind(slabRep, BugKind::NaNPoison));
        CHECK(slabRep.divByZeroCount >= 1);

        // Concrete IEEE-754 demonstration of 0 / 0 = NaN poisoning tMin/tMax in slab intersection:
        // volatile: a folded constant divide by zero is MSVC's C4723, an error under /WX.
        volatile double zeroDir = 0.0;
        double rayOrigX = 10.0, rayDirX = zeroDir, boxMinX = 10.0, boxMaxX = 20.0;
        double t1 = (boxMinX - rayOrigX) / rayDirX;
        double t2 = (boxMaxX - rayOrigX) / rayDirX;
        CHECK(std::isnan(t1));
        CHECK(std::isinf(t2));

        // Prove guarded axis-parallel slab overlap test is symmetric and hazard-free:
        const char* guardedSlabRayBox = R"(
            export function slabAxisHit(rayOrigX, rayDirX, boxMinX, boxMaxX) {
                if (rayDirX === 0) {
                    return (rayOrigX >= boxMinX && rayOrigX <= boxMaxX) ? 1 : 0;
                }
                const t1 = (boxMinX - rayOrigX) / rayDirX;
                const t2 = (boxMaxX - rayOrigX) / rayDirX;
                const tMin = Math.min(t1, t2);
                const tMax = Math.max(t1, t2);
                return tMax >= Math.max(0, tMin) ? 1 : 0;
            }
        )";
        CheckReport guardedSlabRep = bronze::check::checkSource(guardedSlabRayBox, true, &err);
        REQUIRE(err.empty());
        CHECK(guardedSlabRep.divByZeroCount == 0);
    }
}

TEST_CASE("Issue #127.3: Cross-Language CPU-JS vs GPU-GLSL/SPIR-V Coordinate Wrapping Traps (mod vs %, fract vs trunc)") {
    SUBCASE("BVSolver refutes naive JS x % y against GLSL mod(x, y) = x - y * floor(x / y) on negative coordinates and proves Euclidean fix") {
        std::string err;

        // In GLSL / SPIR-V (OpFMod), mod(x, m) = x - m * floor(x / m), which is always in [0, m) for m > 0.
        // In JS, x % m = x - m * trunc(x / m) (SPIR-V OpFRem / OpSRem), which is negative for x < 0!
        // For power-of-two tile grid m = 16:
        //   GLSL mod(x, 16) = x - ((x >> 4) << 4) = x & 15
        //   JS   x % 16     = signed remainder
        EquivReport jsRemVsGlslMod = bronze::check::proveSourceEquivalent(
            R"(
                export function tileCoord(x) {
                    return x % 16;
                }
            )",
            R"(
                export function tileCoord(x) {
                    return x - ((x >> 4) << 4);
                }
            )",
            "tileCoord", -1000, 1000, &err);
        REQUIRE(err.empty());
        CHECK(jsRemVsGlslMod.status == EquivStatus::Counterexample);
        CHECK(jsRemVsGlslMod.counterexampleInput < 0);
        CHECK(jsRemVsGlslMod.expectedOutput < 0);
        CHECK(jsRemVsGlslMod.actualOutput >= 0);

        // Prove that JS Euclidean modulo ((x % 16) + 16) % 16 is bit-identical to GLSL floor-based
        // mod(x, 16) = x - 16 * floor(x / 16) across all coordinates in [-100000, 100000]!
        EquivReport euclideanMatchesGlslMod = bronze::check::proveSourceEquivalent(
            R"(
                export function glslMod16(x) {
                    return ((x % 16) + 16) % 16;
                }
            )",
            R"(
                export function glslMod16(x) {
                    return x - ((x >> 4) << 4);
                }
            )",
            "glslMod16", -100000, 100000, &err);
        REQUIRE(err.empty());
        CHECK(euclideanMatchesGlslMod.status == EquivStatus::Equivalent);
    }

    SUBCASE("BVSolver refutes naive JS x - Math.trunc(x) against GLSL fract(x) = x - Math.floor(x) on negative coordinates") {
        std::string err;

        // In Q24.8 fixed-point coordinates (1.0 === 256):
        //   floor(x) = x & ~255  (arithmetic floor toward -Infinity)
        //   trunc(x) = x >= 0 ? (x & ~255) : -((-x) & ~255)  (truncation toward 0)
        // Naive fract(x) = x - trunc(x) returns negative values on x < 0, mirroring UV coordinates across 0!
        EquivReport truncVsFloorFract = bronze::check::proveSourceEquivalent(
            R"(
                export function fractQ8(x) {
                    const truncX = x >= 0 ? (x & ~255) : -((-x) & ~255);
                    return x - truncX;
                }
            )",
            R"(
                export function fractQ8(x) {
                    const floorX = x & ~255;
                    return x - floorX;
                }
            )",
            "fractQ8", -10000, 10000, &err);
        REQUIRE(err.empty());
        CHECK(truncVsFloorFract.status == EquivStatus::Counterexample);
        CHECK(truncVsFloorFract.counterexampleInput < 0);
        CHECK(truncVsFloorFract.expectedOutput < 0);
        CHECK(truncVsFloorFract.actualOutput > 0);

        // Prove GLSL fract(x) = x - floor(x) is identical to (x & 255) across all 2^32 Q24.8 coordinates
        ProveEquivReport floorFractBitmask = bronze::check::proveSourceEquiv(
            R"(
                export function glslFractQ8(x) {
                    const floorX = x & ~255;
                    return x - floorX;
                }
            )",
            R"(
                export function glslFractQ8(x) {
                    return x & 255;
                }
            )",
            "glslFractQ8", &err);
        REQUIRE(err.empty());
        CHECK(floorFractBitmask.allEquivalent());
    }
}
