#include "codegen-brass/js_gpu_kernel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <brass/target/spirv/spirv_binary_reader.hpp>
#include <spirv/unified1/GLSL.std.450.h>

namespace bronze {

namespace {

uint32_t f32ToBits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(float));
    return u;
}

float bitsToF32(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(float));
    return f;
}

float ulpDiffF32(float a, float b) noexcept {
    if (std::isnan(a) && std::isnan(b)) return 0.0f;
    if (std::isnan(a) || std::isnan(b)) return INFINITY;
    if (a == b) return 0.0f;
    int32_t ia = static_cast<int32_t>(f32ToBits(a));
    int32_t ib = static_cast<int32_t>(f32ToBits(b));
    if (ia < 0) ia = static_cast<int32_t>(0x80000000u) - ia;
    if (ib < 0) ib = static_cast<int32_t>(0x80000000u) - ib;
    return static_cast<float>(std::abs(static_cast<int64_t>(ia) - static_cast<int64_t>(ib)));
}

}  // namespace

float GpuCompileResult::evalCpu(const std::vector<float>& args) const {
    if (!cpuKernel.is_valid()) return std::nanf("");
    float out = 0.0f;
    float a0 = args.size() > 0 ? args[0] : 0.0f;
    float a1 = args.size() > 1 ? args[1] : 0.0f;
    float a2 = args.size() > 2 ? args[2] : 0.0f;
    float a3 = args.size() > 3 ? args[3] : 0.0f;
    switch (scalarParamCount) {
    case 0: {
        using Fn = void (*)(float*);
        cpuKernel.as<Fn>()(&out);
        break;
    }
    case 1: {
        using Fn = void (*)(float, float*);
        cpuKernel.as<Fn>()(a0, &out);
        break;
    }
    case 2: {
        using Fn = void (*)(float, float, float*);
        cpuKernel.as<Fn>()(a0, a1, &out);
        break;
    }
    case 3: {
        using Fn = void (*)(float, float, float, float*);
        cpuKernel.as<Fn>()(a0, a1, a2, &out);
        break;
    }
    default: {
        using Fn = void (*)(float, float, float, float, float*);
        cpuKernel.as<Fn>()(a0, a1, a2, a3, &out);
        break;
    }
    }
    return out;
}

float GpuCompileResult::evalSpirv(const std::vector<float>& args) const {
    if (spirvWords.empty()) return std::nanf("");
    brass::spirv::Module mod;
    if (!brass::spirv::parse_binary(spirvWords, mod)) return std::nanf("");
    if (mod.functions.empty() || mod.functions[0].blocks.empty()) return std::nanf("");

    constexpr uint64_t kRetOutAddr = 0x7000000000000000ULL;
    std::unordered_map<brass::spirv::Id, uint64_t> vals;
    std::unordered_map<brass::spirv::Id, uint32_t> pcChains;
    brass::spirv::Id pushConstVar = 0;

    for (const auto& g : mod.globals) {
        if (g.op == spv::OpConstant) {
            uint64_t bits = g.operands.empty() ? 0ULL : static_cast<uint64_t>(g.operands[0].value);
            if (g.operands.size() >= 2) {
                bits |= (static_cast<uint64_t>(g.operands[1].value) << 32);
            }
            vals[g.result] = bits;
        } else if (g.op == spv::OpConstantTrue) {
            vals[g.result] = 1ULL;
        } else if (g.op == spv::OpConstantFalse || g.op == spv::OpConstantNull) {
            vals[g.result] = 0ULL;
        } else if (g.op == spv::OpVariable && !g.operands.empty() &&
                   g.operands[0].value == spv::StorageClassPushConstant) {
            pushConstVar = g.result;
        }
    }

    const auto& fn = mod.functions[0];
    std::unordered_map<brass::spirv::Id, size_t> labelToIdx;
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        labelToIdx[fn.blocks[i].label] = i;
    }

    size_t curBlock = 0;
    brass::spirv::Id prevLabel = 0;
    uint32_t retBits = 0;

    auto getU32 = [&](brass::spirv::Id id) -> uint32_t {
        auto it = vals.find(id);
        return it != vals.end() ? static_cast<uint32_t>(it->second) : 0u;
    };
    auto getI32 = [&](brass::spirv::Id id) -> int32_t {
        return static_cast<int32_t>(getU32(id));
    };
    auto getF32 = [&](brass::spirv::Id id) -> float {
        return bitsToF32(getU32(id));
    };

    for (uint32_t step = 0; step < 4096; ++step) {
        const auto& bb = fn.blocks[curBlock];
        std::unordered_map<brass::spirv::Id, uint64_t> phiUpdates;
        for (const auto& inst : bb.insts) {
            if (inst.op != spv::OpPhi) break;
            for (size_t i = 0; i + 1 < inst.operands.size(); i += 2) {
                if (inst.operands[i + 1].value == prevLabel) {
                    phiUpdates[inst.result] = vals[inst.operands[i].value];
                    break;
                }
            }
        }
        for (const auto& [k, v] : phiUpdates) vals[k] = v;

        for (const auto& inst : bb.insts) {
            if (inst.op == spv::OpPhi || inst.op == spv::OpSelectionMerge || inst.op == spv::OpLoopMerge) {
                continue;
            }
            switch (inst.op) {
            case spv::OpAccessChain:
            case spv::OpInBoundsAccessChain:
                if (inst.operands.size() >= 2 && inst.operands[0].value == pushConstVar) {
                    pcChains[inst.result] = getU32(inst.operands[1].value);
                }
                break;
            case spv::OpLoad: {
                brass::spirv::Id ptrId = inst.operands[0].value;
                if (pcChains.count(ptrId)) {
                    uint32_t idx = pcChains[ptrId];
                    if (idx < scalarParamCount) {
                        float fv = idx < args.size() ? args[idx] : 0.0f;
                        vals[inst.result] = f32ToBits(fv);
                    } else {
                        vals[inst.result] = kRetOutAddr;
                    }
                } else {
                    vals[inst.result] = vals[ptrId];
                }
                break;
            }
            case spv::OpConvertUToPtr:
            case spv::OpConvertPtrToU:
            case spv::OpBitcast:
            case spv::OpUConvert:
                vals[inst.result] = vals[inst.operands[0].value];
                break;
            case spv::OpSConvert:
                vals[inst.result] = static_cast<uint64_t>(static_cast<int64_t>(getI32(inst.operands[0].value)));
                break;
            case spv::OpStore: {
                uint64_t addr = vals[inst.operands[0].value];
                if (addr == kRetOutAddr) {
                    retBits = getU32(inst.operands[1].value);
                }
                break;
            }
            case spv::OpFNegate:
                vals[inst.result] = f32ToBits(-getF32(inst.operands[0].value));
                break;
            case spv::OpFAdd:
                vals[inst.result] = f32ToBits(getF32(inst.operands[0].value) + getF32(inst.operands[1].value));
                break;
            case spv::OpFSub:
                vals[inst.result] = f32ToBits(getF32(inst.operands[0].value) - getF32(inst.operands[1].value));
                break;
            case spv::OpFMul:
                vals[inst.result] = f32ToBits(getF32(inst.operands[0].value) * getF32(inst.operands[1].value));
                break;
            case spv::OpFDiv:
                vals[inst.result] = f32ToBits(getF32(inst.operands[0].value) / getF32(inst.operands[1].value));
                break;
            case spv::OpFRem:
                vals[inst.result] = f32ToBits(std::fmod(getF32(inst.operands[0].value), getF32(inst.operands[1].value)));
                break;
            case spv::OpSNegate:
                vals[inst.result] = static_cast<uint32_t>(-getI32(inst.operands[0].value));
                break;
            case spv::OpIAdd:
                vals[inst.result] = vals[inst.operands[0].value] + vals[inst.operands[1].value];
                break;
            case spv::OpISub:
                vals[inst.result] = vals[inst.operands[0].value] - vals[inst.operands[1].value];
                break;
            case spv::OpIMul:
                vals[inst.result] = vals[inst.operands[0].value] * vals[inst.operands[1].value];
                break;
            case spv::OpSDiv: {
                int32_t b = getI32(inst.operands[1].value);
                vals[inst.result] = static_cast<uint32_t>(b != 0 ? getI32(inst.operands[0].value) / b : 0);
                break;
            }
            case spv::OpUDiv: {
                uint32_t b = getU32(inst.operands[1].value);
                vals[inst.result] = b != 0 ? getU32(inst.operands[0].value) / b : 0u;
                break;
            }
            case spv::OpSRem:
            case spv::OpSMod: {
                int32_t b = getI32(inst.operands[1].value);
                vals[inst.result] = static_cast<uint32_t>(b != 0 ? getI32(inst.operands[0].value) % b : 0);
                break;
            }
            case spv::OpUMod: {
                uint32_t b = getU32(inst.operands[1].value);
                vals[inst.result] = b != 0 ? getU32(inst.operands[0].value) % b : 0u;
                break;
            }
            case spv::OpBitwiseAnd:
            case spv::OpLogicalAnd:
                vals[inst.result] = getU32(inst.operands[0].value) & getU32(inst.operands[1].value);
                break;
            case spv::OpBitwiseOr:
            case spv::OpLogicalOr:
                vals[inst.result] = getU32(inst.operands[0].value) | getU32(inst.operands[1].value);
                break;
            case spv::OpBitwiseXor:
                vals[inst.result] = getU32(inst.operands[0].value) ^ getU32(inst.operands[1].value);
                break;
            case spv::OpNot:
                vals[inst.result] = getU32(inst.operands[0].value) ^ 0xFFFFFFFFu;
                break;
            case spv::OpLogicalNot:
                vals[inst.result] = getU32(inst.operands[0].value) == 0 ? 1u : 0u;
                break;
            case spv::OpShiftLeftLogical:
                vals[inst.result] = getU32(inst.operands[0].value) << (getU32(inst.operands[1].value) & 31u);
                break;
            case spv::OpShiftRightLogical:
                vals[inst.result] = getU32(inst.operands[0].value) >> (getU32(inst.operands[1].value) & 31u);
                break;
            case spv::OpShiftRightArithmetic:
                vals[inst.result] = static_cast<uint32_t>(getI32(inst.operands[0].value) >> (getU32(inst.operands[1].value) & 31u));
                break;
            case spv::OpIEqual:
                vals[inst.result] = getU32(inst.operands[0].value) == getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpINotEqual:
                vals[inst.result] = getU32(inst.operands[0].value) != getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpSLessThan:
                vals[inst.result] = getI32(inst.operands[0].value) < getI32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpSLessThanEqual:
                vals[inst.result] = getI32(inst.operands[0].value) <= getI32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpSGreaterThan:
                vals[inst.result] = getI32(inst.operands[0].value) > getI32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpSGreaterThanEqual:
                vals[inst.result] = getI32(inst.operands[0].value) >= getI32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpULessThan:
                vals[inst.result] = getU32(inst.operands[0].value) < getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpULessThanEqual:
                vals[inst.result] = getU32(inst.operands[0].value) <= getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpUGreaterThan:
                vals[inst.result] = getU32(inst.operands[0].value) > getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpUGreaterThanEqual:
                vals[inst.result] = getU32(inst.operands[0].value) >= getU32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFOrdEqual:
                vals[inst.result] = getF32(inst.operands[0].value) == getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFUnordNotEqual:
            case spv::OpFOrdNotEqual:
                vals[inst.result] = getF32(inst.operands[0].value) != getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFOrdLessThan:
                vals[inst.result] = getF32(inst.operands[0].value) < getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFOrdLessThanEqual:
                vals[inst.result] = getF32(inst.operands[0].value) <= getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFOrdGreaterThan:
                vals[inst.result] = getF32(inst.operands[0].value) > getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpFOrdGreaterThanEqual:
                vals[inst.result] = getF32(inst.operands[0].value) >= getF32(inst.operands[1].value) ? 1u : 0u;
                break;
            case spv::OpSelect:
                vals[inst.result] = getU32(inst.operands[0].value) != 0 ? vals[inst.operands[1].value] : vals[inst.operands[2].value];
                break;
            case spv::OpConvertSToF:
                vals[inst.result] = f32ToBits(static_cast<float>(getI32(inst.operands[0].value)));
                break;
            case spv::OpConvertUToF:
                vals[inst.result] = f32ToBits(static_cast<float>(getU32(inst.operands[0].value)));
                break;
            case spv::OpConvertFToS:
                vals[inst.result] = static_cast<uint32_t>(static_cast<int32_t>(getF32(inst.operands[0].value)));
                break;
            case spv::OpConvertFToU:
                vals[inst.result] = static_cast<uint32_t>(getF32(inst.operands[0].value));
                break;
            case spv::OpExtInst: {
                uint32_t glslOp = inst.operands[1].value;
                float a0 = inst.operands.size() > 2 ? getF32(inst.operands[2].value) : 0.0f;
                float a1 = inst.operands.size() > 3 ? getF32(inst.operands[3].value) : 0.0f;
                float a2 = inst.operands.size() > 4 ? getF32(inst.operands[4].value) : 0.0f;
                float r = 0.0f;
                switch (glslOp) {
                case GLSLstd450Sin: r = std::sin(a0); break;
                case GLSLstd450Cos: r = std::cos(a0); break;
                case GLSLstd450Tanh: r = std::tanh(a0); break;
                case GLSLstd450Sqrt: r = std::sqrt(a0); break;
                case GLSLstd450InverseSqrt: r = 1.0f / std::sqrt(a0); break;
                case GLSLstd450FAbs: r = std::fabs(a0); break;
                case GLSLstd450Floor: r = std::floor(a0); break;
                case GLSLstd450Ceil: r = std::ceil(a0); break;
                case GLSLstd450Round:
                case GLSLstd450RoundEven: r = std::nearbyint(a0); break;
                case GLSLstd450Trunc: r = std::trunc(a0); break;
                case GLSLstd450Fract: r = a0 - std::floor(a0); break;
                case GLSLstd450FSign: r = (a0 > 0.0f) ? 1.0f : ((a0 < 0.0f) ? -1.0f : 0.0f); break;
                case GLSLstd450Exp: r = std::exp(a0); break;
                case GLSLstd450Log: r = std::log(a0); break;
                case GLSLstd450Exp2: r = std::exp2(a0); break;
                case GLSLstd450Log2: r = std::log2(a0); break;
                case GLSLstd450FMin:
                case GLSLstd450NMin: r = std::fmin(a0, a1); break;
                case GLSLstd450FMax:
                case GLSLstd450NMax: r = std::fmax(a0, a1); break;
                case GLSLstd450Step: r = (a1 < a0) ? 0.0f : 1.0f; break;
                case GLSLstd450FClamp:
                case GLSLstd450NClamp: r = std::fmin(std::fmax(a0, a1), a2); break;
                case GLSLstd450FMix: r = a0 + (a1 - a0) * a2; break;
                case GLSLstd450SmoothStep: {
                    float t = std::fmin(std::fmax((a2 - a0) / (a1 - a0), 0.0f), 1.0f);
                    r = t * t * (3.0f - 2.0f * t);
                    break;
                }
                case GLSLstd450Fma: r = std::fma(a0, a1, a2); break;
                default: break;
                }
                vals[inst.result] = f32ToBits(r);
                break;
            }
            case spv::OpBranch:
                prevLabel = bb.label;
                curBlock = labelToIdx.at(inst.operands[0].value);
                goto next_block;
            case spv::OpBranchConditional: {
                prevLabel = bb.label;
                brass::spirv::Id dst = getU32(inst.operands[0].value) != 0
                                           ? inst.operands[1].value
                                           : inst.operands[2].value;
                curBlock = labelToIdx.at(dst);
                goto next_block;
            }
            case spv::OpReturn:
                return bitsToF32(retBits);
            default:
                break;
            }
        }
        break;
    next_block:;
    }
    return bitsToF32(retBits);
}

namespace gpu_detail {

bool compileSingleSourceCpuAndVerify(
    brass::Function& mirFn,
    const brass::spirv::Module& /*spvMod*/,
    const GpuCompileOptions& opts,
    GpuCompileResult& res) {
    if (!res.hasScalarReturn || !opts.compileCpu || res.scalarParamCount > 4) {
        return true;
    }

    brass::codegen::KernelOptions kopts;
    kopts.enable_optimizations = false;
    kopts.enable_parallel = false;
    brass::codegen::KernelJit jit(kopts);
    res.cpuKernel = jit.compile(mirFn);

    if (opts.verifySingleSourceParity) {
        static const float kCoords[] = {-16.0f, -5.5f, -1.25f, 0.0f, 0.75f, 3.5f, 12.25f};
        float maxUlp = 0.0f;
        uint32_t samples = 0;

        auto checkPoint = [&](const std::vector<float>& pt) -> bool {
            float cpuVal = res.evalCpu(pt);
            float gpuVal = res.evalSpirv(pt);
            float ulp = ulpDiffF32(cpuVal, gpuVal);
            maxUlp = std::max(maxUlp, ulp);
            ++samples;
            if (ulp > 0.0f) {
                std::ostringstream oss;
                oss << "Single-source CPU/SPIR-V parity mismatch: cpu=" << cpuVal
                    << " spirv=" << gpuVal << " (" << ulp << " ULP)";
                res.errorMessage = oss.str();
                res.diagnostics.push_back(res.errorMessage);
                return false;
            }
            return true;
        };

        if (res.scalarParamCount == 0) {
            if (!checkPoint({})) return false;
        } else if (res.scalarParamCount == 1) {
            for (float x : kCoords) {
                if (!checkPoint({x})) return false;
            }
        } else if (res.scalarParamCount == 2) {
            for (float x : kCoords) {
                for (float z : kCoords) {
                    if (!checkPoint({x, z})) return false;
                }
            }
        } else {
            for (float x : {-8.0f, 0.0f, 4.5f}) {
                for (float y : {-4.0f, 1.25f, 6.0f}) {
                    for (float z : {-8.0f, 0.0f, 4.5f}) {
                        if (!checkPoint({x, y, z, 1.0f})) return false;
                    }
                }
            }
        }
        res.paritySampleCount = samples;
        res.maxCpuGpuUlpDiff = maxUlp;
        res.singleSourceParityVerified = true;
    }

    return true;
}

}  // namespace gpu_detail

GpuCompileResult compileJsGpuKernelFile(
    const std::string& sourcePath,
    const GpuCompileOptions& opts) {
    std::ifstream in(sourcePath, std::ios::binary);
    if (!in) {
        GpuCompileResult res;
        res.errorMessage = "error: cannot read " + sourcePath + "\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return compileJsGpuKernelSource(ss.str(), sourcePath, opts);
}

int runGpuCompileCli(int argc, char** argv, std::string* errOut) {
    auto reportErr = [&](const std::string& msg) {
        if (errOut) *errOut = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    };

    auto parseStage = [&](const std::string& s, brass::target::SpirvStage& outStage) -> bool {
        if (s == "compute") { outStage = brass::target::SpirvStage::Compute; return true; }
        if (s == "vertex") { outStage = brass::target::SpirvStage::Vertex; return true; }
        if (s == "fragment") { outStage = brass::target::SpirvStage::Fragment; return true; }
        return false;
    };

    std::string sourcePath;
    std::string spirvOutPath;
    std::string ptxOutPath;
    GpuCompileOptions opts;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--kernel" && i + 1 < argc) opts.kernelName = argv[++i];
        else if (arg.rfind("--kernel=", 0) == 0) opts.kernelName = arg.substr(9);
        else if (arg == "--spirv-out" && i + 1 < argc) spirvOutPath = argv[++i];
        else if (arg.rfind("--spirv-out=", 0) == 0) spirvOutPath = arg.substr(12);
        else if (arg == "--ptx-out" && i + 1 < argc) ptxOutPath = argv[++i];
        else if (arg.rfind("--ptx-out=", 0) == 0) ptxOutPath = arg.substr(10);
        else if (arg == "--sm" && i + 1 < argc) opts.ptxSmVersion = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg.rfind("--sm=", 0) == 0) opts.ptxSmVersion = static_cast<uint32_t>(std::strtoul(arg.substr(5).c_str(), nullptr, 10));
        else if (arg == "--stage" && i + 1 < argc) {
            std::string st = argv[++i];
            if (!parseStage(st, opts.stage)) return reportErr("error: unknown --stage " + st + "\n");
        } else if (arg.rfind("--stage=", 0) == 0) {
            std::string st = arg.substr(8);
            if (!parseStage(st, opts.stage)) return reportErr("error: unknown --stage " + st + "\n");
        } else if (arg == "--fp16") {
            opts.enableRelaxedPrecision = true;
        } else if (sourcePath.empty()) sourcePath = arg;
        else return reportErr("error: unexpected argument " + arg + "\n");
    }

    if (sourcePath.empty()) return reportErr("error: missing <file.js> for gpu-compile\n");

    GpuCompileResult res = compileJsGpuKernelFile(sourcePath, opts);
    if (!res.ok) return reportErr(res.errorMessage);

    if (!spirvOutPath.empty()) {
        std::ofstream out(spirvOutPath, std::ios::binary);
        if (!out) return reportErr("error: cannot write SPIR-V output " + spirvOutPath + "\n");
        out.write(reinterpret_cast<const char*>(res.spirvWords.data()),
                  static_cast<std::streamsize>(res.spirvWords.size() * sizeof(uint32_t)));
    }
    if (!ptxOutPath.empty()) {
        std::ofstream out(ptxOutPath, std::ios::binary);
        if (!out) return reportErr("error: cannot write PTX output " + ptxOutPath + "\n");
        out.write(res.ptxAssembly.data(), static_cast<std::streamsize>(res.ptxAssembly.size()));
    }
    if (spirvOutPath.empty() && ptxOutPath.empty()) {
        const std::string& text = !res.ptxAssembly.empty() ? res.ptxAssembly : res.spirvDisassembly;
        std::fputs(text.c_str(), stdout);
    }
    return 0;
}

}  // namespace bronze
