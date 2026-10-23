#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <brass/target/ptx/ptx_ir.hpp>
#include <brass/target/ptx/ptx_verifier.hpp>
#include <brass/target/ptx_target.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv_target.hpp>
#include <brass/verify/gpu_equiv.hpp>
#include <brass/verify/gpu_kernel_prover.hpp>

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze {

struct GpuCompileOptions {
    std::string kernelName;              // Empty = discover @kernel or first non-entry function
    uint32_t localSizeX = 256;
    uint32_t localSizeY = 1;
    uint32_t localSizeZ = 1;
    uint32_t ptxSmVersion = 80;          // 80 = sm_80, 90 = sm_90
    uint32_t spirvVersion = 0x00010500u; // SPIR-V 1.5
    bool runOptimizations = true;        // SMT guard elim + GPU superopt + scalar cleanup
    bool verifyEquivalence = true;       // verify_spirv_ptx_equivalence
    bool verifySafety = true;            // verify_gpu_memory_safety + verify_gpu_barrier_uniformity
};

struct GpuCompileResult {
    bool ok = false;
    std::string kernelName;
    std::vector<uint32_t> spirvWords;
    std::string spirvDisassembly;
    std::string ptxAssembly;
    std::vector<brass::spirv::Diagnostic> spirvDiagnostics;
    std::vector<brass::ptx::Diagnostic> ptxDiagnostics;
    brass::verify::EquivResult equivalence;
    brass::verify::GpuMemorySafetyResult memorySafety;
    brass::verify::BarrierUniformityResult barrierUniformity;
    std::vector<std::string> diagnostics;
    std::string errorMessage;
};

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
