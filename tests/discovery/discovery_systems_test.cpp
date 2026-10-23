#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
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

// Reference Three.js DataUtils.fromHalfFloat (16-bit IEEE-754 binary16 -> binary32 float)
float referenceFromHalfFloat(uint16_t val) {
    uint32_t s = static_cast<uint32_t>(val & 0x8000u) << 16;
    uint32_t e = (val >> 10) & 0x1Fu;
    uint32_t m = val & 0x03FFu;
    uint32_t outBits = 0;

    if (e == 0) {
        if (m == 0) {
            outBits = s;
        } else {
            // Subnormal half-float: normalize mantissa until leading bit reaches bit 10
            uint32_t exp32 = 127 - 14;
            while ((m & 0x0400u) == 0) {
                m <<= 1;
                --exp32;
            }
            m &= 0x03FFu;
            outBits = s | (exp32 << 23) | (m << 13);
        }
    } else if (e == 31) {
        outBits = s | 0x7F800000u | (m << 13);
    } else {
        outBits = s | ((e + 112u) << 23) | (m << 13);
    }

    float f = 0.0f;
    std::memcpy(&f, &outBits, sizeof(float));
    return f;
}

// Reference IEEE-754 binary32 -> binary16 (Three.js DataUtils.toHalfFloat with subnormal & RNE support)
uint16_t referenceToHalfFloat(float val) {
    uint32_t fbits = 0;
    std::memcpy(&fbits, &val, sizeof(float));

    uint16_t sign = static_cast<uint16_t>((fbits >> 16) & 0x8000u);
    int32_t exp32 = static_cast<int32_t>((fbits >> 23) & 0xFFu);
    uint32_t mant32 = fbits & 0x007FFFFFu;

    if (exp32 == 255) {
        if (mant32 == 0) return sign | 0x7C00u;
        uint16_t nanPayload = static_cast<uint16_t>(mant32 >> 13);
        if (nanPayload == 0) nanPayload = 1;
        return sign | 0x7C00u | nanPayload;
    }

    int32_t exp16 = exp32 - 127 + 15;
    if (exp16 >= 31) {
        return sign | 0x7C00u;
    }
    if (exp16 <= 0) {
        if (exp16 < -10) {
            return sign;
        }
        uint32_t fullMant = mant32 | 0x00800000u;
        uint32_t shift = static_cast<uint32_t>(14 - exp16);
        uint32_t halfMant = fullMant >> shift;
        uint32_t remMask = (1u << shift) - 1u;
        uint32_t rem = fullMant & remMask;
        uint32_t halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (halfMant & 1u) != 0)) {
            ++halfMant;
        }
        return static_cast<uint16_t>(sign + halfMant);
    }

    uint32_t halfMant = mant32 >> 13;
    uint32_t rem = mant32 & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (halfMant & 1u) != 0)) {
        ++halfMant;
    }
    // Note: using addition (+) rather than bitwise OR (|) allows mantissa carry
    // (0x0400) to increment exp16 cleanly (and overflow exp16=30 into 0x7C00 = Inf).
    return static_cast<uint16_t>(sign + (static_cast<uint32_t>(exp16) << 10) + halfMant);
}

// ITU-T G.711 mu-Law 8-bit decoder -> 16-bit signed linear PCM
int32_t decodeMuLawSample(uint8_t uVal) {
    uint8_t u = static_cast<uint8_t>(~uVal);
    int32_t sign = (u & 0x80u) != 0;
    int32_t exponent = (u >> 4) & 0x07;
    int32_t mantissa = u & 0x0F;
    int32_t step = ((2 * mantissa + 33) << exponent) - 33;
    return sign ? -step : step;
}

// ITU-T G.711 mu-Law encoder from decoded segment sample -> 8-bit codeword
uint8_t encodeMuLawSample(int32_t pcm) {
    uint8_t signBit = 0;
    if (pcm < 0) {
        signBit = 0x80u;
        pcm = -pcm;
    }
    int32_t shifted = pcm + 33;
    int32_t exponent = 7;
    for (int32_t e = 7; e >= 0; --e) {
        if ((shifted & (32 << e)) != 0) {
            exponent = e;
            break;
        }
    }
    int32_t mantissa = (shifted >> (exponent + 1)) & 0x0F;
    uint8_t packed = static_cast<uint8_t>(signBit | (exponent << 4) | mantissa);
    return static_cast<uint8_t>(~packed);
}

}  // namespace

TEST_CASE("Issue #126.1: PRNG & Hash Finalizer 2^32 Bijectivity, Xorshift32 Fixed Point & Procedural Noise Indexing") {
    SUBCASE("Proves Murmur3 / SplitMix32 avalanching xorshift steps and odd-multiplier modular inverses are bijections over all 2^32 states") {
        std::string err;

        // 1. Prove x ^ (x >>> 16) is an exact involution (self-inverse) over all 2^32 states
        EquivReport xor16 = bronze::check::proveRoundTripSource(
            R"(
                export function avalanche16(x) {
                    return (x ^ (x >>> 16)) | 0;
                }
                export function unavalanche16(y) {
                    return (y ^ (y >>> 16)) | 0;
                }
            )",
            "avalanche16", "unavalanche16", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(xor16.status == EquivStatus::Equivalent);

        // 2. Prove x ^ (x >>> 13) is inverted over all 2^32 states by y ^ (y >>> 13) ^ (y >>> 26)
        EquivReport xor13 = bronze::check::proveRoundTripSource(
            R"(
                export function avalanche13(x) {
                    return (x ^ (x >>> 13)) | 0;
                }
                export function unavalanche13(y) {
                    let v = y ^ (y >>> 13);
                    v = v ^ (v >>> 26);
                    return v | 0;
                }
            )",
            "avalanche13", "unavalanche13", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(xor13.status == EquivStatus::Equivalent);

        // 3. Prove Murmur3's odd multipliers 0x85ebca6b and 0xc2b2ae35 have exact multiplicative inverses
        // mod 2^32 (0xa5cb9243 and 0x7ed1b41d), establishing that every stage of fmix32 is a bijection on Z_{2^32}
        CHECK(static_cast<uint32_t>(0x85ebca6bu * 0xa5cb9243u) == 1u);
        CHECK(static_cast<uint32_t>(0xc2b2ae35u * 0x7ed1b41du) == 1u);

        // Verify full 5-stage fmix32_inv(fmix32(h)) === h round-trip
        auto fmix32 = [](uint32_t h) -> uint32_t {
            h ^= h >> 16;
            h *= 0x85ebca6bu;
            h ^= h >> 13;
            h *= 0xc2b2ae35u;
            h ^= h >> 16;
            return h;
        };
        auto fmix32Inv = [](uint32_t h) -> uint32_t {
            h ^= h >> 16;
            h *= 0x7ed1b41du;
            h ^= (h >> 13) ^ (h >> 26);
            h *= 0xa5cb9243u;
            h ^= h >> 16;
            return h;
        };
        for (uint32_t seed : {0u, 1u, 42u, 0x7FFFFFFFu, 0x80000000u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
            CHECK(fmix32Inv(fmix32(seed)) == seed);
        }
    }

    SUBCASE("BVSolver synthesizes collisions when a hash finalizer multiplies by an even constant or omits Math.imul") {
        // If a developer accidentally uses an even constant (e.g. 0x85ebca6a instead of 0x85ebca6b),
        // multiplication mod 2^32 is no longer injective: h and (h ^ 0x80000000) collide!
        BVContext ctx;
        BVNodeId a = ctx.make_var("a", 32);
        BVNodeId b = ctx.make_var("b", 32);
        BVNodeId evenConst = ctx.make_const(0x85ebca6au, 32);
        BVNodeId hashA = ctx.make_binary(BVOp::Mul, a, evenConst, 32);
        BVNodeId hashB = ctx.make_binary(BVOp::Mul, b, evenConst, 32);

        BVNodeId distinct = ctx.make_binary(BVOp::Ne, a, b, 1);
        BVNodeId collide = ctx.make_binary(BVOp::Eq, hashA, hashB, 1);
        SolveOutcome res = brass::verify::solve_nonzero(ctx, ctx.make_binary(BVOp::And, distinct, collide, 1));
        CHECK(res.status == SolveOutcome::Sat);
        REQUIRE(res.model.size() >= 2);
        uint32_t witA = static_cast<uint32_t>(res.model[0]);
        uint32_t witB = static_cast<uint32_t>(res.model[1]);
        CHECK(witA != witB);
        CHECK(static_cast<uint32_t>(witA * 0x85ebca6au) == static_cast<uint32_t>(witB * 0x85ebca6au));

        // Prove that naive IEEE-754 double multiplication ((x * 0x85ebca6b) | 0) diverges from Math.imul
        // when x * 0x85ebca6b exceeds Number.MAX_SAFE_INTEGER (2^53 - 1), e.g., x = 0x7FFFFFFF:
        double exactProd = static_cast<double>(0x7FFFFFFFu) * static_cast<double>(0x85ebca6bu);
        CHECK(exactProd > 9007199254740991.0);  // > 2^53 - 1
        int32_t doubleCoerced = static_cast<int32_t>(static_cast<int64_t>(exactProd));
        int32_t imulExact = static_cast<int32_t>(0x7FFFFFFFu * 0x85ebca6bu);
        CHECK(doubleCoerced != imulExact);
    }

    SUBCASE("Proves Marsaglia xorshift32 is a bijection on 2^32 and x === 0 is its unique absorbing zero state") {
        std::string err;

        // 1. Prove xorshift32_inv(xorshift32(x)) === x across all 2^32 32-bit states!
        //    By repeated squaring in GF(2)[L]:
        //    - Inverse of (s ^= s << 5)  is sequential: s ^= s << 5; s ^= s << 10; s ^= s << 20;
        //    - Inverse of (s ^= s >>> 17) is:           s ^= s >>> 17;
        //    - Inverse of (s ^= s << 13) is sequential: s ^= s << 13; s ^= s << 26;
        EquivReport xorshiftBij = bronze::check::proveRoundTripSource(
            R"(
                export function xorshift32(x) {
                    let s = x | 0;
                    s ^= s << 13;
                    s ^= s >>> 17;
                    s ^= s << 5;
                    return s | 0;
                }
                export function xorshift32Inv(y) {
                    let s = y | 0;
                    s ^= s << 5;
                    s ^= s << 10;
                    s ^= s << 20;
                    s ^= s >>> 17;
                    s ^= s << 13;
                    s ^= s << 26;
                    return s | 0;
                }
            )",
            "xorshift32", "xorshift32Inv", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(xorshiftBij.status == EquivStatus::Equivalent);

        // 2. Prove via BVSolver that x === 0 is the ONLY state where xorshift32(x) === x (absorbing trap)
        BVContext ctx;
        BVNodeId x = ctx.make_var("x", 32);
        BVNodeId s1 = ctx.make_binary(BVOp::Xor, x, ctx.make_binary(BVOp::Shl, x, ctx.make_const(13, 32), 32), 32);
        BVNodeId s2 = ctx.make_binary(BVOp::Xor, s1, ctx.make_binary(BVOp::LShr, s1, ctx.make_const(17, 32), 32), 32);
        BVNodeId s3 = ctx.make_binary(BVOp::Xor, s2, ctx.make_binary(BVOp::Shl, s2, ctx.make_const(5, 32), 32), 32);

        // Query: can x != 0 produce xorshift32(x) == 0 or xorshift32(x) == x?
        BVNodeId nonZeroX = ctx.make_binary(BVOp::Ne, x, ctx.make_const(0, 32), 1);
        BVNodeId outZeroOrFixed = ctx.make_binary(
            BVOp::Or,
            ctx.make_binary(BVOp::Eq, s3, ctx.make_const(0, 32), 1),
            ctx.make_binary(BVOp::Eq, s3, x, 1), 1);
        SolveOutcome fixedPointCheck = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::And, nonZeroX, outZeroOrFixed, 1));
        CHECK(fixedPointCheck.status == SolveOutcome::Unsat);
    }

    SUBCASE("Refutes Perlin/Simplex perm[xi % 256] negative coordinate OOB and catches Box-Muller u1 === 0 singularity") {
        std::string err;

        // Perlin/Simplex permutation table indexing: naive xi % 256 returns negative index on xi < 0!
        EquivReport modVsMask = bronze::check::proveSourceEquivalent(
            "export function permIdx(xi) { return xi % 256; }",
            "export function permIdx(xi) { return xi & 255; }",
            "permIdx", -1000, 1000, &err);
        REQUIRE(err.empty());
        CHECK(modVsMask.status == EquivStatus::Counterexample);
        CHECK(modVsMask.counterexampleInput < 0);
        CHECK(modVsMask.expectedOutput < 0);
        CHECK(modVsMask.actualOutput >= 0);
        CHECK(modVsMask.actualOutput <= 255);

        // Prove (xi & 255) is strictly in [0, 255] across all 2^32 signed 32-bit coordinates
        EquivReport maskBounded = bronze::check::proveSourceEquivalent(
            "export function inBounds(xi) { const idx = xi & 255; return (idx >= 0 && idx <= 255) ? 1 : 0; }",
            "export function inBounds(xi) { return 1; }",
            "inBounds", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(maskBounded.status == EquivStatus::Equivalent);

        // Box-Muller transform: Math.sqrt(-2 * Math.log(u1)) when u1 === 0 (from Math.random() in [0, 1))
        const char* unguardedBoxMuller = R"(
            export function boxMullerRadius(u1) {
                return Math.sqrt(-2.0 * Math.log(u1));
            }
        )";
        CheckReport bmRep = bronze::check::checkSource(unguardedBoxMuller, true, &err);
        REQUIRE(err.empty());
        CHECK(hasFindingKind(bmRep, BugKind::NaNPoison));
        CHECK(bmRep.domainErrorCount >= 1);

        SynthTestsReport bmSynth = bronze::check::synthesizeSourceTests(unguardedBoxMuller, &err);
        REQUIRE(err.empty());
        CHECK(bmSynth.hazardReproducerCount >= 1);
    }
}

TEST_CASE("Issue #126.2: Exhaustive 2^16 IEEE-754 Float16 <-> Float32 Pack/Unpack Audit (Three.js DataUtils)") {
    SUBCASE("Exhaustively verifies all 65,536 (2^16) 16-bit half-float bit patterns for round-trip identity and monotonicity") {
        uint32_t normalCount = 0;
        uint32_t subnormalCount = 0;
        uint32_t zeroCount = 0;
        uint32_t infCount = 0;
        uint32_t nanCount = 0;

        float prevPos = 0.0f;
        float prevNeg = 0.0f;

        for (uint32_t h32 = 0; h32 <= 0xFFFFu; ++h32) {
            uint16_t h = static_cast<uint16_t>(h32);
            uint16_t exp16 = (h >> 10) & 0x1Fu;
            uint16_t mant16 = h & 0x03FFu;
            float f = referenceFromHalfFloat(h);
            uint16_t roundTrip = referenceToHalfFloat(f);

            if (exp16 == 0 && mant16 == 0) {
                ++zeroCount;
                CHECK(f == 0.0f);
                CHECK(std::signbit(f) == ((h & 0x8000u) != 0));
                CHECK(roundTrip == h);
            } else if (exp16 == 0) {
                ++subnormalCount;
                CHECK(std::isfinite(f));
                CHECK(f != 0.0f);
                CHECK(roundTrip == h);
            } else if (exp16 == 31 && mant16 == 0) {
                ++infCount;
                CHECK(std::isinf(f));
                CHECK(std::signbit(f) == ((h & 0x8000u) != 0));
                CHECK(roundTrip == h);
            } else if (exp16 == 31) {
                ++nanCount;
                CHECK(std::isnan(f));
                CHECK((roundTrip & 0x7C00u) == 0x7C00u);
                CHECK((roundTrip & 0x03FFu) != 0);
            } else {
                ++normalCount;
                CHECK(std::isfinite(f));
                CHECK(roundTrip == h);
            }

            // Strict monotonicity across positive [0x0000..0x7C00] and negative [0x8000..0xFC00]
            if (h > 0x0000u && h <= 0x7C00u) {
                CHECK(f > prevPos);
                prevPos = f;
            } else if (h == 0x0000u) {
                prevPos = f;
            }
            if (h > 0x8000u && h <= 0xFC00u) {
                CHECK(f < prevNeg);
                prevNeg = f;
            } else if (h == 0x8000u) {
                prevNeg = f;
            }
        }

        CHECK(zeroCount == 2);
        CHECK(subnormalCount == 2046);
        CHECK(normalCount == 61440);
        CHECK(infCount == 2);
        CHECK(nanCount == 2046);
        CHECK(zeroCount + subnormalCount + normalCount + infCount + nanCount == 65536);
    }

    SUBCASE("BVSolver proves normal-range Float16 <-> Float32 bit re-biasing and exposes mantissa-carry OR bug") {
        std::string err;

        // 1. Prove across all normal 16-bit half-floats (exp16 in [1, 30]) that unpacking
        //    to IEEE-754 binary32 bits and re-packing is the exact identity:
        BVContext ctx;
        BVNodeId h = ctx.make_var("h16", 16);
        BVNodeId h32 = ctx.make_zext(h, 16);
        BVNodeId sign16 = ctx.make_binary(BVOp::And, h32, ctx.make_const(0x8000u, 32), 32);
        BVNodeId exp16 = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::LShr, h32, ctx.make_const(10, 32), 32), ctx.make_const(0x1Fu, 32), 32);
        BVNodeId mant16 = ctx.make_binary(BVOp::And, h32, ctx.make_const(0x03FFu, 32), 32);

        BVNodeId isNormal = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Uge, exp16, ctx.make_const(1, 32), 1),
            ctx.make_binary(BVOp::Ule, exp16, ctx.make_const(30, 32), 1), 1);

        // Unpack to binary32: (sign16 << 16) | ((exp16 + 112) << 23) | (mant16 << 13)
        BVNodeId f32Bits = ctx.make_binary(
            BVOp::Or,
            ctx.make_binary(BVOp::Shl, sign16, ctx.make_const(16, 32), 32),
            ctx.make_binary(
                BVOp::Or,
                ctx.make_binary(BVOp::Shl, ctx.make_binary(BVOp::Add, exp16, ctx.make_const(112, 32), 32),
                                ctx.make_const(23, 32), 32),
                ctx.make_binary(BVOp::Shl, mant16, ctx.make_const(13, 32), 32), 32),
            32);

        // Re-pack to binary16
        BVNodeId recSign = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::LShr, f32Bits, ctx.make_const(16, 32), 32), ctx.make_const(0x8000u, 32), 32);
        BVNodeId recExp32 = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::LShr, f32Bits, ctx.make_const(23, 32), 32), ctx.make_const(0xFFu, 32), 32);
        BVNodeId recExp16 = ctx.make_binary(BVOp::Sub, recExp32, ctx.make_const(112, 32), 32);
        BVNodeId recMant16 = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::LShr, f32Bits, ctx.make_const(13, 32), 32), ctx.make_const(0x03FFu, 32), 32);
        BVNodeId recH = ctx.make_binary(
            BVOp::Or, recSign,
            ctx.make_binary(BVOp::Or, ctx.make_binary(BVOp::Shl, recExp16, ctx.make_const(10, 32), 32), recMant16, 32), 32);

        SolveOutcome normalRt = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::And, isNormal, ctx.make_binary(BVOp::Ne, recH, h32, 1), 1));
        CHECK(normalRt.status == SolveOutcome::Unsat);

        // 2. Expose the classic Float16 rounding-carry bug:
        //    When mant16 == 0x03FF (all 10 mantissa bits set) and round-to-nearest-even increments mant16 to 0x0400,
        //    packing with bitwise OR `(exp16 << 10) | (mant16 + 1)` instead of addition `(exp16 << 10) + (mant16 + 1)`
        //    fails whenever bit 0 of exp16 is 1 (e.g. exp16 = 1 -> 0x0400 | 0x0400 = 0x0400 instead of 0x0800)!
        EquivReport carryBug = bronze::check::proveSourceEquivalent(
            R"(
                export function packRounded(exp16) {
                    const roundedMant = 0x03FF + 1;
                    return (exp16 << 10) | roundedMant;
                }
            )",
            R"(
                export function packRounded(exp16) {
                    const roundedMant = 0x03FF + 1;
                    return (exp16 << 10) + roundedMant;
                }
            )",
            "packRounded", 1, 30, &err);
        REQUIRE(err.empty());
        CHECK(carryBug.status == EquivStatus::Counterexample);
        CHECK((carryBug.counterexampleInput & 1) == 1);
        CHECK(carryBug.expectedOutput == carryBug.actualOutput - 0x0400);
    }
}

TEST_CASE("Issue #126.3: IPv4 / CIDR Subnet Masking /0 Shift Count Wrap & Signed Octet Comparison Bugs") {
    SUBCASE("BVSolver refutes ~0 << (32 - prefixLen) on /0 CIDR blocks and proves the guarded unsigned mask") {
        std::string err;

        // Naive ~0 << (32 - prefixLen) or ~((1 << (32 - prefixLen)) - 1) fails at prefixLen === 0
        // because JS bitwise shifts mask the shift count by & 31: 32 & 31 === 0, so ~0 << 32 === -1 (255.255.255.255)!
        EquivReport cidrBug = bronze::check::proveSourceEquivalent(
            R"(
                export function cidrMask(prefixLen) {
                    return (~0 << (32 - prefixLen)) >>> 0;
                }
            )",
            R"(
                export function cidrMask(prefixLen) {
                    return prefixLen === 0 ? 0 : ((~0 << (32 - prefixLen)) >>> 0);
                }
            )",
            "cidrMask", 0, 32, &err);
        REQUIRE(err.empty());
        CHECK(cidrBug.status == EquivStatus::Counterexample);
        CHECK(cidrBug.counterexampleInput == 0);
        CHECK(static_cast<uint32_t>(cidrBug.expectedOutput) == 0xFFFFFFFFu);
        CHECK(cidrBug.actualOutput == 0);

        // Prove that for all prefixLen in [0, 32], the bitwise inversion of the guarded mask (~mask)
        // is a contiguous low-bit host mask satisfying (hostMask & (hostMask + 1)) === 0
        EquivReport contiguousMask = bronze::check::proveSourceEquivalent(
            R"(
                export function isContiguousMask(prefixLen) {
                    const mask = prefixLen === 0 ? 0 : (~0 << (32 - prefixLen));
                    const hostMask = ~mask;
                    return (hostMask & (hostMask + 1)) === 0 ? 1 : 0;
                }
            )",
            R"(
                export function isContiguousMask(prefixLen) {
                    return 1;
                }
            )",
            "isContiguousMask", 0, 32, &err);
        REQUIRE(err.empty());
        CHECK(contiguousMask.status == EquivStatus::Equivalent);
    }

    SUBCASE("BVSolver refutes signed 32-bit IPv4 comparison when first octet >= 128 and proves unsigned >>> 0 fix") {
        // When comparing packed 32-bit IPv4 addresses (e.g. 192.168.1.1 = 0xC0A80101 = -1062731519 in signed int32),
        // signed comparison ip >= startIp fails whenever startIp < 128.0.0.0 and ip >= 128.0.0.0!
        BVContext ctx;
        BVNodeId ip = ctx.make_var("ip", 32);
        BVNodeId startIp = ctx.make_const(0x0A000000u, 32);  // 10.0.0.0
        BVNodeId endIp = ctx.make_const(0xC0A8FFFFu, 32);    // 192.168.255.255

        BVNodeId signedInRange = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Sge, ip, startIp, 1),
            ctx.make_binary(BVOp::Sle, ip, endIp, 1), 1);
        BVNodeId unsignedInRange = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Uge, ip, startIp, 1),
            ctx.make_binary(BVOp::Ule, ip, endIp, 1), 1);

        SolveOutcome mismatch = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::Ne, signedInRange, unsignedInRange, 1));
        CHECK(mismatch.status == SolveOutcome::Sat);
        REQUIRE(!mismatch.model.empty());
        uint32_t witIp = static_cast<uint32_t>(mismatch.model[0]);
        CHECK((witIp >= 0x0A000000u && witIp <= 0xC0A8FFFFu) !=
              (static_cast<int32_t>(witIp) >= static_cast<int32_t>(0x0A000000u) &&
               static_cast<int32_t>(witIp) <= static_cast<int32_t>(0xC0A8FFFFu)));
    }
}

TEST_CASE("Issue #126.4: Web Audio DSP — G.711 mu-Law / IMA ADPCM Codecs & RBJ Biquad Filter Stability") {
    SUBCASE("Exhaustively and symbolically proves G.711 mu-Law 8-bit monotonicity, antisymmetry, and round-trip") {
        // 1. Exhaustive verification over all 256 8-bit G.711 mu-Law codewords
        for (uint32_t u = 0; u <= 255; ++u) {
            uint8_t code = static_cast<uint8_t>(u);
            int32_t pcm = decodeMuLawSample(code);
            int32_t negPcm = decodeMuLawSample(static_cast<uint8_t>(code ^ 0x80u));
            CHECK(pcm == -negPcm);

            uint8_t reencoded = encodeMuLawSample(pcm);
            if (code == 0x7Fu) {
                // Negative zero (0x7F) decodes to 0 and canonicalizes to positive zero (0xFF)
                CHECK(pcm == 0);
                CHECK(reencoded == 0xFFu);
            } else {
                CHECK(reencoded == code);
            }
        }

        // 2. Prove via BVSolver across all 8-bit mu-Law codes that decodeMuLaw is strictly monotonic
        //    across positive codes (0xFF down to 0x80)
        std::string err;
        EquivReport muMono = bronze::check::proveSourceEquivalent(
            R"(
                export function isMuLawMonotonic(magIdx) {
                    const exp1 = (magIdx >> 4) & 7;
                    const mant1 = magIdx & 15;
                    const step1 = ((2 * mant1 + 33) << exp1) - 33;

                    const nextIdx = magIdx + 1;
                    const exp2 = (nextIdx >> 4) & 7;
                    const mant2 = nextIdx & 15;
                    const step2 = ((2 * mant2 + 33) << exp2) - 33;
                    return step2 > step1 ? 1 : 0;
                }
            )",
            R"(
                export function isMuLawMonotonic(magIdx) {
                    return 1;
                }
            )",
            "isMuLawMonotonic", 0, 126, &err);
        REQUIRE(err.empty());
        CHECK(muMono.status == EquivStatus::Equivalent);
    }

    SUBCASE("Proves IMA ADPCM step-index [0, 88] clamping safety vs unguarded stepTable[89] overflow") {
        std::string err;

        // IMA ADPCM index step table for magnitude nibbles 0..7 is [-1, -1, -1, -1, 2, 4, 6, 8].
        // Without clamping to [0, 88], index = 85 + 8 = 93 overflows stepTable[89]!
        EquivReport adpcmUnclamped = bronze::check::proveSourceEquivalent(
            R"(
                export function stepIndexInBounds(idx) {
                    const nextIdx = idx + 8;
                    return (nextIdx >= 0 && nextIdx <= 88) ? 1 : 0;
                }
            )",
            R"(
                export function stepIndexInBounds(idx) {
                    return 1;
                }
            )",
            "stepIndexInBounds", 0, 88, &err);
        REQUIRE(err.empty());
        CHECK(adpcmUnclamped.status == EquivStatus::Counterexample);
        CHECK(adpcmUnclamped.counterexampleInput >= 81);

        EquivReport adpcmClamped = bronze::check::proveSourceEquivalent(
            R"(
                export function clampedStepIndexInBounds(idx) {
                    const delta = 8;
                    const nextIdx = Math.max(0, Math.min(88, idx + delta));
                    return (nextIdx >= 0 && nextIdx <= 88) ? 1 : 0;
                }
            )",
            R"(
                export function clampedStepIndexInBounds(idx) {
                    return 1;
                }
            )",
            "clampedStepIndexInBounds", 0, 88, &err);
        REQUIRE(err.empty());
        CHECK(adpcmClamped.status == EquivStatus::Equivalent);
    }

    SUBCASE("Detects RBJ Audio EQ Cookbook Biquad Q === 0 singularity and proves Jury pole-stability bounds") {
        std::string err;

        // 1. Unguarded RBJ low-pass biquad coefficient calculation divides by 2 * Q and by a0 = 1 + alpha:
        const char* unguardedBiquad = R"(
            export function rbjLowpassA2(sinW0, cosW0, Q) {
                const alpha = sinW0 / (2.0 * Q);
                const a0 = 1.0 + alpha;
                const a2 = 1.0 - alpha;
                return a2 / a0;
            }
        )";
        CheckReport bqRep = bronze::check::checkSource(unguardedBiquad, true, &err);
        REQUIRE(err.empty());
        CHECK(hasFindingKind(bqRep, BugKind::NaNPoison));
        CHECK(bqRep.divByZeroCount >= 1);

        // 2. Prove via BVSolver that for any fixed-point alpha > 0 (e.g. alphaScaled in [1, 10000]
        //    with unity = 1000 so a0 = 1000 + alpha > 1000 and |a2| = |1000 - alpha| < a0),
        //    the Jury stability bound -a0 < a2 && a2 < a0 holds unconditionally!
        EquivReport juryA2 = bronze::check::proveSourceEquivalent(
            R"(
                export function juryPoleRadiusStable(alphaScaled) {
                    const a0 = 1000 + alphaScaled;
                    const a2 = 1000 - alphaScaled;
                    return (a2 > -a0 && a2 < a0) ? 1 : 0;
                }
            )",
            R"(
                export function juryPoleRadiusStable(alphaScaled) {
                    return 1;
                }
            )",
            "juryPoleRadiusStable", 1, 10000, &err);
        REQUIRE(err.empty());
        CHECK(juryA2.status == EquivStatus::Equivalent);
    }
}
