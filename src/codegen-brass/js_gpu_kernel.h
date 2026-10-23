#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <brass/codegen/kernel_jit.hpp>
#include <brass/target/ptx/ptx_ir.hpp>
#include <brass/target/ptx/ptx_verifier.hpp>
#include <brass/target/ptx_target.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv/spirv_relaxed_precision.hpp>
#include <brass/target/spirv_target.hpp>
#include <brass/verify/gpu_equiv.hpp>
#include <brass/verify/gpu_kernel_prover.hpp>

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze {

struct GpuCompileOptions {
    std::string kernelName;              // Empty = discover @kernel / @shader or first non-entry function
    brass::target::SpirvStage stage = brass::target::SpirvStage::Compute;
    uint32_t localSizeX = 256;
    uint32_t localSizeY = 1;
    uint32_t localSizeZ = 1;
    uint32_t ptxSmVersion = 80;          // 80 = sm_80, 90 = sm_90
    uint32_t spirvVersion = 0x00010500u; // SPIR-V 1.5
    bool enableRelaxedPrecision = false; // Decorate bounded f32 ops with RelaxedPrecision (FP16)
    float maxAbsFp16 = 1024.0f;          // Safe magnitude bound for RelaxedPrecision promotion
    bool assumeStageInputsUnitRange = true; // Treat Location inputs in [-1, 1] for FP16 analysis
    bool runOptimizations = true;        // SMT guard elim + GPU superopt + scalar cleanup
    bool compileCpu = true;              // Compile single-source CPU callable via Brass KernelJit
    bool verifyEquivalence = true;       // verify_spirv_equivalence (+ verify_spirv_ptx_equivalence for compute)
    bool verifySafety = true;            // verify_gpu_memory_safety + verify_gpu_barrier_uniformity
    bool verifySingleSourceParity = true;// Concrete CPU vs SPIR-V differential execution across sample points
};

using JsGpuCompileOptions = GpuCompileOptions;

struct GpuCompileResult {
    bool ok = false;
    std::string kernelName;
    brass::target::SpirvStage stage = brass::target::SpirvStage::Compute;
    bool hasScalarReturn = false;
    bool cpuGpuProvenEquivalent = false;
    bool singleSourceParityVerified = false;
    uint32_t paritySampleCount = 0;
    uint32_t scalarParamCount = 0;
    float maxCpuGpuUlpDiff = 0.0f;
    std::vector<uint32_t> spirvWords;
    std::string spirvDisassembly;
    std::string ptxAssembly;
    std::vector<brass::spirv::Diagnostic> spirvDiagnostics;
    std::vector<brass::ptx::Diagnostic> ptxDiagnostics;
    brass::spirv::SpirvRelaxedPrecisionReport relaxedPrecisionReport;
    brass::verify::EquivResult spirvEquivalence;
    brass::verify::EquivResult equivalence;
    brass::verify::GpuMemorySafetyResult memorySafety;
    brass::verify::BarrierUniformityResult barrierUniformity;
    brass::codegen::KernelFunction cpuKernel;
    std::vector<std::string> diagnostics;
    std::string errorMessage;

    // Evaluate a single-source scalar/world function on the CPU (via Brass KernelJit)
    float evalCpu(const std::vector<float>& args) const;
    float evalCpu1D(float x) const { return evalCpu({x}); }
    float evalCpu2D(float x, float z) const { return evalCpu({x, z}); }
    float evalCpu3D(float x, float y, float z) const { return evalCpu({x, y, z}); }

    // Evaluate the compiled Vulkan SPIR-V module on the same scalar inputs
    float evalSpirv(const std::vector<float>& args) const;
    float evalSpirv1D(float x) const { return evalSpirv({x}); }
    float evalSpirv2D(float x, float z) const { return evalSpirv({x, z}); }
    float evalSpirv3D(float x, float y, float z) const { return evalSpirv({x, y, z}); }
};

using JsGpuCompileResult = GpuCompileResult;

namespace gpu_detail {
bool compileSingleSourceCpuAndVerify(
    brass::Function& mirFn,
    const brass::spirv::Module& spvMod,
    const GpuCompileOptions& opts,
    GpuCompileResult& res);
}  // namespace gpu_detail

// Compile a single JS function from source code or file to both Vulkan SPIR-V 1.5
// and NVIDIA PTX assembly, running subset verification, MIR optimization, and
// cross-backend SMT equivalence + GPU memory/barrier safety verification.
GpuCompileResult compileJsGpuKernelSource(
    const std::string& sourceText,
    const std::string& sourcePath = "kernel.js",
    const GpuCompileOptions& opts = {});

GpuCompileResult compileJsGpuKernelFile(
    const std::string& sourcePath,
    const GpuCompileOptions& opts = {});

// CLI entry point for:
//   bronze gpu-compile <file.js> [--kernel <fn>] [--spirv-out <f.spv>] [--ptx-out <f.ptx>] [--sm 80|90]
int runGpuCompileCli(int argc, char** argv, std::string* errOut = nullptr);

}  // namespace bronze
