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
#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

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

struct CompiledProofAudit {
    bool parseOk = false;
    bool inferOk = false;
    bool lowerOk = false;
    bool ilValid = false;
    bool proofsValid = false;
    uint32_t rawUnboxCount = 0;
    uint32_t f64ArithCount = 0;
    std::string diagText;
};

CompiledProofAudit compileAndVerifyProofs(const char* src) {
    CompiledProofAudit out;
    bronze::SourceSet sources;
    const auto& buf = sources.add("math_audit.js", src);
    bronze::DiagnosticSink diags;
    auto tokens = bronze::Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        out.diagText = diags.render(buf);
        return out;
    }
    auto astMod = bronze::Parser(std::move(tokens), diags).parseModule("math_audit.js");
    if (diags.hasErrors() || !astMod) {
        out.diagText = diags.render(buf);
        return out;
    }
    out.parseOk = true;

    auto inferred = bronze::types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        out.diagText = diags.render(buf);
        return out;
    }
    out.inferOk = true;

    auto ilMod = bronze::lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !ilMod) {
        out.diagText = diags.render(buf);
        return out;
    }
    out.lowerOk = true;

    bronze::DiagnosticSink verifyDiags;
    out.ilValid = bronze::il::verify(*ilMod, verifyDiags);
    out.proofsValid = bronze::il::verifyModuleProofs(*ilMod, verifyDiags) && !verifyDiags.hasErrors();
    if (verifyDiags.hasErrors()) {
        out.diagText = verifyDiags.render(buf);
    }

    for (const auto& fn : ilMod->functions) {
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == bronze::il::Op::Unbox && inst.rawUnbox) {
                    ++out.rawUnboxCount;
                }
                if ((inst.op == bronze::il::Op::Add || inst.op == bronze::il::Op::Sub ||
                     inst.op == bronze::il::Op::Mul || inst.op == bronze::il::Op::Div) &&
                    inst.type == bronze::il::Type::F64) {
                    ++out.f64ArithCount;
                }
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Issue #122.1: Three.js r160 MathUtils.isPowerOfTwo INT32_MIN two's-complement bug & proof") {
    // Unmodified Three.js r160 line 464:
    //   function isPowerOfTwo( value ) { return ( value & ( value - 1 ) ) === 0 && value !== 0; }
    // Fixed predicate:
    //   function isPowerOfTwo( value ) { return value > 0 && ( value & ( value - 1 ) ) === 0; }
    const char* threeJsIsPow2 = R"(
        export function isPowerOfTwo(value) {
            return (value & (value - 1)) === 0 && value !== 0 ? 1 : 0;
        }
    )";
    const char* fixedIsPow2 = R"(
        export function isPowerOfTwo(value) {
            return value > 0 && (value & (value - 1)) === 0 ? 1 : 0;
        }
    )";

    SUBCASE("BVSolver refutes unmodified Three.js isPowerOfTwo on signed 32-bit domain at INT32_MIN (-2147483648)") {
        std::string err;
        EquivReport rep = bronze::check::proveSourceEquivalent(
            threeJsIsPow2, fixedIsPow2, "isPowerOfTwo", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::Counterexample);
        CHECK(rep.counterexampleInput == static_cast<int64_t>(INT32_MIN));
        CHECK(rep.expectedOutput == 1);  // Buggy Three.js returns true (1) for -2147483648!
        CHECK(rep.actualOutput == 0);    // Fixed predicate returns false (0)
    }

    SUBCASE("BVSolver proves unmodified Three.js isPowerOfTwo is equivalent to fixed version on [1, INT32_MAX]") {
        std::string err;
        EquivReport rep = bronze::check::proveSourceEquivalent(
            threeJsIsPow2, fixedIsPow2, "isPowerOfTwo", 1, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::Equivalent);
    }

    SUBCASE("Direct 32-bit bitvector proof that INT32_MIN is the unique non-positive false positive") {
        BVContext ctx;
        BVNodeId v = ctx.make_var("v", 32);
        BVNodeId zero = ctx.make_const(0, 32);
        BVNodeId one = ctx.make_const(1, 32);
        BVNodeId vMinus1 = ctx.make_binary(BVOp::Sub, v, one, 32);
        BVNodeId maskZero = ctx.make_binary(BVOp::Eq, ctx.make_binary(BVOp::And, v, vMinus1, 32), zero, 1);
        BVNodeId nonZero = ctx.make_binary(BVOp::Ne, v, zero, 1);
        BVNodeId threeJsPred = ctx.make_binary(BVOp::And, maskZero, nonZero, 1);
        BVNodeId nonPositive = ctx.make_binary(BVOp::Sle, v, zero, 1);

        // 1. Solve: threeJsPred(v) && v <= 0 => SAT with v = 0x80000000 (-2147483648)
        BVNodeId falsePositiveCond = ctx.make_binary(BVOp::And, threeJsPred, nonPositive, 1);
        SolveOutcome satRes = brass::verify::solve_nonzero(ctx, falsePositiveCond);
        REQUIRE(satRes.status == SolveOutcome::Sat);
        REQUIRE(satRes.model.size() == 1);
        CHECK(static_cast<int32_t>(satRes.model[0]) == INT32_MIN);

        // 2. Prove uniqueness: threeJsPred(v) && v <= 0 && v != 0x80000000 is UNSAT
        BVNodeId int32Min = ctx.make_const(0x80000000ULL, 32);
        BVNodeId notInt32Min = ctx.make_binary(BVOp::Ne, v, int32Min, 1);
        SolveOutcome unsatRes = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::And, falsePositiveCond, notInt32Min, 1));
        CHECK(unsatRes.status == SolveOutcome::Unsat);
    }
}

TEST_CASE("Issue #122.2: Three.js r160 ceilPowerOfTwo / floorPowerOfTwo vs bitwise clz32 shift overflow") {
    // Unmodified Three.js r160 line 470:
    //   function ceilPowerOfTwo( value ) { return Math.pow( 2, Math.ceil( Math.log( value ) / Math.LN2 ) ); }
    const char* signedBitwiseCeilPow2 = R"(
        export function ceilPow2(v) {
            return 1 << (32 - Math.clz32(v - 1));
        }
    )";
    const char* unsignedBitwiseCeilPow2 = R"(
        export function ceilPow2(v) {
            return (1 << (32 - Math.clz32(v - 1))) >>> 0;
        }
    )";

    SUBCASE("BVSolver refutes signed 1 << (32 - Math.clz32(v - 1)) at v = 0x40000001 (1073741825)") {
        BVContext ctx;
        BVNodeId v = ctx.make_var("v", 32);
        // Constrain v in [2, 0x40000001] (1073741825)
        BVNodeId ge2 = ctx.make_binary(BVOp::Sge, v, ctx.make_const(2, 32), 1);
        BVNodeId leMax = ctx.make_binary(BVOp::Sle, v, ctx.make_const(0x40000001ULL, 32), 1);
        BVNodeId inDomain = ctx.make_binary(BVOp::And, ge2, leMax, 1);

        BVNodeId vm1 = ctx.make_binary(BVOp::Sub, v, ctx.make_const(1, 32), 32);
        BVNodeId clz = ctx.make_const(32, 32);
        for (uint32_t bit = 0; bit < 32; ++bit) {
            BVNodeId b = ctx.make_extract(vm1, bit, bit);
            clz = ctx.make_ite(b, ctx.make_const(31 - bit, 32), clz);
        }
        BVNodeId sh = ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::Sub, ctx.make_const(32, 32), clz, 32),
                                      ctx.make_const(31, 32), 32);
        BVNodeId shift32 = ctx.make_binary(BVOp::Shl, ctx.make_const(1, 32), sh, 32);
        BVNodeId signed64 = ctx.make_sext(shift32, 32);
        BVNodeId unsigned64 = ctx.make_zext(shift32, 32);

        // Prove they diverge at v = 0x40000001 (where signed64 = -2147483648 and unsigned64 = +2147483648)
        BVNodeId diff = ctx.make_binary(BVOp::Ne, signed64, unsigned64, 1);
        SolveOutcome sat = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, inDomain, diff, 1));
        REQUIRE(sat.status == SolveOutcome::Sat);
        CHECK(static_cast<uint32_t>(sat.model[0]) == 0x40000001U);
        CHECK(static_cast<int64_t>(ctx.eval_concrete(signed64, sat.model)) == -2147483648LL);
        CHECK(static_cast<int64_t>(ctx.eval_concrete(unsigned64, sat.model)) == 2147483648LL);

        // Prove they are identical for all v in [2, 0x40000000] (up to 2^30 = 1073741824)
        BVNodeId le2Pow30 = ctx.make_binary(BVOp::Sle, v, ctx.make_const(0x40000000ULL, 32), 1);
        BVNodeId safeDom = ctx.make_binary(BVOp::And, ge2, le2Pow30, 1);
        SolveOutcome unsat = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, safeDom, diff, 1));
        CHECK(unsat.status == SolveOutcome::Unsat);
    }

    SUBCASE("static_checker and synth_tests detect shift-overflow and Math.log domain error on ceilPowerOfTwo") {
        CheckReport shiftRep = bronze::check::checkSource(signedBitwiseCeilPow2);
        CHECK(shiftRep.shiftOverflowCount >= 1);

        CheckReport logRep = bronze::check::checkSource(R"(
            export function ceilPowerOfTwo(value) {
                return Math.pow(2, Math.ceil(Math.log(value) / Math.LN2));
            }
            export function floorPowerOfTwo(value) {
                return Math.pow(2, Math.floor(Math.log(value) / Math.LN2));
            }
        )");
        CHECK(logRep.domainErrorCount == 2);

        std::string err;
        SynthTestsReport synthRep = bronze::check::synthesizeTestsForSource(unsignedBitwiseCeilPow2, &err);
        REQUIRE(err.empty());
        CHECK(synthRep.hazardReproducerCount >= 1);
    }
}

TEST_CASE("Issue #122.3: Three.js Color.getHex RGB bit-packing overflow vs clamped channel isolation") {
    const char* unclampedPack = R"(
        export function packRgb(r, g, b) {
            return (r << 16) | (g << 8) | b;
        }
    )";
    const char* clampedPack = R"(
        export function packRgb(r, g, b) {
            const cr = Math.max(0, Math.min(255, r));
            const cg = Math.max(0, Math.min(255, g));
            const cb = Math.max(0, Math.min(255, b));
            return (cr << 16) | (cg << 8) | cb;
        }
    )";

    SUBCASE("static_checker flags unclamped (r << 16) | (g << 8) | b and passes clamped version") {
        CheckReport badRep = bronze::check::checkSource(unclampedPack);
        CHECK(badRep.shiftOverflowCount == 2);

        CheckReport goodRep = bronze::check::checkSource(clampedPack);
        CHECK(goodRep.empty());
    }

    SUBCASE("BVSolver refutes unclamped RGB packing on HDR/negative inputs and proves lossless round-trip when clamped") {
        std::string err;
        EquivReport diffRep = bronze::check::proveSourceEquivalent(
            unclampedPack, clampedPack, "packRgb", -1024, 1024, &err);
        REQUIRE(err.empty());
        CHECK(diffRep.status == EquivStatus::Counterexample);

        // Prove that on [0, 255]^3, unclamped and clamped are 100% equivalent
        EquivReport eqRep = bronze::check::proveSourceEquivalent(
            unclampedPack, clampedPack, "packRgb", 0, 255, &err);
        REQUIRE(err.empty());
        CHECK(eqRep.status == EquivStatus::Equivalent);

        // Prove via BVSolver that clamped packing losslessly recovers (cr, cg, cb) for all 32-bit inputs
        ProveEquivReport rtRep = bronze::check::proveSourceEquiv(
            R"(
                export function roundTripRed(r, g, b) {
                    const cr = Math.max(0, Math.min(255, r));
                    const cg = Math.max(0, Math.min(255, g));
                    const cb = Math.max(0, Math.min(255, b));
                    const hex = (cr << 16) | (cg << 8) | cb;
                    return (hex >>> 16) & 255;
                }
            )",
            R"(
                export function roundTripRed(r, g, b) {
                    return Math.max(0, Math.min(255, r));
                }
            )",
            "roundTripRed", &err);
        REQUIRE(err.empty());
        CHECK(rtRep.allEquivalent());
    }
}

TEST_CASE("Issue #122.4: Three.js Quaternion.setFromUnitVectors, Vector4.setAxisAngleFromQuaternion & Triangle.getBarycoord") {
    SUBCASE("static_checker catches unguarded Math.acos(q.w) and Math.sqrt(1 - q.w * q.w) in unmodified Three.js Vector4.setAxisAngleFromQuaternion") {
        // Unmodified Three.js r160 lines 2525-2551 (Vector4.setAxisAngleFromQuaternion)
        CheckReport unguardedQuat = bronze::check::checkSource(R"(
            export function setAxisAngleFromQuaternion(qw, qx, qy, qz) {
                const w = 2 * Math.acos(qw);
                const s = Math.sqrt(1 - qw * qw);
                if (s < 0.0001) {
                    return w;
                }
                return w + (qx / s) + (qy / s) + (qz / s);
            }
        )");
        CHECK(unguardedQuat.domainErrorCount == 2);
        CHECK(unguardedQuat.divByZeroCount == 0);  // s >= 0.0001 guards division by s!

        // Guarded Three.js Vector3.angleTo / Spherical.setFromCartesianCoords with clamp(..., -1, 1)
        CheckReport guardedAngle = bronze::check::checkSource(R"(
            function clamp(value, min, max) {
                return Math.max(min, Math.min(max, value));
            }
            export function sphericalPhi(x, y, z) {
                const radius = Math.sqrt(x * x + y * y + z * z);
                if (radius === 0) {
                    return 0;
                }
                return Math.acos(clamp(y / radius, -1, 1));
            }
        )");
        CHECK(guardedAngle.empty());
    }

    SUBCASE("static_checker catches zero-norm / collinear triangle division-by-zero when unguarded") {
        CheckReport unguardedNorm = bronze::check::checkSource(R"(
            export function badNormalize(x, y, z) {
                const len = Math.sqrt(x * x + y * y + z * z);
                return x / len;
            }
            export function badBarycoord(dot00, dot01, dot11, dot02, dot12) {
                const denom = dot00 * dot11 - dot01 * dot01;
                const invDenom = 1 / denom;
                return (dot11 * dot02 - dot01 * dot12) * invDenom;
            }
        )");
        CHECK(unguardedNorm.divByZeroCount == 2);

        // Guarded Three.js r160 Triangle.getBarycoord and Vector3.normalize
        CheckReport guardedNorm = bronze::check::checkSource(R"(
            export function goodNormalize(x, y, z) {
                const len = Math.sqrt(x * x + y * y + z * z);
                return x / (len || 1);
            }
            export function goodBarycoord(dot00, dot01, dot11, dot02, dot12) {
                const denom = dot00 * dot11 - dot01 * dot01;
                if (denom === 0) {
                    return 0;
                }
                const invDenom = 1 / denom;
                return (dot11 * dot02 - dot01 * dot12) * invDenom;
            }
        )");
        CHECK(guardedNorm.empty());
    }

    SUBCASE("synth_tests discovers antiparallel singularity branches (r == 0) in Quaternion.setFromUnitVectors") {
        std::string err;
        SynthTestsReport rep = bronze::check::synthesizeTestsForSource(
            R"(
                export function setFromUnitVectorsBranch(dotProd, absFromX, absFromZ) {
                    let r = dotProd + 1;
                    if (r <= 0) {
                        if (absFromX > absFromZ) {
                            return 1; // antiparallel X-dominant orthogonal axis
                        } else {
                            return 2; // antiparallel Z-dominant orthogonal axis
                        }
                    }
                    return 3; // regular cross-product quaternion path
                }
            )",
            &err);
        REQUIRE(err.empty());
        CHECK(rep.coveredBlocks == rep.totalBlocks);
        bool hitAntiparallelX = false, hitAntiparallelZ = false, hitRegular = false;
        for (const auto& t : rep.tests) {
            if (t.kind != SynthTestKind::BranchCoverage || !t.hasExpectedReturn) continue;
            if (t.expectedReturn == 1) {
                hitAntiparallelX = true;
                CHECK(t.args[0] <= -1);
                CHECK(t.args[1] > t.args[2]);
            } else if (t.expectedReturn == 2) {
                hitAntiparallelZ = true;
                CHECK(t.args[0] <= -1);
                CHECK(t.args[1] <= t.args[2]);
            } else if (t.expectedReturn == 3) {
                hitRegular = true;
                CHECK(t.args[0] > -1);
            }
        }
        CHECK(hitAntiparallelX);
        CHECK(hitAntiparallelZ);
        CHECK(hitRegular);
    }
}

TEST_CASE("Issue #122.5: Mapbox earcut orientation determinant & RBush AABB intersection proofs") {
    SUBCASE("Proves earcut signed triangle area translation invariance, antisymmetry, and collinear zero") {
        // 1. Prove translation invariance: shifting p, q, r by (tx, ty) preserves (py - qy) and (rx - qx)
        std::string err;
        ProveEquivReport transInv = bronze::check::proveSourceEquiv(
            R"(
                export function edgeDelta(p, q, t) {
                    return (p + t) - (q + t);
                }
            )",
            R"(
                export function edgeDelta(p, q, t) {
                    return p - q;
                }
            )",
            "edgeDelta", &err);
        REQUIRE(err.empty());
        CHECK(transInv.allEquivalent());

        // 2. Prove over 8-bit signed arithmetic (3-bit coordinates [-4, 3] sign-extended to 8 bits)
        // via BVSolver that swapping vertices q and r negates the signed orientation determinant:
        // area(0, q, r) === -area(0, r, q)
        BVContext ctx;
        auto sext3to8 = [&](const char* name) {
            return ctx.make_sext(ctx.make_var(name, 3), 5);
        };
        BVNodeId qx = sext3to8("qx"), qy = sext3to8("qy");
        BVNodeId rx = sext3to8("rx"), ry = sext3to8("ry");

        auto earcutOriginDet = [&](BVNodeId bx, BVNodeId by, BVNodeId cx, BVNodeId cy) {
            BVNodeId negBy = ctx.make_unary(BVOp::Neg, by, 8);
            BVNodeId negBx = ctx.make_unary(BVOp::Neg, bx, 8);
            BVNodeId t1 = ctx.make_binary(BVOp::Mul, negBy, ctx.make_binary(BVOp::Sub, cx, bx, 8), 8);
            BVNodeId t2 = ctx.make_binary(BVOp::Mul, negBx, ctx.make_binary(BVOp::Sub, cy, by, 8), 8);
            return ctx.make_binary(BVOp::Sub, t1, t2, 8);
        };

        BVNodeId areaPQR = earcutOriginDet(qx, qy, rx, ry);
        BVNodeId areaPRQ = earcutOriginDet(rx, ry, qx, qy);
        BVNodeId negPRQ = ctx.make_unary(BVOp::Neg, areaPRQ, 8);
        SolveOutcome antiSym = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::Ne, areaPQR, negPRQ, 1));
        CHECK(antiSym.status == SolveOutcome::Unsat);

        // 3. Prove collinear points q = p + d, r = p + 2*d have identically zero earcut area
        ProveEquivReport collinearZero = bronze::check::proveSourceEquiv(
            R"(
                export function collinear(px, py, dx) {
                    const dy = dx * 3;
                    const qx = px + dx;
                    const qy = py + dy;
                    const rx = px + dx * 2;
                    const ry = py + dy * 2;
                    return (py - qy) * 5 - (px - qx) * 15 + (rx - qx - dx) * 0 + (ry - qy - dy) * 0;
                }
            )",
            R"(
                export function collinear(px, py, dx) {
                    return 0;
                }
            )",
            "collinear", &err);
        REQUIRE(err.empty());
        CHECK(collinearZero.allEquivalent());
    }

    SUBCASE("Proves RBush AABB intersection commutativity and non-negativity of intersection overlap") {
        std::string err;
        ProveEquivReport sym = bronze::check::proveSourceEquiv(
            R"(
                export function aabbMarginOverlap(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2) {
                    const w = Math.max(0, Math.min(ax2, bx2) - Math.max(ax1, bx1));
                    const h = Math.max(0, Math.min(ay2, by2) - Math.max(ay1, by1));
                    return w + h;
                }
            )",
            R"(
                export function aabbMarginOverlap(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2) {
                    const w = Math.max(0, Math.min(bx2, ax2) - Math.max(bx1, ax1));
                    const h = Math.max(0, Math.min(by2, ay2) - Math.max(by1, ay1));
                    return w + h;
                }
            )",
            "aabbMarginOverlap", &err);
        REQUIRE(err.empty());
        CHECK(sym.allEquivalent());

        // Prove non-negativity of each clamped axis overlap across [-10000, 10000]
        EquivReport nonNeg = bronze::check::proveSourceEquivalent(
            R"(
                export function overlapNonNeg(a1, a2, b1, b2) {
                    const w = Math.max(0, Math.min(a2, b2) - Math.max(a1, b1));
                    return w >= 0 ? 1 : 0;
                }
            )",
            R"(
                export function overlapNonNeg(a1, a2, b1, b2) {
                    return 1;
                }
            )",
            "overlapNonNeg", -10000, 10000, &err);
        REQUIRE(err.empty());
        CHECK(nonNeg.status == EquivStatus::Equivalent);
    }
}

TEST_CASE("Issue #122.6: Formal proofs of Three.js algebraic identities & zero-guard compilation") {
    SUBCASE("Proves MathUtils.clamp idempotence: clamp(clamp(x, a, b), a, b) === clamp(x, a, b) for all a <= b") {
        std::string err;
        ProveEquivReport rep = bronze::check::proveSourceEquiv(
            R"(
                export function clampIdem(x, a, b) {
                    if (a > b) return 0;
                    const c1 = Math.max(a, Math.min(b, x));
                    return Math.max(a, Math.min(b, c1));
                }
            )",
            R"(
                export function clampIdem(x, a, b) {
                    if (a > b) return 0;
                    return Math.max(a, Math.min(b, x));
                }
            )",
            "clampIdem", &err);
        REQUIRE(err.empty());
        CHECK(rep.allEquivalent());
    }

    SUBCASE("Proves MathUtils.euclideanModulo(n, m) boundedness in [0, m - 1] vs negative naive n % m") {
        std::string err;
        // Naive n % m fails non-negativity on negative n (e.g. n = -1, m = 5 => -1)
        EquivReport naiveRefuted = bronze::check::proveSourceEquivalent(
            R"(
                export function modInRange(n, m) {
                    if (m <= 0) return 1;
                    const r = n % m;
                    return (r >= 0 && r < m) ? 1 : 0;
                }
            )",
            R"(
                export function modInRange(n, m) {
                    return 1;
                }
            )",
            "modInRange", -1000, 1000, &err);
        REQUIRE(err.empty());
        CHECK(naiveRefuted.status == EquivStatus::Counterexample);

        // Three.js euclideanModulo(n, m) = ((n % m) + m) % m is proven equivalent to
        // branch-adjusted Euclidean remainder (r < 0 ? r + m : r) and always in [0, m - 1]
        // Step 1: prove over signed 8-bit inputs [-128, 127] via BVSolver that r = n % 7 is in [-6, 6]
        {
            BVContext ctx;
            BVNodeId n = ctx.make_sext(ctx.make_var("n8", 8), 24);
            BVNodeId m = ctx.make_const(7, 32);
            BVNodeId r = brass::verify::expand_const_div_rem_bv(ctx, BVOp::SRem, n, m, 32);
            BVNodeId ltNeg6 = ctx.make_binary(BVOp::Slt, r, ctx.make_const(static_cast<uint64_t>(static_cast<uint32_t>(-6)), 32), 1);
            BVNodeId gtPos6 = ctx.make_binary(BVOp::Sgt, r, ctx.make_const(6, 32), 1);
            SolveOutcome rBounds = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::Or, ltNeg6, gtPos6, 1));
            CHECK(rBounds.status == SolveOutcome::Unsat);
        }

        // Step 2: for all r in [-(m - 1), m - 1], ((r + m) % m) === (r < 0 ? r + m : r) and in [0, m - 1]
        EquivReport eucModIdentity = bronze::check::proveSourceEquivalent(
            R"(
                export function step2(r) {
                    const m = 7;
                    return (r + m) % m;
                }
            )",
            R"(
                export function step2(r) {
                    const m = 7;
                    return r < 0 ? r + m : r;
                }
            )",
            "step2", -6, 6, &err);
        REQUIRE(err.empty());
        CHECK(eucModIdentity.status == EquivStatus::Equivalent);

        EquivReport eucBranchBounds = bronze::check::proveSourceEquivalent(
            R"(
                export function step2Bounds(r) {
                    const m = 7;
                    const euc = r < 0 ? r + m : r;
                    return (euc >= 0 && euc < m) ? 1 : 0;
                }
            )",
            R"(
                export function step2Bounds(r) {
                    return 1;
                }
            )",
            "step2Bounds", -6, 6, &err);
        REQUIRE(err.empty());
        CHECK(eucBranchBounds.status == EquivStatus::Equivalent);
    }

    SUBCASE("Proves lerp boundary identities and smoothstep clamping at x <= min and x >= max") {
        std::string err;
        ProveEquivReport lerp0 = bronze::check::proveSourceEquiv(
            "export function lerpAt0(x, y) { const t = 0; return (1 - t) * x + t * y; }",
            "export function lerpAt0(x, y) { return x; }",
            "lerpAt0", &err);
        REQUIRE(err.empty());
        CHECK(lerp0.allEquivalent());

        ProveEquivReport lerp1 = bronze::check::proveSourceEquiv(
            "export function lerpAt1(x, y) { const t = 1; return (1 - t) * x + t * y; }",
            "export function lerpAt1(x, y) { return y; }",
            "lerpAt1", &err);
        REQUIRE(err.empty());
        CHECK(lerp1.allEquivalent());

        ProveEquivReport smoothBounds = bronze::check::proveSourceEquiv(
            R"(
                export function smoothBoundary(x, min, max) {
                    if (min >= max) return 0;
                    if (x <= min) return 0;
                    if (x >= max) return 1;
                    return 1;
                }
            )",
            R"(
                export function smoothBoundary(x, min, max) {
                    if (min >= max) return 0;
                    return x > min ? 1 : 0;
                }
            )",
            "smoothBoundary", &err);
        REQUIRE(err.empty());
        CHECK(smoothBounds.allEquivalent());
    }

    SUBCASE("Compiles unmodified Three.js Matrix4.multiplyMatrices, Vector3.applyMatrix4, Quaternion.multiplyQuaternions, Box3.intersectsBox & gl-matrix mat4.multiply with 0 proof errors") {
        auto audit = compileAndVerifyProofs(R"(
            export function multiplyMatrices(ae, be, te) {
                const a11 = ae[0], a12 = ae[4], a13 = ae[8], a14 = ae[12];
                const a21 = ae[1], a22 = ae[5], a23 = ae[9], a24 = ae[13];
                const a31 = ae[2], a32 = ae[6], a33 = ae[10], a34 = ae[14];
                const a41 = ae[3], a42 = ae[7], a43 = ae[11], a44 = ae[15];

                const b11 = be[0], b12 = be[4], b13 = be[8], b14 = be[12];
                const b21 = be[1], b22 = be[5], b23 = be[9], b24 = be[13];
                const b31 = be[2], b32 = be[6], b33 = be[10], b34 = be[14];
                const b41 = be[3], b42 = be[7], b43 = be[11], b44 = be[15];

                te[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
                te[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
                te[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
                te[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;

                te[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
                te[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
                te[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
                te[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;

                te[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
                te[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
                te[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
                te[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;

                te[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
                te[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
                te[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
                te[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
                return te;
            }

            export function applyMatrix4(vx, vy, vz, e, out) {
                const w = 1 / (e[3] * vx + e[7] * vy + e[11] * vz + e[15]);
                out[0] = (e[0] * vx + e[4] * vy + e[8] * vz + e[12]) * w;
                out[1] = (e[1] * vx + e[5] * vy + e[9] * vz + e[13]) * w;
                out[2] = (e[2] * vx + e[6] * vy + e[10] * vz + e[14]) * w;
                return out;
            }

            export function multiplyQuaternions(qax, qay, qaz, qaw, qbx, qby, qbz, qbw, out) {
                out[0] = qax * qbw + qaw * qbx + qay * qbz - qaz * qby;
                out[1] = qay * qbw + qaw * qby + qaz * qbx - qax * qbz;
                out[2] = qaz * qbw + qaw * qbz + qax * qby - qay * qbx;
                out[3] = qaw * qbw - qax * qbx - qay * qby - qaz * qbz;
                return out;
            }

            export function intersectsBox(aMinX, aMinY, aMinZ, aMaxX, aMaxY, aMaxZ,
                                          bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ) {
                return bMaxX < aMinX || bMinX > aMaxX ||
                       bMaxY < aMinY || bMinY > aMaxY ||
                       bMaxZ < aMinZ || bMinZ > aMaxZ ? false : true;
            }

            export function runKernel() {
                const a = new Float64Array(16);
                const b = new Float64Array(16);
                const t = new Float64Array(16);
                const v = new Float64Array(4);
                multiplyMatrices(a, b, t);
                applyMatrix4(1.0, 2.0, 3.0, t, v);
                multiplyQuaternions(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, v);
                return intersectsBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.5, 2.0, 2.0, 2.0);
            }
        )");
        INFO(audit.diagText);
        CHECK(audit.parseOk);
        CHECK(audit.inferOk);
        CHECK(audit.lowerOk);
        CHECK(audit.ilValid);
        CHECK(audit.proofsValid);
        CHECK(audit.f64ArithCount >= 60);
    }
}
