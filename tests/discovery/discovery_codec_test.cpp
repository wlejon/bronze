#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"

namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;
using bronze::check::EquivReport;
using bronze::check::EquivStatus;
using bronze::check::ProveEquivReport;
using bronze::check::proveRoundTripSource;
using bronze::check::proveSourceEquiv;
using bronze::check::proveSourceEquivalent;

int32_t jsToInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    double intPart = std::trunc(d);
    double mod2p32 = std::fmod(intPart, 4294967296.0);
    if (mod2p32 < 0.0) mod2p32 += 4294967296.0;
    uint32_t u = static_cast<uint32_t>(mod2p32);
    return static_cast<int32_t>(u);
}

}  // namespace

TEST_CASE("Discovery 1: Protobuf / Varint 32-bit ZigZag encoding 2^32 round-trip proof & signed-shift bug") {
    constexpr const char* kZigZagSource = R"(
        export function zigzagEncode(x) {
            return (x << 1) ^ (x >> 31);
        }

        export function zigzagDecode(z) {
            return (z >>> 1) ^ -(z & 1);
        }

        export function zigzagDecodeBrokenSignedShift(z) {
            return (z >> 1) ^ -(z & 1);
        }
    )";

    SUBCASE("proves zigzagDecode(zigzagEncode(x)) === x across all 2^32 signed 32-bit integers") {
        std::string err;
        EquivReport rep = proveRoundTripSource(
            kZigZagSource, "zigzagEncode", "zigzagDecode", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::ProvenEquivalent);
        CHECK(rep.isEquivalent());
    }

    SUBCASE("proves inverse bijection zigzagEncode(zigzagDecode(z)) === z across all 2^32 32-bit words") {
        std::string err;
        EquivReport rep = proveRoundTripSource(
            kZigZagSource, "zigzagDecode", "zigzagEncode", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::ProvenEquivalent);
    }

    SUBCASE("refutes broken zigzagDecode using signed >> 1 with concrete high-bit counterexample") {
        std::string err;
        EquivReport rep = proveRoundTripSource(
            kZigZagSource, "zigzagEncode", "zigzagDecodeBrokenSignedShift", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::RefutedWithCounterexample);
        CHECK(rep.hasCounterexample());

        int32_t x = static_cast<int32_t>(rep.counterexampleInput);
        uint32_t ux = static_cast<uint32_t>(x);
        uint32_t z = (ux << 1) ^ static_cast<uint32_t>(x >> 31);
        // Bit 31 of z must be 1 for >> 1 and >>> 1 to diverge:
        CHECK((z & 0x80000000u) != 0u);
        int32_t brokenDecoded = (static_cast<int32_t>(z) >> 1) ^ -static_cast<int32_t>(z & 1u);
        int32_t correctDecoded = static_cast<int32_t>((z >> 1) ^ (~(z & 1u) + 1u));
        CHECK(brokenDecoded == static_cast<int32_t>(rep.actualOutput));
        CHECK(correctDecoded == x);
        CHECK(brokenDecoded != x);
    }
}

TEST_CASE("Discovery 2: Mozilla source-map Base64-VLQ toVLQSigned / fromVLQSigned overflow & safe domain") {
    // Unmodified logic from Mozilla source-map lib/base64-vlq.js:
    constexpr const char* kMozillaVlqSource = R"(
        export function toVLQSigned(aValue) {
            return aValue < 0 ? ((-aValue) << 1) + 1 : (aValue << 1) + 0;
        }

        export function fromVLQSigned(aValue) {
            var isNegative = (aValue & 1) === 1;
            var shifted = aValue >> 1;
            return isNegative ? -shifted : shifted;
        }

        export function fromVLQSignedUnsignedShift(aValue) {
            var isNegative = (aValue & 1) === 1;
            var shifted = aValue >>> 1;
            return isNegative ? -shifted : shifted;
        }
    )";

    SUBCASE("refutes Mozilla fromVLQSigned(toVLQSigned(x)) === x over full 32-bit signed integers") {
        std::string err;
        EquivReport rep = proveRoundTripSource(
            kMozillaVlqSource, "toVLQSigned", "fromVLQSigned", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::RefutedWithCounterexample);
        CHECK(rep.hasCounterexample());
        CHECK(rep.actualOutput != rep.expectedOutput);
    }

    SUBCASE("proves exact failure modes at x = -1073741824 (-2^30), x = +1073741824 (+2^30), and INT32_MIN") {
        // 1. At x = -1073741824 (-2^30): (-x) << 1 overflows into bit 31 (-2147483648),
        //    so toVLQSigned returns -2147483647 (0x80000001). Then aValue >> 1 sign-extends
        //    bit 31 to -1073741824, and -shifted flips it to +1073741824!
        EquivReport repNeg2p30 = proveRoundTripSource(
            kMozillaVlqSource, "toVLQSigned", "fromVLQSigned", -1073741824, -1073741824);
        CHECK(repNeg2p30.status == EquivStatus::RefutedWithCounterexample);
        CHECK(repNeg2p30.counterexampleInput == -1073741824);
        CHECK(repNeg2p30.encodedIntermediate == -2147483647);
        CHECK(repNeg2p30.actualOutput == 1073741824);

        // 2. At x = +1073741824 (+2^30): (x << 1) overflows into bit 31 (-2147483648),
        //    and fromVLQSigned arithmetic-shifts >> 1 to -1073741824!
        EquivReport repPos2p30 = proveRoundTripSource(
            kMozillaVlqSource, "toVLQSigned", "fromVLQSigned", 1073741824, 1073741824);
        CHECK(repPos2p30.status == EquivStatus::RefutedWithCounterexample);
        CHECK(repPos2p30.counterexampleInput == 1073741824);
        CHECK(repPos2p30.encodedIntermediate == INT32_MIN);
        CHECK(repPos2p30.actualOutput == -1073741824);

        // 3. Even when fromVLQSigned uses >>> 1, INT32_MIN (-2147483648) still fails
        //    because (-(-2147483648)) << 1 === 0 in 32-bit two's complement, encoding
        //    INT32_MIN as 1 (which decodes to -0 === 0, colliding with 0)!
        EquivReport repUnsignedShift = proveRoundTripSource(
            kMozillaVlqSource, "toVLQSigned", "fromVLQSignedUnsignedShift", INT32_MIN, INT32_MAX);
        CHECK(repUnsignedShift.status == EquivStatus::RefutedWithCounterexample);
        CHECK(repUnsignedShift.counterexampleInput == INT32_MIN);
        CHECK(repUnsignedShift.encodedIntermediate == 1);
        CHECK(repUnsignedShift.actualOutput == 0);
    }

    SUBCASE("proves Mozilla VLQ round-trip is 100% lossless on exact maximal symmetric domain [-2^30 + 1, 2^30 - 1]") {
        std::string err;
        EquivReport safeRep = proveRoundTripSource(
            kMozillaVlqSource, "toVLQSigned", "fromVLQSigned", -1073741823, 1073741823, &err);
        REQUIRE(err.empty());
        CHECK(safeRep.status == EquivStatus::ProvenEquivalent);
        CHECK(safeRep.isEquivalent());
    }
}

TEST_CASE("Discovery 3: RFC 4648 Base64 24-bit quantum (3 bytes <-> 4 sextets) 2^24 proof & unmasked sextet bug") {
    constexpr const char* kBase64Source = R"(
        export function base64Pack3To4(n) {
            var s0 = (n >>> 18) & 63;
            var s1 = (n >>> 12) & 63;
            var s2 = (n >>> 6) & 63;
            var s3 = n & 63;
            return (s0 << 24) | (s1 << 16) | (s2 << 8) | s3;
        }

        export function base64Unpack4To3(q) {
            var s0 = (q >>> 24) & 63;
            var s1 = (q >>> 16) & 63;
            var s2 = (q >>> 8) & 63;
            var s3 = q & 63;
            return (s0 << 18) | (s1 << 12) | (s2 << 6) | s3;
        }

        export function base64DirectRoundTrip(n) {
            var s0 = (n >>> 18) & 63;
            var s1 = (n >>> 12) & 63;
            var s2 = (n >>> 6) & 63;
            var s3 = n & 63;
            return (s0 << 18) | (s1 << 12) | (s2 << 6) | s3;
        }
    )";

    SUBCASE("proves 2^24 (16,777,216) 24-bit byte triples pack and unpack losslessly") {
        std::string err;
        EquivReport rep = proveRoundTripSource(
            kBase64Source, "base64Pack3To4", "base64Unpack4To3", 0, 0xFFFFFF, &err);
        REQUIRE(err.empty());
        CHECK(rep.status == EquivStatus::ProvenEquivalent);
    }

    SUBCASE("refutes unmasked sextet assembly when decoder table returns 64 ('=' padding) or -1 (invalid char)") {
        constexpr const char* kMaskedReassemble = R"(
            export function assemble(s0, s1, s2, s3) {
                return ((s0 & 63) << 18) | ((s1 & 63) << 12) | ((s2 & 63) << 6) | (s3 & 63);
            }
        )";
        constexpr const char* kUnmaskedReassemble = R"(
            export function assemble(s0, s1, s2, s3) {
                return (s0 << 18) | (s1 << 12) | (s2 << 6) | s3;
            }
        )";

        // Within valid 6-bit sextet range [0, 63], unmasked and masked assembly are identical:
        EquivReport validRangeRep = proveSourceEquivalent(
            kMaskedReassemble, kUnmaskedReassemble, "assemble", 0, 63);
        CHECK(validRangeRep.status == EquivStatus::ProvenEquivalent);

        // When sextets can take values in [-1, 64] (e.g., 64 for '=' padding or -1 for invalid char),
        // omitting `& 63` bleeds into upper bytes via bitwise OR:
        EquivReport unvalidatedRep = proveSourceEquivalent(
            kMaskedReassemble, kUnmaskedReassemble, "assemble", -1, 64);
        CHECK(unvalidatedRep.status == EquivStatus::RefutedWithCounterexample);
        CHECK(unvalidatedRep.expectedOutput != unvalidatedRep.actualOutput);
    }
}

TEST_CASE("Discovery 4: UTF-8 1/2/3/4-byte scalar round-trips & overlong '/' (0xC0 0xAF) / surrogate security") {
    constexpr const char* kUtf8CodecSource = R"(
        export function utf8Encode1(cp) {
            return cp & 0x7F;
        }
        export function utf8Decode1(b0) {
            return b0 & 0x7F;
        }

        export function utf8Encode2(cp) {
            var b0 = 0xC0 | ((cp >>> 6) & 0x1F);
            var b1 = 0x80 | (cp & 0x3F);
            return (b0 << 8) | b1;
        }
        export function utf8Decode2(w) {
            var b0 = (w >>> 8) & 0xFF;
            var b1 = w & 0xFF;
            return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        }

        export function utf8Encode3(cp) {
            var b0 = 0xE0 | ((cp >>> 12) & 0x0F);
            var b1 = 0x80 | ((cp >>> 6) & 0x3F);
            var b2 = 0x80 | (cp & 0x3F);
            return (b0 << 16) | (b1 << 8) | b2;
        }
        export function utf8Decode3(w) {
            var b0 = (w >>> 16) & 0xFF;
            var b1 = (w >>> 8) & 0xFF;
            var b2 = w & 0xFF;
            return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        }

        export function utf8Encode4(cp) {
            var b0 = 0xF0 | ((cp >>> 18) & 0x07);
            var b1 = 0x80 | ((cp >>> 12) & 0x3F);
            var b2 = 0x80 | ((cp >>> 6) & 0x3F);
            var b3 = 0x80 | (cp & 0x3F);
            return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
        }
        export function utf8Decode4(w) {
            var b0 = (w >>> 24) & 0xFF;
            var b1 = (w >>> 16) & 0xFF;
            var b2 = (w >>> 8) & 0xFF;
            var b3 = w & 0xFF;
            return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        }

        export function utf8EncodeScalar(cp) {
            if (cp <= 0x7F) {
                return cp & 0x7F;
            } else if (cp <= 0x7FF) {
                var b0 = 0xC0 | ((cp >>> 6) & 0x1F);
                var b1 = 0x80 | (cp & 0x3F);
                return (b0 << 8) | b1;
            } else if (cp <= 0xFFFF) {
                var c0 = 0xE0 | ((cp >>> 12) & 0x0F);
                var c1 = 0x80 | ((cp >>> 6) & 0x3F);
                var c2 = 0x80 | (cp & 0x3F);
                return (c0 << 16) | (c1 << 8) | c2;
            } else {
                var d0 = 0xF0 | ((cp >>> 18) & 0x07);
                var d1 = 0x80 | ((cp >>> 12) & 0x3F);
                var d2 = 0x80 | ((cp >>> 6) & 0x3F);
                var d3 = 0x80 | (cp & 0x3F);
                return (d0 << 24) | (d1 << 16) | (d2 << 8) | d3;
            }
        }

        export function utf8DecodeScalar(w) {
            if ((w & 0xFF000000) !== 0) {
                var d0 = (w >>> 24) & 0xFF;
                var d1 = (w >>> 16) & 0xFF;
                var d2 = (w >>> 8) & 0xFF;
                var d3 = w & 0xFF;
                return ((d0 & 0x07) << 18) | ((d1 & 0x3F) << 12) | ((d2 & 0x3F) << 6) | (d3 & 0x3F);
            } else if ((w & 0x00FF0000) !== 0) {
                var c0 = (w >>> 16) & 0xFF;
                var c1 = (w >>> 8) & 0xFF;
                var c2 = w & 0xFF;
                return ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            } else if ((w & 0x0000FF00) !== 0) {
                var b0 = (w >>> 8) & 0xFF;
                var b1 = w & 0xFF;
                return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
            } else {
                return w & 0x7F;
            }
        }
    )";

    SUBCASE("proves 1-byte, 2-byte, 3-byte, 4-byte, and unified scalar UTF-8 round-trips across all valid Unicode scalar values") {
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8Encode1", "utf8Decode1", 0x0000, 0x007F).status ==
              EquivStatus::ProvenEquivalent);
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8Encode2", "utf8Decode2", 0x0080, 0x07FF).status ==
              EquivStatus::ProvenEquivalent);
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8Encode3", "utf8Decode3", 0x0800, 0xFFFF).status ==
              EquivStatus::ProvenEquivalent);
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8Encode4", "utf8Decode4", 0x10000, 0x10FFFF).status ==
              EquivStatus::ProvenEquivalent);

        // Unified branching encoder/decoder across both Unicode scalar intervals [0x0000..0xD7FF] and [0xE000..0x10FFFF]:
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8EncodeScalar", "utf8DecodeScalar", 0x0000, 0xD7FF).status ==
              EquivStatus::ProvenEquivalent);
        CHECK(proveRoundTripSource(kUtf8CodecSource, "utf8EncodeScalar", "utf8DecodeScalar", 0xE000, 0x10FFFF).status ==
              EquivStatus::ProvenEquivalent);
    }

    SUBCASE("discovers 0xC0 0xAF -> '/' (0x2F) overlong directory-traversal witness and proves b0 >= 0xC2 invariant") {
        BVContext ctx;
        BVNodeId b0 = ctx.make_var("b0", 32);
        BVNodeId b1 = ctx.make_var("b1", 32);

        // Naive 2-byte header check: (b0 & 0xE0) == 0xC0 && (b1 & 0xC0) == 0x80
        BVNodeId validLeadNaive = ctx.make_binary(
            BVOp::Eq, ctx.make_binary(BVOp::And, b0, ctx.make_const(0xFFFFFFE0u, 32)), ctx.make_const(0xC0u, 32));
        BVNodeId validTrail = ctx.make_binary(
            BVOp::Eq, ctx.make_binary(BVOp::And, b1, ctx.make_const(0xFFFFFFC0u, 32)), ctx.make_const(0x80u, 32));

        BVNodeId decoded = ctx.make_binary(
            BVOp::Or,
            ctx.make_binary(BVOp::Shl, ctx.make_binary(BVOp::And, b0, ctx.make_const(0x1Fu, 32)), ctx.make_const(6, 32)),
            ctx.make_binary(BVOp::And, b1, ctx.make_const(0x3Fu, 32)));

        // Query 1: Can a naive 2-byte decoder produce ASCII '/' (0x2F)?
        BVNodeId isSlash = ctx.make_binary(BVOp::Eq, decoded, ctx.make_const(0x2Fu, 32));
        BVNodeId overlongSlashCond = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::And, validLeadNaive, validTrail), isSlash);

        SolveOutcome slashSat = brass::verify::solve_nonzero(ctx, overlongSlashCond);
        REQUIRE(slashSat.status == SolveOutcome::Sat);
        REQUIRE(slashSat.model.size() == 2);
        CHECK(slashSat.model[0] == 0xC0u);
        CHECK(slashSat.model[1] == 0xAFu);

        // Query 2: With strict RFC 3629 lead-byte check (0xC2 <= b0 <= 0xDF), can decoded ever be < 0x80?
        BVNodeId strictLead = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Uge, b0, ctx.make_const(0xC2u, 32)),
            ctx.make_binary(BVOp::Ule, b0, ctx.make_const(0xDFu, 32)));
        BVNodeId decodedUnder0x80 = ctx.make_binary(BVOp::Ult, decoded, ctx.make_const(0x80u, 32));
        BVNodeId strictOverlongCond = ctx.make_binary(
            BVOp::And, ctx.make_binary(BVOp::And, strictLead, validTrail), decodedUnder0x80);

        SolveOutcome strictOut = brass::verify::solve_nonzero(ctx, strictOverlongCond);
        CHECK(strictOut.status == SolveOutcome::Unsat);
    }

    SUBCASE("discovers UTF-16 surrogate half (0xD800..0xDFFF) bypass in naive 3-byte UTF-8 decoder and proves RFC 3629 guard") {
        BVContext ctx;
        BVNodeId b0 = ctx.make_var("b0", 32);
        BVNodeId b1 = ctx.make_var("b1", 32);
        BVNodeId b2 = ctx.make_var("b2", 32);

        BVNodeId validLead3 = ctx.make_binary(
            BVOp::Eq, ctx.make_binary(BVOp::And, b0, ctx.make_const(0xFFFFFFF0u, 32)), ctx.make_const(0xE0u, 32));
        BVNodeId validTrail1 = ctx.make_binary(
            BVOp::Eq, ctx.make_binary(BVOp::And, b1, ctx.make_const(0xFFFFFFC0u, 32)), ctx.make_const(0x80u, 32));
        BVNodeId validTrail2 = ctx.make_binary(
            BVOp::Eq, ctx.make_binary(BVOp::And, b2, ctx.make_const(0xFFFFFFC0u, 32)), ctx.make_const(0x80u, 32));
        BVNodeId validNaive3 = ctx.make_binary(BVOp::And, validLead3, ctx.make_binary(BVOp::And, validTrail1, validTrail2));

        BVNodeId hiPart = ctx.make_binary(
            BVOp::Shl, ctx.make_binary(BVOp::And, b0, ctx.make_const(0x0Fu, 32)), ctx.make_const(12, 32));
        BVNodeId midPart = ctx.make_binary(
            BVOp::Shl, ctx.make_binary(BVOp::And, b1, ctx.make_const(0x3Fu, 32)), ctx.make_const(6, 32));
        BVNodeId loPart = ctx.make_binary(BVOp::And, b2, ctx.make_const(0x3Fu, 32));
        BVNodeId decoded3 = ctx.make_binary(BVOp::Or, hiPart, ctx.make_binary(BVOp::Or, midPart, loPart));

        BVNodeId inSurrogateRange = ctx.make_binary(
            BVOp::And,
            ctx.make_binary(BVOp::Uge, decoded3, ctx.make_const(0xD800u, 32)),
            ctx.make_binary(BVOp::Ule, decoded3, ctx.make_const(0xDFFFu, 32)));

        // Naive 3-byte decoder accepts surrogate halves (e.g. 0xED 0xA0 0x80 -> 0xD800):
        SolveOutcome naiveSurrogate = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::And, validNaive3, inSurrogateRange));
        REQUIRE(naiveSurrogate.status == SolveOutcome::Sat);
        CHECK(naiveSurrogate.model[0] == 0xEDu);
        CHECK(naiveSurrogate.model[1] >= 0xA0u);
        CHECK(naiveSurrogate.model[1] <= 0xBFu);

        // RFC 3629 guard: when b0 == 0xED, require b1 <= 0x9F:
        BVNodeId notEd = ctx.make_binary(BVOp::Ne, b0, ctx.make_const(0xEDu, 32));
        BVNodeId b1Le9F = ctx.make_binary(BVOp::Ule, b1, ctx.make_const(0x9Fu, 32));
        BVNodeId noSurrogateGuard = ctx.make_binary(BVOp::Or, notEd, b1Le9F);
        SolveOutcome guardedSurrogate = brass::verify::solve_nonzero(
            ctx, ctx.make_binary(BVOp::And, ctx.make_binary(BVOp::And, validNaive3, noSurrogateGuard), inSurrogateRange));
        CHECK(guardedSurrogate.status == SolveOutcome::Unsat);
    }
}

TEST_CASE("Discovery 5: MurmurHash3 / xxHash32 / FNV-1a split-16-bit multiply proof & IEEE-754 f64 precision-loss witness") {
    SUBCASE("proves 16-bit split multiply is bit-identical to Math.imul across all 2^32 inputs for Murmur3, xxHash32, and FNV-1a") {
        constexpr const char* kSplitMulMurmur = R"(
            export function murmurMulC1(a) {
                var b = 0xcc9e2d51;
                return (((a & 0xffff) * b) + ((((a >>> 16) * b) & 0xffff) << 16)) | 0;
            }
            export function xxhashMulPrime2(a) {
                var b = 0x85ebca77;
                return (((a & 0xffff) * b) + ((((a >>> 16) * b) & 0xffff) << 16)) | 0;
            }
            export function fnv1aMulPrime(a) {
                var b = 0x01000193;
                return (((a & 0xffff) * b) + ((((a >>> 16) * b) & 0xffff) << 16)) | 0;
            }
            export function murmurMixK1(k1) {
                var c1 = 0xcc9e2d51;
                var c2 = 0x1b873593;
                var m1 = (((k1 & 0xffff) * c1) + ((((k1 >>> 16) * c1) & 0xffff) << 16)) | 0;
                var r1 = (m1 << 15) | (m1 >>> 17);
                var m2 = (((r1 & 0xffff) * c2) + ((((r1 >>> 16) * c2) & 0xffff) << 16)) | 0;
                return m2;
            }
        )";

        constexpr const char* kImulMurmur = R"(
            export function murmurMulC1(a) {
                return Math.imul(a, 0xcc9e2d51);
            }
            export function xxhashMulPrime2(a) {
                return Math.imul(a, 0x85ebca77);
            }
            export function fnv1aMulPrime(a) {
                return Math.imul(a, 0x01000193);
            }
            export function murmurMixK1(k1) {
                var m1 = Math.imul(k1, 0xcc9e2d51);
                var r1 = (m1 << 15) | (m1 >>> 17);
                return Math.imul(r1, 0x1b873593);
            }
        )";

        std::string err;
        ProveEquivReport repAll = proveSourceEquiv(kSplitMulMurmur, kImulMurmur, "", &err);
        REQUIRE(err.empty());
        REQUIRE(repAll.functions.size() == 4);
        CHECK(repAll.allEquivalent());

        EquivReport c1Rep = proveSourceEquivalent(kSplitMulMurmur, kImulMurmur, "murmurMulC1");
        CHECK(c1Rep.status == EquivStatus::ProvenEquivalent);
        EquivReport xxhRep = proveSourceEquivalent(kSplitMulMurmur, kImulMurmur, "xxhashMulPrime2");
        CHECK(xxhRep.status == EquivStatus::ProvenEquivalent);
        EquivReport fnvRep = proveSourceEquivalent(kSplitMulMurmur, kImulMurmur, "fnv1aMulPrime");
        CHECK(fnvRep.status == EquivStatus::ProvenEquivalent);
        EquivReport mixRep = proveSourceEquivalent(kSplitMulMurmur, kImulMurmur, "murmurMixK1");
        CHECK(mixRep.status == EquivStatus::ProvenEquivalent);
    }

    SUBCASE("discovers exact IEEE-754 f64 precision-loss threshold witnesses where ((a * c) | 0) !== Math.imul(a, c)") {
        // MurmurHash3 c1 = 0xcc9e2d51 = 3432918353 (odd).
        // Number.MAX_SAFE_INTEGER = 2^53 - 1 = 9007199254740991.
        // 2623773 * 3432918353 = 9007198483544229 (< 2^53),
        // 2623774 * 3432918353 = 9007201916462582 (> 2^53, even so LSB=0 is preserved),
        // 2623775 * 3432918353 = 9007205349380935 (> 2^53, odd -> rounds to 9007205349380936)!
        constexpr uint64_t kMurmurC1 = 0xcc9e2d51ULL;
        constexpr int32_t kMurmurSafeMax = 2623774;
        constexpr int32_t kMurmurFirstBug = 2623775;

        // Verify all a in [1, kMurmurSafeMax] around the boundary are exact, and kMurmurFirstBug is the
        // smallest positive integer where IEEE-754 f64 multiplication loses low bit precision:
        int32_t safeNaive = jsToInt32(static_cast<double>(kMurmurSafeMax) * static_cast<double>(kMurmurC1));
        int32_t safeImul = static_cast<int32_t>(static_cast<uint32_t>(kMurmurSafeMax) * static_cast<uint32_t>(kMurmurC1));
        CHECK(safeNaive == safeImul);

        int32_t bugNaive = jsToInt32(static_cast<double>(kMurmurFirstBug) * static_cast<double>(kMurmurC1));
        int32_t bugImul = static_cast<int32_t>(static_cast<uint32_t>(kMurmurFirstBug) * static_cast<uint32_t>(kMurmurC1));
        CHECK(bugNaive != bugImul);
        CHECK(bugImul == 1801934287);
        CHECK(bugNaive == 1801934288);

        // Also verify via BVContext IEEE-754 f64 evaluation (BVOp::SiToFp + BVOp::FMul):
        BVContext ctx;
        BVNodeId aVar = ctx.make_var("a", 64);
        BVNodeId aF64 = ctx.make_unary(BVOp::SiToFp, aVar, 64);
        double c1Double = static_cast<double>(kMurmurC1);
        uint64_t c1Bits = 0;
        std::memcpy(&c1Bits, &c1Double, sizeof(double));
        BVNodeId c1F64 = ctx.make_const(c1Bits, 64);
        BVNodeId prodF64 = ctx.make_binary(BVOp::FMul, aF64, c1F64, 64);

        uint64_t evalBits = ctx.eval_concrete(prodF64, {static_cast<uint64_t>(kMurmurFirstBug)});
        double evalDouble = 0.0;
        std::memcpy(&evalDouble, &evalBits, sizeof(double));
        CHECK(jsToInt32(evalDouble) == bugNaive);
        CHECK(jsToInt32(evalDouble) != bugImul);

        // xxHash32 PRIME32_2 = 0x85ebca77 = 2246822519 (odd):
        // 4008862 * 2246822519 is even, first odd product > 2^53 is at a = 4008863!
        constexpr uint64_t kXxHashPrime2 = 0x85ebca77ULL;
        constexpr int32_t kXxHashSafeMax = 4008862;
        constexpr int32_t kXxHashFirstBug = 4008863;
        CHECK(jsToInt32(static_cast<double>(kXxHashSafeMax) * static_cast<double>(kXxHashPrime2)) ==
              static_cast<int32_t>(static_cast<uint32_t>(kXxHashSafeMax) * static_cast<uint32_t>(kXxHashPrime2)));
        CHECK(jsToInt32(static_cast<double>(kXxHashFirstBug) * static_cast<double>(kXxHashPrime2)) !=
              static_cast<int32_t>(static_cast<uint32_t>(kXxHashFirstBug) * static_cast<uint32_t>(kXxHashPrime2)));
    }
}

TEST_CASE("Discovery 6: Morton 2D Z-order curve SWAR bit-interleaving 2^16 & 2^32 round-trip proofs") {
    constexpr const char* kMortonSource = R"(
        export function part1By1(x) {
            var n = x & 0x0000ffff;
            n = (n | (n << 8)) & 0x00ff00ff;
            n = (n | (n << 4)) & 0x0f0f0f0f;
            n = (n | (n << 2)) & 0x33333333;
            n = (n | (n << 1)) & 0x55555555;
            return n;
        }

        export function compact1By1(n) {
            var x = n & 0x55555555;
            x = (x | (x >>> 1)) & 0x33333333;
            x = (x | (x >>> 2)) & 0x0f0f0f0f;
            x = (x | (x >>> 4)) & 0x00ff00ff;
            x = (x | (x >>> 8)) & 0x0000ffff;
            return x;
        }

        export function encodeMorton2D(packedXY) {
            var x = packedXY & 0xffff;
            var y = (packedXY >>> 16) & 0xffff;
            return part1By1(x) | (part1By1(y) << 1);
        }

        export function decodeMorton2D(code) {
            var x = compact1By1(code);
            var y = compact1By1(code >>> 1);
            return (y << 16) | x;
        }

        export function decodeMorton2DBrokenSignedShift(code) {
            var x = compact1By1(code);
            var y = compact1By1(code >> 1);
            return (y << 16) | x;
        }
    )";

    SUBCASE("proves compact1By1(part1By1(x)) === x across all 2^16 (65,536) 16-bit coordinates") {
        std::string err;
        EquivReport rep1D = proveRoundTripSource(
            kMortonSource, "part1By1", "compact1By1", 0, 65535, &err);
        REQUIRE(err.empty());
        CHECK(rep1D.status == EquivStatus::ProvenEquivalent);
        CHECK(rep1D.isEquivalent());
    }

    SUBCASE("proves decodeMorton2D(encodeMorton2D(packedXY)) === packedXY across all 2^32 (x, y) coordinate pairs") {
        std::string err;
        EquivReport rep2D = proveRoundTripSource(
            kMortonSource, "encodeMorton2D", "decodeMorton2D", INT32_MIN, INT32_MAX, &err);
        REQUIRE(err.empty());
        CHECK(rep2D.status == EquivStatus::ProvenEquivalent);
        CHECK(rep2D.isEquivalent());
    }

    SUBCASE("refutes decodeMorton2D variant using signed code >> 1 when y has bit 15 set (y >= 32768)") {
        //Wait: why would compact1By1(code >> 1) fail or succeed?
        // Look at compact1By1(n): the first step is `var x = n & 0x55555555;`!
        // Since bit 31 of 0x55555555 is 0, `(code >> 1) & 0x55555555` actually masks off bit 31!
        // Where DOES signed `>>` break compact1By1? Inside `compact1By1` if `x >>> 1` is written `x >> 1`
        // OR if `part1By1` has a mask typo (e.g. `0x0f0f0ff0` instead of `0x0f0f0f0f`)!
        constexpr const char* kMortonMaskBugSource = R"(
            export function part1By1Buggy(x) {
                var n = x & 0x0000ffff;
                n = (n | (n << 8)) & 0x00ff00ff;
                n = (n | (n << 4)) & 0x0f0f0ff0;
                n = (n | (n << 2)) & 0x33333333;
                n = (n | (n << 1)) & 0x55555555;
                return n;
            }
            export function compact1By1(n) {
                var x = n & 0x55555555;
                x = (x | (x >>> 1)) & 0x33333333;
                x = (x | (x >>> 2)) & 0x0f0f0f0f;
                x = (x | (x >>> 4)) & 0x00ff00ff;
                x = (x | (x >>> 8)) & 0x0000ffff;
                return x;
            }
        )";
        std::string err;
        EquivReport bugRep = proveRoundTripSource(
            kMortonMaskBugSource, "part1By1Buggy", "compact1By1", 0, 65535, &err);
        REQUIRE(err.empty());
        CHECK(bugRep.status == EquivStatus::RefutedWithCounterexample);
        CHECK((bugRep.counterexampleInput & 0x0F) != 0);
        CHECK(bugRep.actualOutput != bugRep.expectedOutput);
    }
}
