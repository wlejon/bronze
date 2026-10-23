#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "codegen-brass/js_gpu_kernel.h"

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

}  // namespace

TEST_CASE("js_gpu_kernel: 1D SAXPY vector kernel compiles to verified SPIR-V 1.5 and PTX sm_80") {
    const std::string src = R"(
// @kernel
function saxpy(out, x, y, a, n) {
    let i = global_tid_x();
    if (i < n) {
        out[i] = a * x[i] + y[i];
    }
}
)";
    bronze::GpuCompileOptions opts;
    opts.ptxSmVersion = 80;
    auto res = bronze::compileJsGpuKernelSource(src, "saxpy.js", opts);
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.kernelName == "saxpy");
    CHECK(res.spirvDiagnostics.empty());
    CHECK(res.ptxDiagnostics.empty());
    REQUIRE(res.spirvWords.size() > 5);
    CHECK(res.spirvWords[0] == kSpirvMagic);
    CHECK(res.spirvWords[1] == 0x00010500u);
    CHECK(res.ptxAssembly.find(".target sm_80") != std::string::npos);
    CHECK(res.ptxAssembly.find(".visible .entry saxpy") != std::string::npos);
    CHECK(res.equivalence.is_equivalent());
    CHECK(res.memorySafety.is_safe());
    CHECK(res.barrierUniformity.is_uniform());
}

TEST_CASE("js_gpu_kernel: 2D Mandelbrot kernel compiles to verified SPIR-V 1.5 and PTX sm_80") {
    const std::string src = R"(
// @kernel
function mandelbrot(out, width, height, maxIter) {
    let tid = global_tid_x();
    let px = tid % width;
    let py = (tid / width) | 0;
    if (px < width && py < height) {
        let cx = (px - width * 0.5) * 0.005;
        let cy = (py - height * 0.5) * 0.005;
        let zx = 0.0;
        let zy = 0.0;
        let iter = 0;
        while (iter < maxIter && (zx * zx + zy * zy) < 4.0) {
            let tmp = zx * zx - zy * zy + cx;
            zy = 2.0 * zx * zy + cy;
            zx = tmp;
            iter = iter + 1;
        }
        out[py * width + px] = iter;
    }
}
)";
    bronze::GpuCompileOptions opts;
    opts.ptxSmVersion = 80;
    auto res = bronze::compileJsGpuKernelSource(src, "mandelbrot.js", opts);
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.kernelName == "mandelbrot");
    CHECK(res.spirvDiagnostics.empty());
    CHECK(res.ptxDiagnostics.empty());
    REQUIRE(res.spirvWords.size() > 5);
    CHECK(res.spirvWords[0] == kSpirvMagic);
    CHECK(res.ptxAssembly.find(".visible .entry mandelbrot") != std::string::npos);
    CHECK(res.equivalence.is_equivalent());
    CHECK(res.memorySafety.is_safe());
    CHECK(res.barrierUniformity.is_uniform());
}

TEST_CASE("js_gpu_kernel: 2D particle-position update kernel compiles to SPIR-V 1.5 and PTX sm_80") {
    const std::string src = R"(
// @kernel
function updateParticles(px, py, vx, vy, dt, n) {
    for (let i = 0; i < n; i = i + 1) {
        let speedX = vx[i];
        let speedY = vy[i];
        let damp = Math.sqrt(speedX * speedX + speedY * speedY + 1.0);
        px[i] = px[i] + (speedX / damp) * dt;
        py[i] = py[i] + (speedY / damp) * dt;
    }
}
)";
    bronze::GpuCompileOptions opts;
    opts.ptxSmVersion = 80;
    auto res = bronze::compileJsGpuKernelSource(src, "particles.js", opts);
    INFO(res.errorMessage);
    REQUIRE(res.ok);
    CHECK(res.kernelName == "updateParticles");
    CHECK(res.spirvDiagnostics.empty());
    CHECK(res.ptxDiagnostics.empty());
    REQUIRE(res.spirvWords.size() > 5);
    CHECK(res.spirvWords[0] == kSpirvMagic);
    CHECK(res.ptxAssembly.find(".visible .entry updateParticles") != std::string::npos);
    CHECK(res.equivalence.is_equivalent());
    CHECK(res.memorySafety.is_safe());
    CHECK(res.barrierUniformity.is_uniform());
}

TEST_CASE("js_gpu_kernel: rejects object allocation and DOM access with exact line:col diagnostics") {
    SUBCASE("object allocation { x: 1 }") {
        const std::string src =
            "// @kernel\n"
            "function badObj(out, i) {\n"
            "    let obj = { x: 1 };\n"
            "    out[i] = 1.0;\n"
            "}\n";
        auto res = bronze::compileJsGpuKernelSource(src, "bad_obj.js");
        CHECK_FALSE(res.ok);
        CHECK(res.errorMessage.find("bad_obj.js:3:15:") != std::string::npos);
        CHECK(res.errorMessage.find("object allocation") != std::string::npos);
    }

    SUBCASE("DOM access document.body") {
        const std::string src =
            "// @kernel\n"
            "function badDom(out, i) {\n"
            "    let b = document.body;\n"
            "    out[i] = 1.0;\n"
            "}\n";
        auto res = bronze::compileJsGpuKernelSource(src, "bad_dom.js");
        CHECK_FALSE(res.ok);
        CHECK(res.errorMessage.find("bad_dom.js:3:13:") != std::string::npos);
        CHECK(res.errorMessage.find("document") != std::string::npos);
    }
}

TEST_CASE("js_gpu_kernel: CLI subcommand bronze gpu-compile writes .spv and .ptx files") {
    namespace fs = std::filesystem;
    fs::path tmpDir = fs::temp_directory_path() / "bronze_gpu_compile_test";
    fs::create_directories(tmpDir);
    fs::path jsFile = tmpDir / "saxpy_cli.js";
    fs::path spvFile = tmpDir / "saxpy_cli.spv";
    fs::path ptxFile = tmpDir / "saxpy_cli.ptx";

    {
        std::ofstream out(jsFile);
        out << "// @kernel\n"
               "function saxpyCli(out, x, y, a, n) {\n"
               "    let i = global_tid_x();\n"
               "    if (i < n) out[i] = a * x[i] + y[i];\n"
               "}\n";
    }

    std::string jsStr = jsFile.string();
    std::string spvStr = spvFile.string();
    std::string ptxStr = ptxFile.string();
    std::vector<char*> argv = {
        const_cast<char*>("bronze"),
        const_cast<char*>("gpu-compile"),
        jsStr.data(),
        const_cast<char*>("--kernel"),
        const_cast<char*>("saxpyCli"),
        const_cast<char*>("--spirv-out"),
        spvStr.data(),
        const_cast<char*>("--ptx-out"),
        ptxStr.data(),
        const_cast<char*>("--sm"),
        const_cast<char*>("80"),
    };

    std::string err;
    int rc = bronze::runGpuCompileCli(static_cast<int>(argv.size()), argv.data(), &err);
    INFO(err);
    CHECK(rc == 0);
    CHECK(fs::exists(spvFile));
    CHECK(fs::file_size(spvFile) > 20);
    CHECK(fs::exists(ptxFile));
    CHECK(fs::file_size(ptxFile) > 20);

    std::error_code ec;
    fs::remove_all(tmpDir, ec);
}
