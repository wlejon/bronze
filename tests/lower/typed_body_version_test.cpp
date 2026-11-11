// Loop-free typed-view body versioning (`src/lower/typed_body_version.h`):
// multi-view Float32Array / Float64Array read+write specialization with entry
// bounds probes (`ElemGetTyped` + `ElemSetTyped` + unboxed `Cmp*`), alongside
// vertex-batcher (`Float32 + Uint32`) and generic word-view preservation.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "abi/bronze_abi.h"
#include "il/il_pin.h"
#include "il/verifier.h"
#include "lower_fixture.h"

using namespace bronze;

namespace {

std::optional<il::Module> lowerAot(std::string_view src, DiagnosticSink& diags, SourceBuffer& buf) {
    auto astMod = lower_test::parseOnly(src, diags, buf);
    if (diags.hasErrors() || !astMod) return std::nullopt;
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) return std::nullopt;
    lower::AheadOfTimeOptions opts;
    return lower::lowerAheadOfTime(*astMod, diags, &*inferred, nullptr, opts);
}

const il::Function* findFnEndingIn(const il::Module& mod, std::string_view suffix) {
    for (const auto& fn : mod.functions) {
        if (fn.typedOrigin != il::Function::kNoTypedOrigin) continue;
        if (fn.name.size() >= suffix.size() &&
            fn.name.compare(fn.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return &fn;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("typed body version: slerpFlat 3-view kernel emits concrete Float32/Float64 copies with unboxed reads and compares") {
    // Include a BigInt helper in the module so arithmetic lowers in Dynamic
    // form (matching Three.js modules) and exercises full F64 promotion.
    const std::string src =
        "function keepBigInt() { return 1n; }\n"
        "function slerpFlat(dst: any, dstOffset: any, src0: any, srcOffset0: any, src1: any, srcOffset1: any, t: any) {\n"
        "  let x0 = src0[srcOffset0 + 0], y0 = src0[srcOffset0 + 1], z0 = src0[srcOffset0 + 2], w0 = src0[srcOffset0 + 3];\n"
        "  const x1 = src1[srcOffset1 + 0], y1 = src1[srcOffset1 + 1], z1 = src1[srcOffset1 + 2], w1 = src1[srcOffset1 + 3];\n"
        "  if (t === 0) {\n"
        "    dst[dstOffset + 0] = x0; dst[dstOffset + 1] = y0; dst[dstOffset + 2] = z0; dst[dstOffset + 3] = w0;\n"
        "    return;\n"
        "  }\n"
        "  if (w0 !== w1 || x0 !== x1 || y0 !== y1 || z0 !== z1) {\n"
        "    let s = 1 - t;\n"
        "    const cos = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1;\n"
        "    const dir = cos >= 0 ? 1 : -1;\n"
        "    const sqrSin = 1 - cos * cos;\n"
        "    if (sqrSin > 1e-12) {\n"
        "      const sin = Math.sqrt(sqrSin);\n"
        "      const len = Math.atan2(sin, cos * dir);\n"
        "      s = Math.sin(s * len) / sin;\n"
        "      t = Math.sin(t * len) / sin;\n"
        "    }\n"
        "    const tDir = t * dir;\n"
        "    x0 = x0 * s + x1 * tDir;\n"
        "    y0 = y0 * s + y1 * tDir;\n"
        "    z0 = z0 * s + z1 * tDir;\n"
        "    w0 = w0 * s + w1 * tDir;\n"
        "  }\n"
        "  dst[dstOffset] = x0;\n"
        "  dst[dstOffset + 1] = y0;\n"
        "  dst[dstOffset + 2] = z0;\n"
        "  dst[dstOffset + 3] = w0;\n"
        "}\n"
        "keepBigInt();\n";

    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lowerAot(src, diags, buf);
    REQUIRE(mod.has_value());
    REQUIRE_FALSE(diags.hasErrors());

    const il::Function* fn = findFnEndingIn(*mod, "slerpFlat");
    REQUIRE(fn != nullptr);

    size_t taF32Checks = 0;
    size_t taF64Checks = 0;
    size_t taWordChecks = 0;
    size_t hotTypedGets = 0;
    size_t hotInBoundsGets = 0;
    size_t hotTypedSets = 0;
    size_t hotGenericElemOps = 0;
    size_t hotStrictEqs = 0;
    size_t hotRelOps = 0;
    size_t hotF64Boxes = 0;

    for (const auto& blk : fn->blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::IsTypedArray) {
                if (inst.immI32 == BRONZE_ABI_TA_KIND_FLOAT32) ++taF32Checks;
                else if (inst.immI32 == BRONZE_ABI_TA_KIND_FLOAT64) ++taF64Checks;
                else if (inst.immI32 == il::kTypedArrayWordView) ++taWordChecks;
            }
            if (blk.cold || blk.copyClass == il::CopyClass::Slow) continue;
            if (inst.op == il::Op::ElemGetTyped) {
                ++hotTypedGets;
                if ((inst.immI32 & il::kElemFlagProvenInBounds) != 0) ++hotInBoundsGets;
            } else if (inst.op == il::Op::ElemSetTyped) {
                ++hotTypedSets;
            } else if (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet) {
                ++hotGenericElemOps;
            } else if (inst.op == il::Op::StrictEq) {
                ++hotStrictEqs;
            } else if (inst.op == il::Op::RelGe || inst.op == il::Op::RelGt ||
                       inst.op == il::Op::RelLt || inst.op == il::Op::RelLe) {
                ++hotRelOps;
            } else if (inst.op == il::Op::Box && inst.boxType == il::Type::F64) {
                ++hotF64Boxes;
            }
        }
    }

    // 3 concrete versions: [F32,F32,F32], [F64,F32,F32], [F64,F64,F64].
    CHECK(taF32Checks == 5);
    CHECK(taF64Checks == 4);
    CHECK(taWordChecks == 0);
    // 8 body reads * 3 versions = 24 in-bounds loads (+ 6 entry bounds probes).
    CHECK(hotInBoundsGets == 24);
    CHECK(hotTypedGets == 30);
    // 8 body stores * 3 versions = 24 typed stores.
    CHECK(hotTypedSets == 24);
    CHECK(hotGenericElemOps == 0);
    CHECK(hotStrictEqs == 0);
    CHECK(hotRelOps == 0);
    CHECK(hotF64Boxes == 0);

    DiagnosticSink verifyDiags;
    CHECK(il::verify(*mod, verifyDiags));
}

TEST_CASE("typed body version: preserves float+uint32 packer and pure-read word-view helper") {
    const std::string src =
        "function pack(el: any, f32: any, u32: any, index: any, tex: any) {\n"
        "  const packed = (tex << 16) | (el.round & 65535);\n"
        "  f32[index + 0] = el.a * el.x + el.tx;\n"
        "  f32[index + 1] = el.d * el.y + el.ty;\n"
        "  u32[index + 2] = el.color;\n"
        "  u32[index + 3] = packed;\n"
        "}\n"
        "function fmt(x: any) { return typeof x === 'number' ? String(x) : '?'; }\n"
        "function readBack(a: any, b: any, index: any) {\n"
        "  const x = a[index + 0], y = a[index + 1], z = b[index + 2], w = b[index + 3];\n"
        "  return fmt(x) + ',' + fmt(y) + ',' + fmt(z) + ',' + fmt(w);\n"
        "}\n";

    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lowerAot(src, diags, buf);
    REQUIRE(mod.has_value());
    REQUIRE_FALSE(diags.hasErrors());

    const il::Function* packFn = findFnEndingIn(*mod, "pack");
    const il::Function* readFn = findFnEndingIn(*mod, "readBack");
    REQUIRE(packFn != nullptr);
    REQUIRE(readFn != nullptr);

    bool packHasF32 = false;
    bool packHasU32 = false;
    for (const auto& blk : packFn->blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::IsTypedArray) {
                if (inst.immI32 == BRONZE_ABI_TA_KIND_FLOAT32) packHasF32 = true;
                if (inst.immI32 == BRONZE_ABI_TA_KIND_UINT32) packHasU32 = true;
            }
        }
    }
    CHECK(packHasF32);
    CHECK(packHasU32);

    size_t readWordChecks = 0;
    for (const auto& blk : readFn->blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::IsTypedArray && inst.immI32 == il::kTypedArrayWordView) {
                ++readWordChecks;
            }
        }
    }
    CHECK(readWordChecks == 2);

    DiagnosticSink verifyDiags;
    CHECK(il::verify(*mod, verifyDiags));
}
