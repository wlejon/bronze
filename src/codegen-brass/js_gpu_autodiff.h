#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <brass/codegen/kernel_jit.hpp>
#include <brass/target/ptx/ptx_ir.hpp>
#include <brass/target/ptx/ptx_verifier.hpp>
#include <brass/target/ptx_target.hpp>
#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv/spirv_relaxed_precision.hpp>
#include <brass/target/spirv_target.hpp>
#include <brass/verify/gpu_equiv.hpp>
#include <brass/verify/gpu_kernel_prover.hpp>

namespace bronze {

struct GpuAutodiffSample {
    float value = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
};

struct GpuAutodiffOptions {
    std::string kernelName;
    brass::target::SpirvStage stage = brass::target::SpirvStage::Compute;
    uint32_t spirvVersion = 0x00010500;
    uint32_t ptxSmVersion = 80;
    uint32_t localSizeX = 64;
    uint32_t localSizeY = 1;
    uint32_t localSizeZ = 1;
    bool runOptimizations = true;
    bool verifyEquivalence = true;
    bool verifySafety = true;
    bool compileCpu = true;
    bool verifySingleSourceParity = true;
    bool enableRelaxedPrecision = false;
    float maxAbsFp16 = 60000.0f;
    bool allowUnannotated = true;
    bool forceHeightfieldNormal = false;
};

struct GpuAutodiffResult {
    bool ok = false;
    std::string kernelName;
    brass::target::SpirvStage stage = brass::target::SpirvStage::Compute;
    uint32_t scalarParamCount = 0;
    bool isHeightfield2D = false;

    std::vector<uint32_t> spirvWords;
    std::string spirvDisassembly;
    std::string ptxAssembly;

    std::vector<brass::spirv::Diagnostic> spirvDiagnostics;
    std::vector<brass::ptx::Diagnostic> ptxDiagnostics;
    brass::verify::EquivResult equivalence;
    brass::verify::EquivResult spirvEquivalence;
    brass::verify::GpuMemorySafetyResult memorySafety;
    brass::verify::BarrierUniformityResult barrierUniformity;
    brass::spirv::ComputeShaderAuditReport shaderSafetyReport;
    brass::spirv::SpirvRelaxedPrecisionReport relaxedPrecisionReport;

    brass::codegen::KernelFunction cpuKernel;
    bool cpuGpuProvenEquivalent = false;
    bool provenSafe = false;
    bool singleSourceParityVerified = false;
    uint32_t paritySampleCount = 0;
    float maxCpuGpuUlpDiff = 0.0f;

    size_t gvnExpressionsEliminated = 0;
    uint32_t sinCallCountBeforeGvn = 0;
    uint32_t cosCallCountBeforeGvn = 0;
    uint32_t sinCallCountAfterGvn = 0;
    uint32_t cosCallCountAfterGvn = 0;
    uint32_t totalInstCountBeforeOpt = 0;
    uint32_t totalInstCountAfterOpt = 0;

    std::vector<std::string> diagnostics;
    std::string errorMessage;

    GpuAutodiffSample evalGradCpu(const std::vector<float>& args) const;
    GpuAutodiffSample evalGradCpu(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) const;
    void evalGradBatchCpu(const float* coords, size_t pointCount, size_t stride, float* outValues7) const;

    GpuAutodiffSample evalGradSpirv(const std::vector<float>& args) const;
    GpuAutodiffSample evalGradSpirv(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f) const;
};

using JsGpuAutodiffOptions = GpuAutodiffOptions;
using JsGpuAutodiffResult = GpuAutodiffResult;

brass::spirv::ComputeShaderAuditReport prove_spirv_shader_safety(
    const brass::spirv::Module& spvMod);

GpuAutodiffResult compileJsGpuAutodiffSource(
    const std::string& source,
    const std::string& sourcePath = "<shader>",
    const GpuAutodiffOptions& opts = {});

}  // namespace bronze
