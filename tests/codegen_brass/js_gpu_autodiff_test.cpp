#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "codegen-brass/js_gpu_autodiff.h"

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

}  // namespace

TEST_CASE("js_gpu_autodiff: Test 1 - Polynomial & Trigonometric 3D Gradient Accuracy across 100 coordinates") {
    const std::string src = R"(
// @shader
function polyTrig3D(x, y, z) {
    return x * x * y + Math.sin(z) * x - Math.exp(-y * z);
}
)";
    auto res = bronze::compileJsGpuAutodiffSource(src, "poly_trig_3d.js");
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.kernelName == "polyTrig3D");
    CHECK(res.scalarParamCount == 3);
    CHECK_FALSE(res.isHeightfield2D);
    CHECK(res.provenSafe);
    CHECK(res.cpuGpuProvenEquivalent);

    int tested = 0;
    for (int ix = 0; ix < 5; ++ix) {
        for (int iy = 0; iy < 5; ++iy) {
            for (int iz = 0; iz < 4; ++iz) {
                const float x = -2.0f + 0.95f * static_cast<float>(ix);
                const float y = -1.2f + 0.65f * static_cast<float>(iy);
                const float z = -1.5f + 0.85f * static_cast<float>(iz);

                const float expectedVal = x * x * y + std::sin(z) * x - std::exp(-y * z);
                const float expectedDx = 2.0f * x * y + std::sin(z);
                const float expectedDy = x * x + z * std::exp(-y * z);
                const float expectedDz = x * std::cos(z) + y * std::exp(-y * z);

                const auto s = res.evalGradCpu(x, y, z);
                CHECK(std::fabs(s.value - expectedVal) < 5e-5f);
                CHECK(std::fabs(s.dx - expectedDx) < 5e-5f);
                CHECK(std::fabs(s.dy - expectedDy) < 5e-5f);
                CHECK(std::fabs(s.dz - expectedDz) < 5e-5f);

                const float normLen = std::sqrt(s.nx * s.nx + s.ny * s.ny + s.nz * s.nz);
                CHECK(std::fabs(normLen - 1.0f) < 1e-5f);
                ++tested;
            }
        }
    }
    CHECK(tested == 100);
}

TEST_CASE("js_gpu_autodiff: Test 2 - Procedural Terrain Heightfield Normal Compilation with smoothstep and clamp ridges") {
    const std::string src = R"(
// @shader
function terrainHeight(x, z) {
    let sx = Math.sin(x * 0.25) * 8.0 + Math.cos(x * 0.5) * 2.0;
    let cz = Math.cos(z * 0.25) * 8.0 + Math.sin(z * 0.5) * 2.0;
    let ridge = clamp(Math.abs(sx - cz) * 0.35, 0.0, 5.0);
    let blend = smoothstep(-8.0, 8.0, sx + cz);
    return mix(sx + cz, ridge, blend);
}
)";
    auto res = bronze::compileJsGpuAutodiffSource(src, "terrain_autodiff.js");
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.kernelName == "terrainHeight");
    CHECK(res.scalarParamCount == 2);
    CHECK(res.isHeightfield2D);
    CHECK(res.provenSafe);
    CHECK(res.cpuGpuProvenEquivalent);

    // Include ridge cusps where sx == cz (e.g. x=0, z=0 and symmetric diagonal points)
    const std::vector<std::pair<float, float>> coords = {
        {0.0f, 0.0f},
        {3.14159265f, 3.14159265f},
        {-6.2831853f, 0.0f},
        {-15.0f, -12.0f},
        {-4.25f, 7.75f},
        {2.5f, -3.5f},
        {11.125f, 14.875f},
        {24.0f, -19.5f},
    };
    for (const auto& [x, z] : coords) {
        const auto s = res.evalGradCpu(x, z);
        CHECK_FALSE(std::isnan(s.value));
        CHECK_FALSE(std::isnan(s.dx));
        CHECK_FALSE(std::isnan(s.dz));
        CHECK_FALSE(std::isnan(s.nx));
        CHECK_FALSE(std::isnan(s.ny));
        CHECK_FALSE(std::isnan(s.nz));

        const float nLen = std::sqrt(s.nx * s.nx + s.ny * s.ny + s.nz * s.nz);
        CHECK(std::fabs(nLen - 1.0f) < 1e-6f);
        CHECK(s.ny > 0.0f);
    }
}

TEST_CASE("js_gpu_autodiff: Test 3 - Shared Subexpression GVN Compression on Math.sin(x*x + y*y + z*z)") {
    const std::string src = R"(
// @shader
function radialWave(x, y, z) {
    return Math.sin(x * x + y * y + z * z);
}
)";
    auto res = bronze::compileJsGpuAutodiffSource(src, "radial_wave.js");
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.sinCallCountBeforeGvn == 1);
    CHECK(res.cosCallCountBeforeGvn == 3);
    CHECK(res.sinCallCountAfterGvn == 1);
    CHECK(res.cosCallCountAfterGvn == 1);
    CHECK(res.gvnExpressionsEliminated >= 2);

    const auto s = res.evalGradCpu(1.2f, -0.8f, 0.5f);
    const float r2 = 1.2f * 1.2f + (-0.8f) * (-0.8f) + 0.5f * 0.5f;
    CHECK(std::fabs(s.value - std::sin(r2)) < 1e-5f);
    CHECK(std::fabs(s.dx - 2.0f * 1.2f * std::cos(r2)) < 1e-5f);
    CHECK(std::fabs(s.dy - 2.0f * (-0.8f) * std::cos(r2)) < 1e-5f);
    CHECK(std::fabs(s.dz - 2.0f * 0.5f * std::cos(r2)) < 1e-5f);
}

TEST_CASE("js_gpu_autodiff: Test 4 - CPU vs SPIR-V Equivalence & Safety Verification") {
    const std::string src = R"(
// @kernel
function sdfTorusLike(x, y, z) {
    let qx = Math.sqrt(x * x + z * z) - 2.0;
    return Math.sqrt(qx * qx + y * y) - 0.5;
}
)";
    auto res = bronze::compileJsGpuAutodiffSource(src, "sdf_torus.js");
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    REQUIRE(res.spirvWords.size() > 5);
    CHECK(res.spirvWords[0] == kSpirvMagic);
    CHECK(res.spirvDiagnostics.empty());
    CHECK(res.spirvEquivalence.is_equivalent());
    CHECK(res.cpuGpuProvenEquivalent);
    CHECK(res.shaderSafetyReport.ok());
    CHECK(res.provenSafe);
    CHECK(res.singleSourceParityVerified);

    const auto cpuS = res.evalGradCpu(3.0f, 0.4f, 4.0f);
    const auto spvS = res.evalGradSpirv(3.0f, 0.4f, 4.0f);
    CHECK(std::fabs(cpuS.value - spvS.value) < 1e-5f);
    CHECK(std::fabs(cpuS.dx - spvS.dx) < 1e-5f);
    CHECK(std::fabs(cpuS.dy - spvS.dy) < 1e-5f);
    CHECK(std::fabs(cpuS.dz - spvS.dz) < 1e-5f);
}
