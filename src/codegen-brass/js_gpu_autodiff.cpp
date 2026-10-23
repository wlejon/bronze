#include "codegen-brass/js_gpu_autodiff.h"
#include "codegen-brass/js_gpu_subset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/gpu_superopt.hpp>
#include <brass/mir/gvn.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/runtime_symbols.hpp>
#include <brass/mir/scalar_opt.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/target/ptx/ptx_cleanup.hpp>
#include <brass/target/ptx/ptx_isel.hpp>
#include <brass/target/ptx/ptx_printer.hpp>
#include <brass/target/ptx/ptx_verifier.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv/spirv_isel.hpp>
#include <spirv/unified1/GLSL.std.450.h>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "types/infer.h"

namespace bronze {

namespace {

uint32_t f32ToBits(float f) noexcept { uint32_t u = 0; std::memcpy(&u, &f, sizeof(float)); return u; }
float bitsToF32(uint32_t u) noexcept { float f = 0.0f; std::memcpy(&f, &u, sizeof(float)); return f; }

float ulpDiffF32(float a, float b) noexcept {
    if (std::isnan(a) && std::isnan(b)) return 0.0f;
    if (std::isnan(a) || std::isnan(b)) return INFINITY;
    if (a == b) return 0.0f;
    int32_t ia = static_cast<int32_t>(f32ToBits(a)), ib = static_cast<int32_t>(f32ToBits(b));
    if (ia < 0) ia = static_cast<int32_t>(0x80000000u) - ia;
    if (ib < 0) ib = static_cast<int32_t>(0x80000000u) - ib;
    return static_cast<float>(std::abs(static_cast<int64_t>(ia) - static_cast<int64_t>(ib)));
}

void countTrigAndTotalInsts(const brass::Function& fn, uint32_t* outSinCount, uint32_t* outCosCount, uint32_t* outTotalCount) {
    uint32_t sinC = 0, cosC = 0, totC = 0;
    for (const brass::BasicBlock* bb : fn.blocks()) {
        if (!bb) continue;
        for (const brass::Instruction* inst : *bb) {
            if (!inst) continue;
            ++totC;
            if (inst->opcode() == brass::Opcode::call) {
                if (inst->symbol() == "ptx_sin" || inst->symbol() == "sin" || inst->symbol() == "sinf") ++sinC;
                else if (inst->symbol() == "ptx_cos" || inst->symbol() == "cos" || inst->symbol() == "cosf") ++cosC;
            }
        }
    }
    if (outSinCount) *outSinCount = sinC;
    if (outCosCount) *outCosCount = cosC;
    if (outTotalCount) *outTotalCount = totC;
}

bool removeDeadPureCalls(brass::Function& fn) {
    bool anyRemoved = false, progress = true;
    while (progress) {
        progress = false;
        std::unordered_map<const brass::Value*, uint32_t> uses;
        for (const brass::BasicBlock* bb : fn.blocks()) {
            if (!bb) continue;
            for (const brass::Instruction* inst : *bb) {
                if (!inst) continue;
                for (size_t i = 0; i < inst->operand_count(); ++i) if (inst->operand(i)) ++uses[inst->operand(i)];
                for (const brass::Value* a : inst->branch_target().args) if (a) ++uses[a];
                for (const brass::Value* a : inst->true_target().args) if (a) ++uses[a];
                for (const brass::Value* a : inst->false_target().args) if (a) ++uses[a];
            }
        }
        for (brass::BasicBlock* bb : fn.blocks()) {
            if (!bb) continue;
            for (brass::Instruction* cur = bb->tail(); cur;) {
                brass::Instruction* prev = cur->prev();
                if (cur->opcode() == brass::Opcode::call && brass::callee_has_role(*cur, brass::SymbolRole::Pure) &&
                    cur->produces_value() && cur->result() && uses[cur->result()] == 0) {
                    bb->remove_instruction(cur);
                    progress = anyRemoved = true;
                }
                cur = prev;
            }
        }
    }
    return anyRemoved;
}

struct DualVal { brass::Value* val = nullptr; std::array<brass::Value*, 3> d{nullptr, nullptr, nullptr}; };

}  // namespace

brass::spirv::ComputeShaderAuditReport prove_spirv_shader_safety(const brass::spirv::Module& spvMod) {
    return brass::spirv::audit_compute_shader(spvMod);
}

GpuAutodiffSample GpuAutodiffResult::evalGradCpu(const std::vector<float>& args) const {
    GpuAutodiffSample sample;
    if (!cpuKernel.is_valid()) { sample.value = std::nanf(""); return sample; }
    float out[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    float a0 = args.size() > 0 ? args[0] : 0.0f, a1 = args.size() > 1 ? args[1] : 0.0f;
    float a2 = args.size() > 2 ? args[2] : 0.0f, a3 = args.size() > 3 ? args[3] : 0.0f;
    switch (scalarParamCount) {
    case 0: cpuKernel.as<void (*)(float*)>()(out); break;
    case 1: cpuKernel.as<void (*)(float, float*)>()(a0, out); break;
    case 2: cpuKernel.as<void (*)(float, float, float*)>()(a0, a1, out); break;
    case 3: cpuKernel.as<void (*)(float, float, float, float*)>()(a0, a1, a2, out); break;
    default: cpuKernel.as<void (*)(float, float, float, float, float*)>()(a0, a1, a2, a3, out); break;
    }
    sample.value = out[0]; sample.dx = out[1]; sample.dy = out[2]; sample.dz = out[3];
    sample.nx = out[4];    sample.ny = out[5]; sample.nz = out[6];
    return sample;
}

GpuAutodiffSample GpuAutodiffResult::evalGradCpu(float x, float y, float z, float w) const {
    return evalGradCpu(std::vector<float>{x, y, z, w});
}

void GpuAutodiffResult::evalGradBatchCpu(const float* coords, size_t pointCount, size_t stride, float* outValues7) const {
    if (!coords || !outValues7 || pointCount == 0 || stride == 0) return;
    for (size_t i = 0; i < pointCount; ++i) {
        const float* pt = coords + i * stride;
        GpuAutodiffSample s = evalGradCpu(stride >= 1 ? pt[0] : 0.0f, stride >= 2 ? pt[1] : 0.0f,
                                          stride >= 3 ? pt[2] : 0.0f, stride >= 4 ? pt[3] : 0.0f);
        float* dst = outValues7 + i * 7;
        dst[0] = s.value; dst[1] = s.dx; dst[2] = s.dy; dst[3] = s.dz;
        dst[4] = s.nx;    dst[5] = s.ny; dst[6] = s.nz;
    }
}

GpuAutodiffSample GpuAutodiffResult::evalGradSpirv(const std::vector<float>& args) const {
    GpuAutodiffSample sample;
    brass::spirv::Module mod;
    if (spirvWords.empty() || !brass::spirv::parse_binary(spirvWords, mod) ||
        mod.functions.empty() || mod.functions[0].blocks.empty()) {
        sample.value = std::nanf("");
        return sample;
    }

    constexpr uint64_t kRetOutAddr = 0x7000000000000000ULL;
    std::unordered_map<brass::spirv::Id, uint64_t> vals;
    std::unordered_map<brass::spirv::Id, uint32_t> pcChains;
    brass::spirv::Id pushConstVar = 0;

    for (const auto& g : mod.globals) {
        if (g.op == spv::OpConstant) {
            uint64_t bits = g.operands.empty() ? 0ULL : static_cast<uint64_t>(g.operands[0].value);
            if (g.operands.size() >= 2) bits |= (static_cast<uint64_t>(g.operands[1].value) << 32);
            vals[g.result] = bits;
        } else if (g.op == spv::OpConstantTrue) vals[g.result] = 1ULL;
        else if (g.op == spv::OpConstantFalse || g.op == spv::OpConstantNull) vals[g.result] = 0ULL;
        else if (g.op == spv::OpVariable && !g.operands.empty() &&
                 g.operands[0].value == spv::StorageClassPushConstant) pushConstVar = g.result;
    }

    const auto& fn = mod.functions[0];
    std::unordered_map<brass::spirv::Id, size_t> labelToIdx;
    for (size_t i = 0; i < fn.blocks.size(); ++i) labelToIdx[fn.blocks[i].label] = i;

    size_t curBlock = 0;
    brass::spirv::Id prevLabel = 0;
    float out[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    auto getU32 = [&](brass::spirv::Id id) -> uint32_t {
        auto it = vals.find(id);
        return it != vals.end() ? static_cast<uint32_t>(it->second) : 0u;
    };
    auto getI32 = [&](brass::spirv::Id id) -> int32_t { return static_cast<int32_t>(getU32(id)); };
    auto getF32 = [&](brass::spirv::Id id) -> float { return bitsToF32(getU32(id)); };

    for (uint32_t step = 0; step < 4096; ++step) {
        const auto& bb = fn.blocks[curBlock];
        std::unordered_map<brass::spirv::Id, uint64_t> phiUpdates;
        for (const auto& inst : bb.insts) {
            if (inst.op != spv::OpPhi) break;
            for (size_t i = 0; i + 1 < inst.operands.size(); i += 2) {
                if (inst.operands[i + 1].value == prevLabel) { phiUpdates[inst.result] = vals[inst.operands[i].value]; break; }
            }
        }
        for (const auto& [k, v] : phiUpdates) vals[k] = v;

        for (const auto& inst : bb.insts) {
            const auto& op = inst.operands;
            switch (inst.op) {
            case spv::OpAccessChain:
            case spv::OpInBoundsAccessChain:
                if (op.size() >= 2 && op[0].value == pushConstVar) pcChains[inst.result] = getU32(op[1].value);
                break;
            case spv::OpLoad: {
                brass::spirv::Id ptrId = op[0].value;
                if (pcChains.count(ptrId)) {
                    uint32_t idx = pcChains[ptrId];
                    vals[inst.result] = (idx < scalarParamCount) ? f32ToBits(idx < args.size() ? args[idx] : 0.0f) : kRetOutAddr;
                } else vals[inst.result] = vals[ptrId];
                break;
            }
            case spv::OpConvertUToPtr: case spv::OpConvertPtrToU: case spv::OpBitcast: case spv::OpUConvert:
                vals[inst.result] = vals[op[0].value]; break;
            case spv::OpSConvert:
                vals[inst.result] = static_cast<uint64_t>(static_cast<int64_t>(getI32(op[0].value))); break;
            case spv::OpStore:
                if (vals[op[0].value] >= kRetOutAddr && vals[op[0].value] < kRetOutAddr + 7 * sizeof(float))
                    out[(vals[op[0].value] - kRetOutAddr) / sizeof(float)] = getF32(op[1].value);
                break;
            case spv::OpFNegate: vals[inst.result] = f32ToBits(-getF32(op[0].value)); break;
            case spv::OpFAdd: vals[inst.result] = f32ToBits(getF32(op[0].value) + getF32(op[1].value)); break;
            case spv::OpFSub: vals[inst.result] = f32ToBits(getF32(op[0].value) - getF32(op[1].value)); break;
            case spv::OpFMul: vals[inst.result] = f32ToBits(getF32(op[0].value) * getF32(op[1].value)); break;
            case spv::OpFDiv: vals[inst.result] = f32ToBits(getF32(op[0].value) / getF32(op[1].value)); break;
            case spv::OpIAdd: vals[inst.result] = vals[op[0].value] + vals[op[1].value]; break;
            case spv::OpISub: vals[inst.result] = vals[op[0].value] - vals[op[1].value]; break;
            case spv::OpIMul: vals[inst.result] = vals[op[0].value] * vals[op[1].value]; break;
            case spv::OpBitwiseAnd: case spv::OpLogicalAnd: vals[inst.result] = getU32(op[0].value) & getU32(op[1].value); break;
            case spv::OpBitwiseOr: case spv::OpLogicalOr: vals[inst.result] = getU32(op[0].value) | getU32(op[1].value); break;
            case spv::OpBitwiseXor: vals[inst.result] = getU32(op[0].value) ^ getU32(op[1].value); break;
            case spv::OpNot: vals[inst.result] = getU32(op[0].value) ^ 0xFFFFFFFFu; break;
            case spv::OpLogicalNot: vals[inst.result] = getU32(op[0].value) == 0 ? 1u : 0u; break;
            case spv::OpIEqual: vals[inst.result] = getU32(op[0].value) == getU32(op[1].value) ? 1u : 0u; break;
            case spv::OpINotEqual: vals[inst.result] = getU32(op[0].value) != getU32(op[1].value) ? 1u : 0u; break;
            case spv::OpSLessThan: vals[inst.result] = getI32(op[0].value) < getI32(op[1].value) ? 1u : 0u; break;
            case spv::OpSLessThanEqual: vals[inst.result] = getI32(op[0].value) <= getI32(op[1].value) ? 1u : 0u; break;
            case spv::OpSGreaterThan: vals[inst.result] = getI32(op[0].value) > getI32(op[1].value) ? 1u : 0u; break;
            case spv::OpSGreaterThanEqual: vals[inst.result] = getI32(op[0].value) >= getI32(op[1].value) ? 1u : 0u; break;
            case spv::OpFOrdEqual: vals[inst.result] = getF32(op[0].value) == getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpFUnordNotEqual: case spv::OpFOrdNotEqual: vals[inst.result] = getF32(op[0].value) != getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpFOrdLessThan: vals[inst.result] = getF32(op[0].value) < getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpFOrdLessThanEqual: vals[inst.result] = getF32(op[0].value) <= getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpFOrdGreaterThan: vals[inst.result] = getF32(op[0].value) > getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpFOrdGreaterThanEqual: vals[inst.result] = getF32(op[0].value) >= getF32(op[1].value) ? 1u : 0u; break;
            case spv::OpSelect: vals[inst.result] = getU32(op[0].value) != 0 ? vals[op[1].value] : vals[op[2].value]; break;
            case spv::OpConvertSToF: vals[inst.result] = f32ToBits(static_cast<float>(getI32(op[0].value))); break;
            case spv::OpConvertFToS: vals[inst.result] = static_cast<uint32_t>(static_cast<int32_t>(getF32(op[0].value))); break;
            case spv::OpExtInst: {
                uint32_t glslOp = op[1].value;
                float a0 = op.size() > 2 ? getF32(op[2].value) : 0.0f;
                float a1 = op.size() > 3 ? getF32(op[3].value) : 0.0f;
                float a2 = op.size() > 4 ? getF32(op[4].value) : 0.0f;
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
                case GLSLstd450Round: case GLSLstd450RoundEven: r = std::nearbyint(a0); break;
                case GLSLstd450Trunc: r = std::trunc(a0); break;
                case GLSLstd450Fract: r = a0 - std::floor(a0); break;
                case GLSLstd450FSign: r = (a0 > 0.0f) ? 1.0f : ((a0 < 0.0f) ? -1.0f : 0.0f); break;
                case GLSLstd450Exp: r = std::exp(a0); break;
                case GLSLstd450Log: r = std::log(a0); break;
                case GLSLstd450Exp2: r = std::exp2(a0); break;
                case GLSLstd450Log2: r = std::log2(a0); break;
                case GLSLstd450FMin: case GLSLstd450NMin: r = std::fmin(a0, a1); break;
                case GLSLstd450FMax: case GLSLstd450NMax: r = std::fmax(a0, a1); break;
                case GLSLstd450Step: r = (a1 < a0) ? 0.0f : 1.0f; break;
                case GLSLstd450FClamp: case GLSLstd450NClamp: r = std::fmin(std::fmax(a0, a1), a2); break;
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
            case spv::OpBranch: prevLabel = bb.label; curBlock = labelToIdx.at(op[0].value); goto next_block;
            case spv::OpBranchConditional: prevLabel = bb.label; curBlock = labelToIdx.at(getU32(op[0].value) != 0 ? op[1].value : op[2].value); goto next_block;
            case spv::OpReturn: goto done_exec;
            default: break;
            }
        }
        break;
    next_block:;
    }
done_exec:
    sample.value = out[0]; sample.dx = out[1]; sample.dy = out[2]; sample.dz = out[3];
    sample.nx = out[4];    sample.ny = out[5]; sample.nz = out[6];
    return sample;
}

GpuAutodiffSample GpuAutodiffResult::evalGradSpirv(float x, float y, float z, float w) const {
    return evalGradSpirv(std::vector<float>{x, y, z, w});
}

GpuAutodiffResult compileJsGpuAutodiffSource(
    const std::string& sourceText,
    const std::string& sourcePath,
    const GpuAutodiffOptions& opts) {
    using namespace gpu_detail;
    GpuAutodiffResult res;
    res.stage = opts.stage;

    SourceBuffer buffer(sourcePath, sourceText);
    DiagnosticSink parseDiags;
    auto tokens = Lexer(buffer, parseDiags).lex();
    if (parseDiags.hasErrors()) {
        res.errorMessage = parseDiags.render(buffer);
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }
    auto astMod = Parser(std::move(tokens), parseDiags).parseModule(sourcePath);
    if (parseDiags.hasErrors() || !astMod) {
        res.errorMessage = parseDiags.render(buffer);
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }

    const ast::FunctionDecl* fnDecl = findTargetKernelDecl(*astMod, sourceText, opts.kernelName);
    if (!fnDecl && opts.allowUnannotated) {
        for (const auto& s : astMod->body) {
            if (const auto* fd = dynamic_cast<const ast::FunctionDecl*>(s.get())) {
                fnDecl = fd;
                break;
            }
        }
    }
    if (!fnDecl) {
        res.errorMessage = sourcePath + ":1:1: error: no @kernel or @shader function found for autodiff\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }
    res.kernelName = fnDecl->name;

    std::vector<std::string> gpuHostGlobals = {
        "clamp", "mix", "smoothstep", "step", "fract", "sign", "tanh",
        "sin", "cos", "tan", "sqrt", "abs", "floor", "ceil", "round", "trunc",
        "exp", "log", "min", "max", "pow", "atan2"
    };
    SourceSet sources;
    sources.add(sourcePath, sourceText);
    DiagnosticSink lowerDiags;
    auto inferred = types::inferModule(*astMod, lowerDiags, &gpuHostGlobals);
    auto ilMod = lower::lowerModule(*astMod, lowerDiags, inferred ? &*inferred : nullptr,
                                    &gpuHostGlobals, &sources, nullptr, /*assumeNoBigInt=*/true);
    if (lowerDiags.hasErrors() || !ilMod) {
        res.errorMessage = lowerDiags.render(sources);
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }

    const il::Function* origFn = nullptr;
    for (const auto& f : ilMod->functions) {
        if (f.name == fnDecl->name) {
            origFn = &f;
            break;
        }
    }
    if (!origFn) {
        res.errorMessage = sourcePath + ":1:1: error: lowered IL function '" + fnDecl->name + "' not found\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }

    il::Function fn = normalizeGpuIlFunction(*origFn);
    const size_t firstParam = fn.firstSourceParam();
    const size_t numParams = fn.params.size() > firstParam ? fn.params.size() - firstParam : 0;
    res.scalarParamCount = static_cast<uint32_t>(numParams);

    bool param1IsZ = false;
    if (numParams == 2) {
        std::string_view p1Name = fn.params[firstParam + 1].name;
        param1IsZ = (p1Name == "z" || p1Name == "wz" || p1Name == "pz" || p1Name == "tz");
        res.isHeightfield2D = true;
    } else if (opts.forceHeightfieldNormal) {
        res.isHeightfield2D = true;
    }

    std::unordered_map<il::ValueId, il::ValueId> aliasOrigin;
    std::unordered_map<il::ValueId, std::string> globalNames;
    std::unordered_map<il::ValueId, double> constF64Map;
    std::unordered_map<il::ValueId, int32_t> constI32Map;

    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ConstF64) {
                constF64Map[inst.result] = inst.immF64;
            } else if (inst.op == il::Op::ConstI32 || inst.op == il::Op::ConstBool) {
                constI32Map[inst.result] = inst.immI32;
            } else if ((inst.op == il::Op::Unbox || inst.op == il::Op::Box ||
                        inst.op == il::Op::ToNumeric) && !inst.operands.empty()) {
                aliasOrigin[inst.result] = inst.operands[0];
            } else if (inst.op == il::Op::GlobalGet || inst.op == il::Op::ResolveName) {
                std::string name = inst.keyIndex < ilMod->keyConstants.size()
                                       ? ilMod->keyConstants[inst.keyIndex]
                                       : "";
                globalNames[inst.result] = name;
            }
        }
    }

    auto originOf = [&](il::ValueId v) {
        for (int depth = 0; depth < 8 && aliasOrigin.count(v); ++depth) v = aliasOrigin[v];
        return v;
    };

    brass::Module mirMod("js_gpu_autodiff_module");
    for (std::string_view sym : {
             "ptx_sin", "ptx_cos", "ptx_ex2", "ptx_lg2", "ptx_rsqrt",
             "ptx_sqrt", "ptx_fabs", "ptx_fmin", "ptx_fmax", "ptx_tanh",
             "ptx_clamp", "ptx_mix", "ptx_smoothstep", "ptx_step",
             "ptx_fract", "ptx_sign"}) {
        mirMod.add_symbol_role(sym, brass::SymbolRole::Pure);
    }

    std::vector<brass::Type> paramTypes(numParams, brass::Type::f32());
    paramTypes.push_back(brass::Type::ptr()); // outPtr -> [value, dx, dy, dz, nx, ny, nz]

    brass::Function* mirFn = mirMod.create_function(fn.name, brass::Type::void_type(), paramTypes);
    brass::Builder b(*mirFn);
    b.set_module(&mirMod);
    brass::codegen::KernelBuilder kb(b);

    std::unordered_map<il::ValueId, DualVal> dualMap;
    std::unordered_map<il::ValueId, brass::Value*> condMap;
    std::vector<brass::BasicBlock*> mirBlocks(fn.blocks.size(), nullptr);
    brass::Value* retOutPtr = nullptr;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        if (bi == 0) {
            mirBlocks[0] = b.append_block("entry");
            kb.position_at_end(mirBlocks[0]);
            brass::Value* zero = kb.const_f32(0.0f);
            brass::Value* one = kb.const_f32(1.0f);
            for (size_t i = 0; i < numParams; ++i) {
                brass::Value* pv = b.add_block_param(mirBlocks[0], brass::Type::f32());
                DualVal dv{pv, {zero, zero, zero}};
                if (i == 0) dv.d[0] = one;
                else if (i == 1) dv.d[(numParams == 2 && param1IsZ) ? 2 : 1] = one;
                else if (i == 2) dv.d[2] = one;
                dualMap[static_cast<il::ValueId>(firstParam + i)] = dv;
            }
            retOutPtr = b.add_block_param(mirBlocks[0], brass::Type::ptr());
        } else {
            mirBlocks[bi] = b.append_block("bb" + std::to_string(bi));
            for (const auto& bp : fn.blocks[bi].params) {
                DualVal dv;
                dv.val = b.add_block_param(mirBlocks[bi], brass::Type::f32());
                dv.d[0] = b.add_block_param(mirBlocks[bi], brass::Type::f32());
                dv.d[1] = b.add_block_param(mirBlocks[bi], brass::Type::f32());
                dv.d[2] = b.add_block_param(mirBlocks[bi], brass::Type::f32());
                dualMap[bp.id] = dv;
            }
        }
    }

    auto makeConstDual = [&](float c) -> DualVal {
        brass::Value* z = kb.const_f32(0.0f);
        return DualVal{kb.const_f32(c), {z, z, z}};
    };

    auto getDual = [&](il::ValueId id) -> DualVal {
        auto it = dualMap.find(id);
        if (it != dualMap.end() && it->second.val != nullptr) return it->second;
        il::ValueId orig = originOf(id);
        if (orig != id) {
            auto it2 = dualMap.find(orig);
            if (it2 != dualMap.end() && it2->second.val != nullptr) return it2->second;
        }
        if (constF64Map.count(orig)) return makeConstDual(static_cast<float>(constF64Map[orig]));
        if (constI32Map.count(orig)) return makeConstDual(static_cast<float>(constI32Map[orig]));
        return makeConstDual(0.0f);
    };

    auto getConstOpt = [&](il::ValueId id, float* outVal) -> bool {
        il::ValueId orig = originOf(id);
        if (constF64Map.count(orig)) { if (outVal) *outVal = static_cast<float>(constF64Map[orig]); return true; }
        if (constI32Map.count(orig)) { if (outVal) *outVal = static_cast<float>(constI32Map[orig]); return true; }
        return false;
    };

    auto safeDenom = [&](brass::Value* v) -> brass::Value* {
        if (v->is_instruction() && v->defining_instruction() &&
            v->defining_instruction()->opcode() == brass::Opcode::fconst_f64 &&
            std::fabs(v->defining_instruction()->imm_f64()) >= 1e-20) {
            return v;
        }
        brass::Value* eps = kb.const_f32(1e-20f);
        return b.build_select(b.build_slt(kb.fabs(v), eps), eps, v);
    };

    bool usesSpirvOnlyIntrinsics = (opts.stage != brass::target::SpirvStage::Compute);
    const size_t activeAxes = (numParams == 0) ? 0 : ((numParams == 1) ? 1 : 3);

    auto emitDiffMath = [&](const std::string& key,
                            const DualVal& u,
                            const DualVal& v,
                            const DualVal& w,
                            il::ValueId vOperandId = il::kNoValue) -> DualVal {
        brass::Value* zero = kb.const_f32(0.0f);
        DualVal out{zero, {zero, zero, zero}};

        if (key == "sin") {
            out.val = kb.sin_approx(u.val);
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(kb.cos_approx(u.val), u.d[k]);
            return out;
        }
        if (key == "cos") {
            out.val = kb.cos_approx(u.val);
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(b.build_neg(kb.sin_approx(u.val)), u.d[k]);
            return out;
        }
        if (key == "tan") {
            brass::Value* s = kb.sin_approx(u.val);
            brass::Value* c = safeDenom(kb.cos_approx(u.val));
            out.val = kb.div(s, c);
            brass::Value* invCosSq = kb.div(kb.const_f32(1.0f), safeDenom(kb.mul(c, c)));
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(invCosSq, u.d[k]);
            return out;
        }
        if (key == "exp") {
            out.val = kb.exp_fast(u.val);
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(out.val, u.d[k]);
            return out;
        }
        if (key == "log") {
            brass::Value* safeU = kb.add(kb.fmax(u.val, zero), kb.const_f32(1e-20f));
            out.val = kb.log_fast(safeU);
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.div(u.d[k], safeU);
            return out;
        }
        if (key == "sqrt") {
            brass::Value* rad = kb.add(kb.fmax(u.val, zero), kb.const_f32(1e-20f));
            out.val = b.build_sqrt(rad);
            brass::Value* twoSqrt = kb.add(kb.mul(kb.const_f32(2.0f), out.val), kb.const_f32(1e-20f));
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.div(u.d[k], twoSqrt);
            return out;
        }
        if (key == "abs") {
            out.val = kb.fabs(u.val);
            brass::Value* sgn = b.build_select(
                b.build_slt(u.val, zero), kb.const_f32(-1.0f),
                b.build_select(b.build_sgt(u.val, zero), kb.const_f32(1.0f), zero));
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(sgn, u.d[k]);
            return out;
        }
        if (key == "min" || key == "max") {
            out.val = (key == "min") ? kb.fmin(u.val, v.val) : kb.fmax(u.val, v.val);
            brass::Value* cond = (key == "min") ? b.build_sle(u.val, v.val) : b.build_sge(u.val, v.val);
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = b.build_select(cond, u.d[k], v.d[k]);
            return out;
        }
        if (key == "pow") {
            float expConst = 0.0f;
            if (vOperandId != il::kNoValue && getConstOpt(vOperandId, &expConst)) {
                if (expConst == 2.0f) {
                    out.val = kb.mul(u.val, u.val);
                    brass::Value* twoU = kb.mul(kb.const_f32(2.0f), u.val);
                    for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(twoU, u.d[k]);
                    return out;
                }
                if (expConst == 3.0f) {
                    brass::Value* u2 = kb.mul(u.val, u.val);
                    out.val = kb.mul(u2, u.val);
                    brass::Value* threeU2 = kb.mul(kb.const_f32(3.0f), u2);
                    for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(threeU2, u.d[k]);
                    return out;
                }
            }
            brass::Value* safeU = kb.add(kb.fabs(u.val), kb.const_f32(1e-20f));
            brass::Value* lnU = kb.log_fast(safeU);
            out.val = kb.exp_fast(kb.mul(v.val, lnU));
            for (size_t k = 0; k < activeAxes; ++k) {
                out.d[k] = kb.mul(out.val, kb.add(kb.mul(v.d[k], lnU), kb.div(kb.mul(v.val, u.d[k]), safeU)));
            }
            return out;
        }
        if (key == "clamp") {
            usesSpirvOnlyIntrinsics = true;
            out.val = kb.clamp_f32(u.val, v.val, w.val);
            brass::Value* below = b.build_slt(u.val, v.val);
            brass::Value* above = b.build_sgt(u.val, w.val);
            for (size_t k = 0; k < activeAxes; ++k) {
                out.d[k] = b.build_select(below, v.d[k], b.build_select(above, w.d[k], u.d[k]));
            }
            return out;
        }
        if (key == "mix") {
            usesSpirvOnlyIntrinsics = true;
            out.val = kb.mix_f32(u.val, v.val, w.val);
            brass::Value* oneMinusA = kb.sub(kb.const_f32(1.0f), w.val);
            brass::Value* yMinusX = kb.sub(v.val, u.val);
            for (size_t k = 0; k < activeAxes; ++k) {
                out.d[k] = kb.add(kb.add(kb.mul(oneMinusA, u.d[k]), kb.mul(w.val, v.d[k])), kb.mul(yMinusX, w.d[k]));
            }
            return out;
        }
        if (key == "smoothstep") {
            usesSpirvOnlyIntrinsics = true;
            out.val = kb.smoothstep_f32(u.val, v.val, w.val);
            brass::Value* span = safeDenom(kb.sub(v.val, u.val));
            brass::Value* spanSq = safeDenom(kb.mul(span, span));
            brass::Value* rawT = kb.div(kb.sub(w.val, u.val), span);
            brass::Value* t = kb.clamp_f32(rawT, zero, kb.const_f32(1.0f));
            brass::Value* inRange = b.build_and(b.build_sgt(rawT, zero), b.build_slt(rawT, kb.const_f32(1.0f)));
            brass::Value* dsDt = kb.mul(kb.const_f32(6.0f), kb.mul(t, kb.sub(kb.const_f32(1.0f), t)));
            for (size_t k = 0; k < activeAxes; ++k) {
                brass::Value* dNum = kb.sub(
                    kb.mul(kb.sub(w.d[k], u.d[k]), span),
                    kb.mul(kb.sub(w.val, u.val), kb.sub(v.d[k], u.d[k])));
                out.d[k] = kb.mul(dsDt, b.build_select(inRange, kb.div(dNum, spanSq), zero));
            }
            return out;
        }
        if (key == "tanh") {
            usesSpirvOnlyIntrinsics = true;
            out.val = kb.tanh_approx(u.val);
            brass::Value* sechSq = kb.sub(kb.const_f32(1.0f), kb.mul(out.val, out.val));
            for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.mul(sechSq, u.d[k]);
            return out;
        }
        if (key == "floor") { out.val = b.build_floor(u.val); return out; }
        if (key == "ceil") { out.val = b.build_ceil(u.val); return out; }
        if (key == "round") { out.val = b.build_round(u.val); return out; }
        if (key == "trunc") { out.val = b.build_sitofp_f32_i32(b.build_fptosi_i32_f32(u.val)); return out; }
        if (key == "step") { usesSpirvOnlyIntrinsics = true; out.val = kb.step_f32(u.val, v.val); return out; }
        if (key == "fract") {
            usesSpirvOnlyIntrinsics = true;
            out.val = b.build_call("ptx_fract", brass::Type::f32(), {u.val});
            out.d = u.d;
            return out;
        }
        if (key == "sign") {
            usesSpirvOnlyIntrinsics = true;
            out.val = b.build_call("ptx_sign", brass::Type::f32(), {u.val});
            return out;
        }
        if (key == "atan2") {
            brass::Value* ay = kb.fabs(u.val);
            brass::Value* ax = kb.fabs(v.val);
            brass::Value* mn = kb.fmin(ax, ay);
            brass::Value* mx = kb.add(kb.fmax(ax, ay), kb.const_f32(1e-20f));
            brass::Value* r = kb.div(mn, mx);
            brass::Value* s = kb.mul(r, r);
            brass::Value* p = kb.fma(s, kb.const_f32(-0.0464964749f), kb.const_f32(0.15931422f));
            p = kb.fma(p, s, kb.const_f32(-0.327622764f));
            p = kb.fma(kb.mul(p, s), r, r);
            p = b.build_select(b.build_sgt(ay, ax), kb.sub(kb.const_f32(1.57079637f), p), p);
            p = b.build_select(b.build_slt(v.val, zero), kb.sub(kb.const_f32(3.14159265f), p), p);
            out.val = b.build_select(b.build_slt(u.val, zero), b.build_neg(p), p);
            brass::Value* rSq = safeDenom(kb.add(kb.mul(v.val, v.val), kb.mul(u.val, u.val)));
            for (size_t k = 0; k < activeAxes; ++k) {
                out.d[k] = kb.div(kb.sub(kb.mul(v.val, u.d[k]), kb.mul(u.val, v.d[k])), rSq);
            }
            return out;
        }
        return u;
    };

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        kb.position_at_end(mirBlocks[bi]);
        brass::Value* zero = kb.const_f32(0.0f);
        for (const auto& inst : fn.blocks[bi].instructions) {
            switch (inst.op) {
            case il::Op::Unbox:
            case il::Op::Box:
            case il::Op::ToNumeric:
                if (!inst.operands.empty()) dualMap[inst.result] = getDual(inst.operands[0]);
                break;
            case il::Op::ToInt32: {
                DualVal u = getDual(inst.operands[0]);
                dualMap[inst.result] = DualVal{b.build_sitofp_f32_i32(b.build_fptosi_i32_f32(u.val)), {zero, zero, zero}};
                break;
            }
            case il::Op::NumericStep: {
                DualVal u = getDual(inst.operands[0]);
                u.val = kb.add(u.val, kb.const_f32(inst.immI32 >= 0 ? 1.0f : -1.0f));
                dualMap[inst.result] = u;
                break;
            }
            case il::Op::Neg: {
                DualVal u = getDual(inst.operands[0]);
                DualVal out{b.build_neg(u.val), {zero, zero, zero}};
                for (size_t k = 0; k < activeAxes; ++k) out.d[k] = b.build_neg(u.d[k]);
                dualMap[inst.result] = out;
                break;
            }
            case il::Op::Add:
            case il::Op::Sub:
            case il::Op::Mul: {
                DualVal u = getDual(inst.operands[0]);
                DualVal v = getDual(inst.operands[1]);
                DualVal out{zero, {zero, zero, zero}};
                if (inst.op == il::Op::Add) {
                    out.val = kb.add(u.val, v.val);
                    for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.add(u.d[k], v.d[k]);
                } else if (inst.op == il::Op::Sub) {
                    out.val = kb.sub(u.val, v.val);
                    for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.sub(u.d[k], v.d[k]);
                } else {
                    out.val = kb.mul(u.val, v.val);
                    for (size_t k = 0; k < activeAxes; ++k) out.d[k] = kb.add(kb.mul(u.d[k], v.val), kb.mul(u.val, v.d[k]));
                }
                dualMap[inst.result] = out;
                break;
            }
            case il::Op::Div: {
                DualVal u = getDual(inst.operands[0]), v = getDual(inst.operands[1]);
                brass::Value* denom = safeDenom(v.val);
                brass::Value* denomSq = safeDenom(kb.mul(denom, denom));
                DualVal out{kb.div(u.val, denom), {zero, zero, zero}};
                for (size_t k = 0; k < activeAxes; ++k)
                    out.d[k] = kb.div(kb.sub(kb.mul(u.d[k], denom), kb.mul(u.val, v.d[k])), denomSq);
                dualMap[inst.result] = out;
                break;
            }
            case il::Op::Pow:
                dualMap[inst.result] = emitDiffMath("pow", getDual(inst.operands[0]), getDual(inst.operands[1]),
                                                    makeConstDual(0.0f), inst.operands[1]);
                break;
            case il::Op::MathUnary: {
                DualVal u = getDual(inst.operands[0]), dummy = makeConstDual(0.0f);
                switch (static_cast<il::MathUnaryFn>(inst.immI32)) {
                case il::MathUnaryFn::Sqrt: dualMap[inst.result] = emitDiffMath("sqrt", u, dummy, dummy); break;
                case il::MathUnaryFn::Abs: dualMap[inst.result] = emitDiffMath("abs", u, dummy, dummy); break;
                case il::MathUnaryFn::Floor: dualMap[inst.result] = emitDiffMath("floor", u, dummy, dummy); break;
                case il::MathUnaryFn::Ceil: dualMap[inst.result] = emitDiffMath("ceil", u, dummy, dummy); break;
                case il::MathUnaryFn::Trunc: dualMap[inst.result] = emitDiffMath("trunc", u, dummy, dummy); break;
                case il::MathUnaryFn::Sin: dualMap[inst.result] = emitDiffMath("sin", u, dummy, dummy); break;
                case il::MathUnaryFn::Cos: dualMap[inst.result] = emitDiffMath("cos", u, dummy, dummy); break;
                }
                break;
            }
            case il::Op::PropGet: {
                std::string key = inst.keyIndex < ilMod->keyConstants.size() ? ilMod->keyConstants[inst.keyIndex] : "";
                float cval = 0.0f;
                dualMap[inst.result] = isAllowedMathConst(key, &cval) ? makeConstDual(cval) : makeConstDual(0.0f);
                break;
            }
            case il::Op::MethodCall:
            case il::Op::DynamicCall: {
                std::string key = (inst.op == il::Op::MethodCall)
                    ? (inst.keyIndex < ilMod->keyConstants.size() ? ilMod->keyConstants[inst.keyIndex] : "")
                    : (!inst.operands.empty() && globalNames.count(originOf(inst.operands[0])) ? globalNames[originOf(inst.operands[0])] : "");
                DualVal a0 = inst.operands.size() >= 2 ? getDual(inst.operands[1]) : makeConstDual(0.0f);
                DualVal a1 = inst.operands.size() >= 3 ? getDual(inst.operands[2]) : makeConstDual(0.0f);
                DualVal a2 = inst.operands.size() >= 4 ? getDual(inst.operands[3]) : makeConstDual(0.0f);
                dualMap[inst.result] = emitDiffMath(key, a0, a1, a2, inst.operands.size() >= 3 ? inst.operands[2] : il::kNoValue);
                break;
            }
            case il::Op::CmpLt: case il::Op::CmpLe: case il::Op::CmpGt: case il::Op::CmpGe:
            case il::Op::CmpEq: case il::Op::CmpNe: case il::Op::RelLt: case il::Op::RelLe:
            case il::Op::RelGt: case il::Op::RelGe: case il::Op::StrictEq: case il::Op::LooseEq: {
                DualVal lhs = getDual(inst.operands[0]), rhs = getDual(inst.operands[1]);
                if (inst.op == il::Op::CmpLt || inst.op == il::Op::RelLt) condMap[inst.result] = b.build_slt(lhs.val, rhs.val);
                else if (inst.op == il::Op::CmpLe || inst.op == il::Op::RelLe) condMap[inst.result] = b.build_sle(lhs.val, rhs.val);
                else if (inst.op == il::Op::CmpGt || inst.op == il::Op::RelGt) condMap[inst.result] = b.build_sgt(lhs.val, rhs.val);
                else if (inst.op == il::Op::CmpGe || inst.op == il::Op::RelGe) condMap[inst.result] = b.build_sge(lhs.val, rhs.val);
                else if (inst.op == il::Op::CmpNe) condMap[inst.result] = b.build_ne(lhs.val, rhs.val);
                else condMap[inst.result] = b.build_eq(lhs.val, rhs.val);
                break;
            }
            case il::Op::NumTruthy:
                condMap[inst.result] = b.build_ne(getDual(inst.operands[0]).val, zero);
                break;
            case il::Op::Jump: {
                std::vector<brass::Value*> args;
                const auto& dstParams = fn.blocks[inst.target.block].params;
                for (size_t k = 0; k < dstParams.size() && k < inst.target.args.size(); ++k) {
                    DualVal dv = getDual(inst.target.args[k]);
                    args.insert(args.end(), {dv.val, dv.d[0], dv.d[1], dv.d[2]});
                }
                b.build_br(mirBlocks[inst.target.block], std::move(args));
                break;
            }
            case il::Op::Branch: {
                brass::Value* cond = condMap.count(inst.operands[0])
                    ? condMap[inst.operands[0]] : b.build_ne(getDual(inst.operands[0]).val, zero);
                auto buildArgs = [&](const il::BlockTarget& t) {
                    std::vector<brass::Value*> args;
                    const auto& dstParams = fn.blocks[t.block].params;
                    for (size_t k = 0; k < dstParams.size() && k < t.args.size(); ++k) {
                        DualVal dv = getDual(t.args[k]);
                        args.insert(args.end(), {dv.val, dv.d[0], dv.d[1], dv.d[2]});
                    }
                    return args;
                };
                b.build_br_if(cond, mirBlocks[inst.target.block], buildArgs(inst.target),
                              mirBlocks[inst.elseTarget.block], buildArgs(inst.elseTarget));
                break;
            }
            case il::Op::Ret: {
                if (retOutPtr && !inst.operands.empty()) {
                    DualVal rv = getDual(inst.operands[0]);
                    brass::Value *dx = rv.d[0], *dy = rv.d[1], *dz = rv.d[2];
                    brass::Value *nx = zero, *ny = kb.const_f32(1.0f), *nz = zero;
                    if (res.isHeightfield2D) {
                        brass::Value *du0 = dx, *du1 = param1IsZ ? dz : dy;
                        brass::Value* lenSq = kb.add(kb.const_f32(1.0f), kb.add(kb.mul(du0, du0), kb.mul(du1, du1)));
                        brass::Value* safeLen = b.build_sqrt(kb.add(kb.fmax(lenSq, kb.const_f32(1.0f)), kb.const_f32(1e-20f)));
                        nx = kb.div(b.build_neg(du0), safeLen);
                        ny = kb.div(kb.const_f32(1.0f), safeLen);
                        nz = kb.div(b.build_neg(du1), safeLen);
                    } else {
                        brass::Value* lenSq = kb.add(kb.mul(dx, dx), kb.add(kb.mul(dy, dy), kb.mul(dz, dz)));
                        brass::Value* hasLen = b.build_sgt(lenSq, kb.const_f32(1e-20f));
                        brass::Value* safeLen = b.build_sqrt(kb.add(kb.fmax(lenSq, zero), kb.const_f32(1e-20f)));
                        nx = b.build_select(hasLen, kb.div(dx, safeLen), zero);
                        ny = b.build_select(hasLen, kb.div(dy, safeLen), kb.const_f32(1.0f));
                        nz = b.build_select(hasLen, kb.div(dz, safeLen), zero);
                    }
                    kb.store_f32(retOutPtr, rv.val, 0);
                    kb.store_f32(retOutPtr, dx, 4);
                    kb.store_f32(retOutPtr, dy, 8);
                    kb.store_f32(retOutPtr, dz, 12);
                    kb.store_f32(retOutPtr, nx, 16);
                    kb.store_f32(retOutPtr, ny, 20);
                    kb.store_f32(retOutPtr, nz, 24);
                }
                b.build_ret_void();
                break;
            }
            default: break;
            }
        }
    }

    countTrigAndTotalInsts(*mirFn, &res.sinCallCountBeforeGvn, &res.cosCallCountBeforeGvn, &res.totalInstCountBeforeOpt);

    if (opts.runOptimizations) {
        brass::GvnStats gvnStats;
        brass::GvnOptions gvnOpts;
        gvnOpts.stats = &gvnStats;

        brass::gvn_function(*mirFn, gvnOpts);
        removeDeadPureCalls(*mirFn);
        brass::scalar_cleanup(*mirFn);
        brass::run_smt_guard_elimination(*mirFn, mirMod);
        brass::run_mir_superopt(*mirFn, mirMod);
        brass::run_gpu_mir_superopt(*mirFn, mirMod);
        brass::gvn_function(*mirFn, gvnOpts);
        removeDeadPureCalls(*mirFn);
        brass::eliminate_dead_code(*mirFn);
        res.gvnExpressionsEliminated = gvnStats.expressions_eliminated;
    }

    countTrigAndTotalInsts(*mirFn, &res.sinCallCountAfterGvn, &res.cosCallCountAfterGvn, &res.totalInstCountAfterOpt);

    try {
        brass::target::SpirvOptions spvOpts;
        spvOpts.spirv_version = opts.spirvVersion;
        spvOpts.stage = opts.stage;
        spvOpts.local_size_x = opts.localSizeX;
        spvOpts.local_size_y = opts.localSizeY;
        spvOpts.local_size_z = opts.localSizeZ;

        brass::spirv::Module spvMod(spvOpts.spirv_version);
        brass::spirv::SpirvISel spvIsel(spvMod, spvOpts);
        spvIsel.lower(*mirFn);

        if (opts.enableRelaxedPrecision) {
            brass::spirv::SpirvRelaxedPrecisionOptions rpOpts;
            rpOpts.max_abs_fp16 = opts.maxAbsFp16;
            res.relaxedPrecisionReport = brass::spirv::promote_relaxed_precision(spvMod, rpOpts);
        }

        res.spirvDiagnostics = brass::spirv::verify(spvMod);
        if (!res.spirvDiagnostics.empty()) {
            res.errorMessage = "SPIR-V verification failed:\n" + brass::spirv::format_diagnostics(res.spirvDiagnostics);
            res.diagnostics.push_back(res.errorMessage);
            return res;
        }
        res.spirvWords = brass::spirv::write(spvMod);
        res.spirvDisassembly = brass::spirv::dump(spvMod);

        if (!usesSpirvOnlyIntrinsics) {
            brass::target::PtxOptions ptxOpts;
            ptxOpts.sm_arch = "sm_" + std::to_string(opts.ptxSmVersion);
            brass::ptx::PtxISel ptxIsel;
            brass::ptx::Function ptxFn = ptxIsel.lower(*mirFn);
            if (ptxOpts.cleanup) brass::ptx::cleanup(ptxFn);
            res.ptxDiagnostics = brass::ptx::verify(ptxFn);
            if (res.ptxDiagnostics.empty()) res.ptxAssembly = brass::ptx::print(ptxFn, ptxOpts);
        }

        if (opts.verifyEquivalence) {
            std::string equivErr;
            res.spirvEquivalence = brass::verify::verify_spirv_equivalence(*mirFn, spvMod, &equivErr);
            res.equivalence = res.spirvEquivalence;
            res.cpuGpuProvenEquivalent = res.spirvEquivalence.is_equivalent();
            if (!res.spirvEquivalence.is_equivalent()) {
                res.errorMessage = "Autodiff MIR/SPIR-V equivalence check failed: " + equivErr;
                res.diagnostics.push_back(res.errorMessage);
                return res;
            }
        }

        if (opts.verifySafety) {
            brass::verify::GpuProverOptions proverOpts;
            proverOpts.local_size_x = opts.localSizeX;
            proverOpts.local_size_y = opts.localSizeY;
            proverOpts.local_size_z = opts.localSizeZ;
            res.memorySafety = brass::verify::verify_gpu_memory_safety(*mirFn, proverOpts);
            res.barrierUniformity = brass::verify::verify_gpu_barrier_uniformity(*mirFn, proverOpts);
            res.shaderSafetyReport = prove_spirv_shader_safety(spvMod);
            bool allDivsSafe = true;
            for (const auto& d : res.shaderSafetyReport.divisions) {
                if (!d.proven_safe) { allDivsSafe = false; break; }
            }
            res.provenSafe = res.memorySafety.is_safe() &&
                             res.barrierUniformity.is_uniform() &&
                             res.shaderSafetyReport.ok() &&
                             allDivsSafe;
            if (!res.provenSafe) {
                std::string details;
                for (const auto& d : res.shaderSafetyReport.divisions) {
                    if (!d.proven_safe) details += " [div op=" + std::to_string(d.op) + " ext=" + std::to_string(d.ext_op) + " reason=" + d.guard_reason + "]";
                }
                res.errorMessage = "Autodiff GPU shader safety verification failed: mem=" +
                                   std::to_string(res.memorySafety.is_safe()) + " (" + res.memorySafety.diagnostic + ") bar=" +
                                   std::to_string(res.barrierUniformity.is_uniform()) + " rep=" +
                                   std::to_string(res.shaderSafetyReport.ok()) + " divs=" + std::to_string(allDivsSafe) + details;
                res.diagnostics.push_back(res.errorMessage);
                return res;
            }
        }

        if (opts.compileCpu && res.scalarParamCount <= 4) {
            brass::codegen::KernelOptions kopts;
            kopts.enable_optimizations = false;
            kopts.enable_parallel = false;
            brass::codegen::KernelJit jit(kopts);
            res.cpuKernel = jit.compile(*mirFn);

            if (opts.verifySingleSourceParity) {
                float maxUlp = 0.0f;
                uint32_t samples = 0;
                for (float x : {-4.0f, -1.25f, 0.0f, 1.5f, 3.25f}) {
                    for (float y : {-3.0f, 0.0f, 2.25f}) {
                        std::vector<float> pt = {x, y, 0.75f, 1.0f};
                        GpuAutodiffSample c = res.evalGradCpu(pt);
                        GpuAutodiffSample g = res.evalGradSpirv(pt);
                        maxUlp = std::max(maxUlp, ulpDiffF32(c.value, g.value));
                        maxUlp = std::max(maxUlp, ulpDiffF32(c.dx, g.dx));
                        maxUlp = std::max(maxUlp, ulpDiffF32(c.dy, g.dy));
                        maxUlp = std::max(maxUlp, ulpDiffF32(c.dz, g.dz));
                        ++samples;
                    }
                }
                res.paritySampleCount = samples;
                res.maxCpuGpuUlpDiff = maxUlp;
                res.singleSourceParityVerified = (maxUlp <= 2.0f);
            }
        }
    } catch (const std::exception& ex) {
        res.errorMessage = std::string("error: GPU autodiff codegen failed: ") + ex.what() + "\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }

    res.ok = true;
    return res;
}

}  // namespace bronze
