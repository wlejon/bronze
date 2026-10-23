#include "codegen-brass/js_gpu_kernel.h"
#include "codegen-brass/js_gpu_subset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/gpu_superopt.hpp>
#include <brass/mir/scalar_opt.hpp>
#include <brass/mir/smt_guard_elim.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/target/ptx/ptx_cleanup.hpp>
#include <brass/target/ptx/ptx_isel.hpp>
#include <brass/target/ptx/ptx_printer.hpp>
#include <brass/target/ptx/ptx_verifier.hpp>
#include <brass/target/spirv/spirv_ir.hpp>
#include <brass/target/spirv/spirv_isel.hpp>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "parse/parser.h"
#include "types/infer.h"

namespace bronze {

GpuCompileResult compileJsGpuKernelSource(
    const std::string& sourceText,
    const std::string& sourcePath,
    const GpuCompileOptions& opts) {
    using namespace gpu_detail;
    GpuCompileResult res;

    SourceBuffer buffer(sourcePath, sourceText);
    LineTable lt(buffer.text());
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
    if (!fnDecl) {
        res.errorMessage = sourcePath + ":1:1: error: @kernel function not found\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }
    res.kernelName = fnDecl->name;

    // 1. AST-level GPU subset check with exact line:col diagnostics
    AstSubsetChecker astChecker{sourcePath, lt, fnDecl->name, {}, {}, res.diagnostics};
    if (fnDecl->isGenerator || fnDecl->isAsync) {
        astChecker.error(fnDecl->span, "async/generator functions are not allowed in @kernel functions");
    }
    for (const auto& p : fnDecl->params) {
        if (p.pattern || p.isRest) {
            astChecker.error(p.span, "destructuring/rest parameters are not allowed in @kernel functions");
        } else {
            astChecker.paramNames.insert(p.name);
        }
    }
    for (const auto& s : fnDecl->body) astChecker.checkStmt(s.get());
    if (!res.diagnostics.empty()) {
        std::ostringstream oss;
        for (const auto& d : res.diagnostics) oss << d << "\n";
        res.errorMessage = oss.str();
        return res;
    }

    // 2. Lower to Bronze IL
    std::vector<std::string> gpuHostGlobals = {
        "tid_x", "tid_y", "tid_z", "ctaid_x", "ctaid_y", "ctaid_z",
        "ntid_x", "ntid_y", "ntid_z", "nctaid_x", "nctaid_y", "nctaid_z",
        "global_tid_x", "lane_id", "warp_id", "sync", "bar_sync"
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

    // 3. Classify ValueIds: pointers, floats, ints
    std::unordered_map<il::ValueId, il::ValueId> unboxOrigin;
    std::unordered_map<il::ValueId, std::string> globalNames;
    std::unordered_map<il::ValueId, double> constF64Map;
    std::unordered_map<il::ValueId, int32_t> constI32Map;

    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if ((inst.op == il::Op::Unbox || inst.op == il::Op::Box) && !inst.operands.empty()) {
                il::ValueId src = inst.operands[0];
                while (unboxOrigin.count(src)) src = unboxOrigin[src];
                unboxOrigin[inst.result] = src;
            } else if ((inst.op == il::Op::GlobalGet || inst.op == il::Op::ResolveName) &&
                       inst.keyIndex < ilMod->keyConstants.size()) {
                globalNames[inst.result] = ilMod->keyConstants[inst.keyIndex];
            } else if (inst.op == il::Op::ConstF64) {
                constF64Map[inst.result] = inst.immF64;
            } else if (inst.op == il::Op::ConstI32 || inst.op == il::Op::ConstBool) {
                constI32Map[inst.result] = inst.immI32;
            }
        }
    }
    auto originOf = [&](il::ValueId v) {
        while (unboxOrigin.count(v)) v = unboxOrigin[v];
        return v;
    };

    std::vector<bool> isBufferParam(numParams, false);
    std::vector<brass::Type> bufferElemType(numParams, brass::Type::f32());
    for (size_t i = 0; i < numParams && i < fnDecl->params.size(); ++i) {
        const std::string& ann = fnDecl->params[i].typeAnnotation;
        if (ann.find("Int32Array") != std::string::npos || ann.find("Uint32Array") != std::string::npos) {
            isBufferParam[i] = true;
            bufferElemType[i] = brass::Type::i32();
        } else if (ann.find("Float32Array") != std::string::npos || ann.find("Float64Array") != std::string::npos) {
            isBufferParam[i] = true;
            bufferElemType[i] = brass::Type::f32();
        }
    }

    std::unordered_set<il::ValueId> usedAsIndexOrInt;
    std::unordered_set<il::ValueId> mustBeFloat;

    for (const auto& blk : fn.blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet ||
                inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) {
                if (!inst.operands.empty()) {
                    il::ValueId base = originOf(inst.operands[0]);
                    if (base >= firstParam && base < firstParam + numParams) {
                        isBufferParam[base - firstParam] = true;
                    }
                }
                if (inst.operands.size() >= 2) usedAsIndexOrInt.insert(originOf(inst.operands[1]));
                if (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemGetTyped) {
                    il::ValueId base = !inst.operands.empty() ? originOf(inst.operands[0]) : il::kNoValue;
                    bool isIntBuf = (base >= firstParam && base < firstParam + numParams &&
                                     bufferElemType[base - firstParam].is_i32());
                    if (!isIntBuf) mustBeFloat.insert(inst.result);
                }
            } else if (inst.op == il::Op::ConstF64) {
                double d = inst.immF64;
                if (std::floor(d) != d || d < -2147483648.0 || d > 2147483647.0) {
                    mustBeFloat.insert(inst.result);
                }
            } else if (inst.op == il::Op::Div || inst.op == il::Op::Pow || inst.op == il::Op::MathUnary) {
                mustBeFloat.insert(inst.result);
            } else if (inst.op == il::Op::MethodCall && inst.keyIndex < ilMod->keyConstants.size()) {
                if (ilMod->keyConstants[inst.keyIndex] != "imul") mustBeFloat.insert(inst.result);
                else usedAsIndexOrInt.insert(inst.result);
            } else if (inst.op == il::Op::PropGet && inst.keyIndex < ilMod->keyConstants.size()) {
                const std::string& key = ilMod->keyConstants[inst.keyIndex];
                if (isAllowedMathConst(key)) mustBeFloat.insert(inst.result);
                else if (key == "length" || key == "byteLength") usedAsIndexOrInt.insert(inst.result);
            } else if (inst.op == il::Op::ToInt32 || inst.op == il::Op::BitAnd ||
                       inst.op == il::Op::BitOr || inst.op == il::Op::BitXor ||
                       inst.op == il::Op::Shl || inst.op == il::Op::Shr ||
                       inst.op == il::Op::UShr || inst.op == il::Op::MathImul) {
                usedAsIndexOrInt.insert(inst.result);
                for (il::ValueId op : inst.operands) usedAsIndexOrInt.insert(originOf(op));
            } else if (inst.op == il::Op::DynamicCall && !inst.operands.empty()) {
                usedAsIndexOrInt.insert(inst.result);
            }
        }
    }

    bool propChanged = true;
    while (propChanged) {
        propChanged = false;
        auto markFloat = [&](il::ValueId v) {
            if (v != il::kNoValue && mustBeFloat.insert(v).second) propChanged = true;
        };
        auto markInt = [&](il::ValueId v) {
            if (v != il::kNoValue && !mustBeFloat.count(v) && usedAsIndexOrInt.insert(v).second) {
                propChanged = true;
            }
        };
        for (const auto& blk : fn.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::Unbox || inst.op == il::Op::Box || inst.op == il::Op::Neg) {
                    if (!inst.operands.empty() && mustBeFloat.count(inst.operands[0])) markFloat(inst.result);
                } else if (inst.op == il::Op::Add || inst.op == il::Op::Sub ||
                           inst.op == il::Op::Mul || inst.op == il::Op::Mod) {
                    bool anyFl = false;
                    for (il::ValueId op : inst.operands) {
                        if (mustBeFloat.count(op) || mustBeFloat.count(originOf(op))) anyFl = true;
                    }
                    if (anyFl) {
                        markFloat(inst.result);
                        for (il::ValueId op : inst.operands) {
                            il::ValueId orig = originOf(op);
                            if (orig >= firstParam && orig < firstParam + numParams &&
                                !isBufferParam[orig - firstParam] && !usedAsIndexOrInt.count(orig)) {
                                markFloat(orig);
                            }
                        }
                    } else if (usedAsIndexOrInt.count(inst.result)) {
                        for (il::ValueId op : inst.operands) markInt(originOf(op));
                    }
                } else if (inst.op == il::Op::CmpLt || inst.op == il::Op::CmpLe ||
                           inst.op == il::Op::CmpGt || inst.op == il::Op::CmpGe ||
                           inst.op == il::Op::RelLt || inst.op == il::Op::RelLe ||
                           inst.op == il::Op::RelGt || inst.op == il::Op::RelGe) {
                    if (inst.operands.size() == 2) {
                        il::ValueId a = originOf(inst.operands[0]);
                        il::ValueId b = originOf(inst.operands[1]);
                        if (usedAsIndexOrInt.count(a) && !mustBeFloat.count(b)) markInt(b);
                        if (usedAsIndexOrInt.count(b) && !mustBeFloat.count(a)) markInt(a);
                    }
                } else if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                    auto propEdge = [&](const il::BlockTarget& t) {
                        if (t.block >= fn.blocks.size()) return;
                        const auto& dstParams = fn.blocks[t.block].params;
                        for (size_t k = 0; k < dstParams.size() && k < t.args.size(); ++k) {
                            il::ValueId pid = dstParams[k].id;
                            il::ValueId aid = originOf(t.args[k]);
                            if (mustBeFloat.count(aid) || mustBeFloat.count(t.args[k])) markFloat(pid);
                            if (mustBeFloat.count(pid)) markFloat(aid);
                            if (usedAsIndexOrInt.count(pid)) markInt(aid);
                            if (usedAsIndexOrInt.count(aid)) markInt(pid);
                        }
                    };
                    propEdge(inst.target);
                    if (inst.op == il::Op::Branch) propEdge(inst.elseTarget);
                }
            }
        }
    }

    auto isIntParamName = [](std::string_view n) {
        return n == "n" || n == "N" || n == "count" || n == "len" || n == "length" ||
               n == "width" || n == "height" || n == "depth" || n == "stride" ||
               n == "maxIter" || n == "iters" || n == "idx" || n == "i" || n == "j" || n == "k" ||
               n == "px" || n == "py";
    };

    // 4. Build Brass MIR Function via KernelBuilder
    brass::Module mirMod("js_gpu_module");
    std::vector<brass::Type> paramTypes;
    for (size_t i = 0; i < numParams; ++i) {
        il::ValueId pid = static_cast<il::ValueId>(firstParam + i);
        if (isBufferParam[i]) {
            paramTypes.push_back(brass::Type::ptr());
        } else if (!mustBeFloat.count(pid) &&
                   (usedAsIndexOrInt.count(pid) || isIntParamName(fn.params[pid].name))) {
            paramTypes.push_back(brass::Type::i32());
        } else {
            paramTypes.push_back(brass::Type::f32());
        }
    }

    brass::Function* mirFn = mirMod.create_function(fn.name, brass::Type::void_type(), paramTypes);
    brass::Builder b(*mirFn);
    b.set_module(&mirMod);
    brass::codegen::KernelBuilder kb(b);

    std::unordered_map<il::ValueId, brass::Value*> valMap;
    std::vector<brass::BasicBlock*> mirBlocks(fn.blocks.size(), nullptr);
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        if (bi == 0) {
            mirBlocks[0] = b.append_block("entry");
            for (size_t i = 0; i < numParams; ++i) {
                valMap[static_cast<il::ValueId>(firstParam + i)] =
                    b.add_block_param(mirBlocks[0], paramTypes[i]);
            }
        } else {
            mirBlocks[bi] = b.append_block("bb" + std::to_string(bi));
            for (const auto& bp : fn.blocks[bi].params) {
                brass::Type pt = mustBeFloat.count(bp.id) ? brass::Type::f32() : brass::Type::i32();
                valMap[bp.id] = b.add_block_param(mirBlocks[bi], pt);
            }
        }
    }

    auto getVal = [&](il::ValueId id, brass::Type preferredScalar = brass::Type::f32()) -> brass::Value* {
        auto it = valMap.find(id);
        if (it != valMap.end() && it->second != nullptr) return it->second;
        il::ValueId orig = originOf(id);
        if (orig != id) {
            auto it2 = valMap.find(orig);
            if (it2 != valMap.end() && it2->second != nullptr) return it2->second;
        }
        auto cf = constF64Map.find(orig);
        if (cf != constF64Map.end()) {
            double d = cf->second;
            if (preferredScalar.is_i32() && !mustBeFloat.count(orig) &&
                std::floor(d) == d && d >= -2147483648.0 && d <= 2147483647.0) {
                return kb.const_i32(static_cast<int32_t>(d));
            }
            return kb.const_f32(static_cast<float>(d));
        }
        auto ci = constI32Map.find(orig);
        if (ci != constI32Map.end()) {
            if (preferredScalar.is_float() && mustBeFloat.count(orig)) {
                return kb.const_f32(static_cast<float>(ci->second));
            }
            return kb.const_i32(ci->second);
        }
        return preferredScalar.is_i32() ? kb.const_i32(0) : kb.const_f32(0.0f);
    };

    auto asF32 = [&](brass::Value* v) -> brass::Value* {
        if (!v) return kb.const_f32(0.0f);
        if (v->type().kind() == brass::TypeKind::F32) return v;
        if (v->type().is_i32()) {
            if (v->is_instruction() && v->defining_instruction() &&
                v->defining_instruction()->opcode() == brass::Opcode::iconst_i32) {
                return kb.const_f32(static_cast<float>(v->defining_instruction()->imm_i32()));
            }
            return b.build_sitofp_f32_i32(v);
        }
        return kb.const_f32(0.0f);
    };

    auto asI32 = [&](brass::Value* v) -> brass::Value* {
        if (!v) return kb.const_i32(0);
        if (v->type().is_i32()) return v;
        if (v->type().kind() == brass::TypeKind::F32) {
            if (v->is_instruction() && v->defining_instruction() &&
                v->defining_instruction()->opcode() == brass::Opcode::fconst_f64) {
                return kb.const_i32(static_cast<int32_t>(v->defining_instruction()->imm_f64()));
            }
            return b.build_fptosi_i32_f32(v);
        }
        return kb.const_i32(0);
    };

    auto safeIndex = [&](brass::Value* idxVal) -> brass::Value* {
        brass::Value* i32Idx = asI32(idxVal);
        if (i32Idx->is_instruction() && i32Idx->defining_instruction() &&
            i32Idx->defining_instruction()->opcode() == brass::Opcode::iconst_i32 &&
            i32Idx->defining_instruction()->imm_i32() >= 0) {
            return i32Idx;
        }
        return b.build_and(i32Idx, kb.const_i32(0x3fffffff));
    };

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        kb.position_at_end(mirBlocks[bi]);
        for (const auto& inst : fn.blocks[bi].instructions) {
            switch (inst.op) {
            case il::Op::ConstF64:
            case il::Op::ConstI32:
            case il::Op::ConstBool:
            case il::Op::ConstUndefined:
            case il::Op::ConstNull:
            case il::Op::IsNumber:
            case il::Op::IsDenseArray:
            case il::Op::PinGuard:
            case il::Op::CensusRecord:
            case il::Op::KeepAlive:
                break;

            case il::Op::Unbox:
            case il::Op::Box:
            case il::Op::ToNumeric:
                if (!inst.operands.empty()) {
                    brass::Type pref = mustBeFloat.count(inst.result) ? brass::Type::f32() : brass::Type::i32();
                    valMap[inst.result] = getVal(inst.operands[0], pref);
                }
                break;

            case il::Op::ToInt32:
                valMap[inst.result] = asI32(getVal(inst.operands[0], brass::Type::i32()));
                break;

            case il::Op::NumericStep: {
                bool isFl = mustBeFloat.count(inst.result) || mustBeFloat.count(inst.operands[0]);
                if (isFl) {
                    valMap[inst.result] = kb.add(asF32(getVal(inst.operands[0], brass::Type::f32())),
                                                 kb.const_f32(inst.immI32 >= 0 ? 1.0f : -1.0f));
                } else {
                    valMap[inst.result] = kb.add(asI32(getVal(inst.operands[0], brass::Type::i32())),
                                                 kb.const_i32(inst.immI32 >= 0 ? 1 : -1));
                }
                break;
            }

            case il::Op::Neg: {
                bool isFl = mustBeFloat.count(inst.result) || mustBeFloat.count(inst.operands[0]);
                valMap[inst.result] = b.build_neg(isFl ? asF32(getVal(inst.operands[0], brass::Type::f32()))
                                                       : asI32(getVal(inst.operands[0], brass::Type::i32())));
                break;
            }

            case il::Op::Add:
            case il::Op::Sub:
            case il::Op::Mul: {
                bool isFl = mustBeFloat.count(inst.result);
                brass::Type pref = isFl ? brass::Type::f32() : brass::Type::i32();
                brass::Value* lhs = getVal(inst.operands[0], pref);
                brass::Value* rhs = getVal(inst.operands[1], pref);
                if (isFl || lhs->type().is_float() || rhs->type().is_float()) {
                    lhs = asF32(lhs);
                    rhs = asF32(rhs);
                } else {
                    lhs = asI32(lhs);
                    rhs = asI32(rhs);
                }
                if (inst.op == il::Op::Add) valMap[inst.result] = kb.add(lhs, rhs);
                else if (inst.op == il::Op::Sub) valMap[inst.result] = kb.sub(lhs, rhs);
                else valMap[inst.result] = kb.mul(lhs, rhs);
                break;
            }

            case il::Op::Div:
                valMap[inst.result] = kb.div(asF32(getVal(inst.operands[0], brass::Type::f32())),
                                             asF32(getVal(inst.operands[1], brass::Type::f32())));
                break;

            case il::Op::Mod: {
                if (mustBeFloat.count(inst.result)) {
                    brass::Value* lhs = asF32(getVal(inst.operands[0], brass::Type::f32()));
                    brass::Value* rhs = asF32(getVal(inst.operands[1], brass::Type::f32()));
                    brass::Value* q = b.build_sitofp_f32_i32(b.build_fptosi_i32_f32(kb.div(lhs, rhs)));
                    valMap[inst.result] = kb.sub(lhs, kb.mul(q, rhs));
                } else {
                    brass::Value* lhs = asI32(getVal(inst.operands[0], brass::Type::i32()));
                    brass::Value* rhs = asI32(getVal(inst.operands[1], brass::Type::i32()));
                    brass::Value* nz = b.build_select(b.build_eq(rhs, kb.const_i32(0)), kb.const_i32(1), rhs);
                    valMap[inst.result] = b.build_smod(lhs, nz);
                }
                break;
            }

            case il::Op::Pow: {
                brass::Value* lhs = asF32(getVal(inst.operands[0], brass::Type::f32()));
                brass::Value* rhs = asF32(getVal(inst.operands[1], brass::Type::f32()));
                valMap[inst.result] = kb.exp_fast(kb.mul(rhs, kb.log_fast(lhs)));
                break;
            }

            case il::Op::BitAnd:
            case il::Op::BitOr:
            case il::Op::BitXor:
            case il::Op::Shl:
            case il::Op::Shr:
            case il::Op::UShr:
            case il::Op::MathImul: {
                brass::Value* lhs = asI32(getVal(inst.operands[0], brass::Type::i32()));
                brass::Value* rhs = asI32(getVal(inst.operands[1], brass::Type::i32()));
                if (inst.op == il::Op::BitAnd) valMap[inst.result] = b.build_and(lhs, rhs);
                else if (inst.op == il::Op::BitOr) valMap[inst.result] = b.build_or(lhs, rhs);
                else if (inst.op == il::Op::BitXor) valMap[inst.result] = b.build_xor(lhs, rhs);
                else if (inst.op == il::Op::Shl) valMap[inst.result] = b.build_shl(lhs, b.build_and(rhs, kb.const_i32(31)));
                else if (inst.op == il::Op::Shr) valMap[inst.result] = b.build_ashr(lhs, b.build_and(rhs, kb.const_i32(31)));
                else if (inst.op == il::Op::UShr) valMap[inst.result] = b.build_lshr(lhs, b.build_and(rhs, kb.const_i32(31)));
                else valMap[inst.result] = kb.mul(lhs, rhs);
                break;
            }

            case il::Op::BitNot:
                valMap[inst.result] = b.build_not(asI32(getVal(inst.operands[0], brass::Type::i32())));
                break;

            case il::Op::CmpLt:
            case il::Op::CmpLe:
            case il::Op::CmpGt:
            case il::Op::CmpGe:
            case il::Op::CmpEq:
            case il::Op::CmpNe:
            case il::Op::RelLt:
            case il::Op::RelLe:
            case il::Op::RelGt:
            case il::Op::RelGe:
            case il::Op::StrictEq:
            case il::Op::LooseEq: {
                il::ValueId aId = originOf(inst.operands[0]);
                il::ValueId bId = originOf(inst.operands[1]);
                bool useFl = mustBeFloat.count(aId) || mustBeFloat.count(bId) ||
                             mustBeFloat.count(inst.operands[0]) || mustBeFloat.count(inst.operands[1]);
                brass::Type pref = useFl ? brass::Type::f32() : brass::Type::i32();
                brass::Value* lhs = getVal(inst.operands[0], pref);
                brass::Value* rhs = getVal(inst.operands[1], pref);
                if (useFl || lhs->type().is_float() || rhs->type().is_float()) {
                    lhs = asF32(lhs);
                    rhs = asF32(rhs);
                } else {
                    lhs = asI32(lhs);
                    rhs = asI32(rhs);
                }
                if (inst.op == il::Op::CmpLt || inst.op == il::Op::RelLt) valMap[inst.result] = b.build_slt(lhs, rhs);
                else if (inst.op == il::Op::CmpLe || inst.op == il::Op::RelLe) valMap[inst.result] = b.build_sle(lhs, rhs);
                else if (inst.op == il::Op::CmpGt || inst.op == il::Op::RelGt) valMap[inst.result] = b.build_sgt(lhs, rhs);
                else if (inst.op == il::Op::CmpGe || inst.op == il::Op::RelGe) valMap[inst.result] = b.build_sge(lhs, rhs);
                else if (inst.op == il::Op::CmpNe) valMap[inst.result] = b.build_ne(lhs, rhs);
                else valMap[inst.result] = b.build_eq(lhs, rhs);
                break;
            }

            case il::Op::NumTruthy: {
                brass::Value* v = getVal(inst.operands[0], brass::Type::f32());
                valMap[inst.result] = v->type().is_float()
                                          ? b.build_ne(v, kb.const_f32(0.0f))
                                          : b.build_ne(asI32(v), kb.const_i32(0));
                break;
            }

            case il::Op::ElemGet:
            case il::Op::ElemGetTyped: {
                il::ValueId baseId = originOf(inst.operands[0]);
                brass::Value* ptr = getVal(baseId, brass::Type::ptr());
                brass::Value* idx = safeIndex(getVal(inst.operands[1], brass::Type::i32()));
                bool isIntBuf = (baseId >= firstParam && baseId < firstParam + numParams &&
                                 bufferElemType[baseId - firstParam].is_i32());
                valMap[inst.result] = isIntBuf ? kb.load_i32_indexed(ptr, idx)
                                               : kb.load_f32_indexed(ptr, idx);
                break;
            }

            case il::Op::ElemSet:
            case il::Op::ElemSetTyped: {
                il::ValueId baseId = originOf(inst.operands[0]);
                brass::Value* ptr = getVal(baseId, brass::Type::ptr());
                brass::Value* idx = safeIndex(getVal(inst.operands[1], brass::Type::i32()));
                bool isIntBuf = (baseId >= firstParam && baseId < firstParam + numParams &&
                                 bufferElemType[baseId - firstParam].is_i32());
                if (isIntBuf) {
                    kb.store_i32_indexed(ptr, idx, asI32(getVal(inst.operands[2], brass::Type::i32())));
                } else {
                    kb.store_f32_indexed(ptr, idx, asF32(getVal(inst.operands[2], brass::Type::f32())));
                }
                break;
            }

            case il::Op::MathUnary: {
                brass::Value* x = asF32(getVal(inst.operands[0], brass::Type::f32()));
                switch (static_cast<il::MathUnaryFn>(inst.immI32)) {
                case il::MathUnaryFn::Sqrt: valMap[inst.result] = b.build_sqrt(x); break;
                case il::MathUnaryFn::Abs: valMap[inst.result] = kb.fabs(x); break;
                case il::MathUnaryFn::Floor: valMap[inst.result] = b.build_floor(x); break;
                case il::MathUnaryFn::Ceil: valMap[inst.result] = b.build_ceil(x); break;
                case il::MathUnaryFn::Trunc:
                    valMap[inst.result] = b.build_sitofp_f32_i32(b.build_fptosi_i32_f32(x));
                    break;
                case il::MathUnaryFn::Sin:
                    valMap[inst.result] = b.build_call("ptx_sin", brass::Type::f32(), {x});
                    break;
                case il::MathUnaryFn::Cos:
                    valMap[inst.result] = b.build_call("ptx_cos", brass::Type::f32(), {x});
                    break;
                }
                break;
            }

            case il::Op::PropGet: {
                std::string key = inst.keyIndex < ilMod->keyConstants.size()
                                      ? ilMod->keyConstants[inst.keyIndex]
                                      : "";
                float cval = 0.0f;
                if (isAllowedMathConst(key, &cval)) valMap[inst.result] = kb.const_f32(cval);
                else valMap[inst.result] = kb.const_i32(static_cast<int32_t>(opts.localSizeX));
                break;
            }

            case il::Op::MethodCall: {
                std::string key = inst.keyIndex < ilMod->keyConstants.size()
                                      ? ilMod->keyConstants[inst.keyIndex]
                                      : "";
                brass::Value* a0 = inst.operands.size() >= 2
                                       ? asF32(getVal(inst.operands[1], brass::Type::f32()))
                                       : kb.const_f32(0.0f);
                brass::Value* a1 = inst.operands.size() >= 3
                                       ? asF32(getVal(inst.operands[2], brass::Type::f32()))
                                       : kb.const_f32(0.0f);
                if (key == "sin") valMap[inst.result] = b.build_call("ptx_sin", brass::Type::f32(), {a0});
                else if (key == "cos") valMap[inst.result] = b.build_call("ptx_cos", brass::Type::f32(), {a0});
                else if (key == "sqrt") valMap[inst.result] = b.build_sqrt(a0);
                else if (key == "abs") valMap[inst.result] = kb.fabs(a0);
                else if (key == "floor") valMap[inst.result] = b.build_floor(a0);
                else if (key == "ceil") valMap[inst.result] = b.build_ceil(a0);
                else if (key == "round") valMap[inst.result] = b.build_round(a0);
                else if (key == "trunc") valMap[inst.result] = b.build_sitofp_f32_i32(b.build_fptosi_i32_f32(a0));
                else if (key == "exp") valMap[inst.result] = kb.exp_fast(a0);
                else if (key == "log") valMap[inst.result] = kb.log_fast(a0);
                else if (key == "min") valMap[inst.result] = kb.fmin(a0, a1);
                else if (key == "max") valMap[inst.result] = kb.fmax(a0, a1);
                else if (key == "pow") valMap[inst.result] = kb.exp_fast(kb.mul(a1, kb.log_fast(a0)));
                else if (key == "atan2") {
                    brass::Value* ay = kb.fabs(a0);
                    brass::Value* ax = kb.fabs(a1);
                    brass::Value* mn = kb.fmin(ax, ay);
                    brass::Value* mx = kb.fmax(kb.fmax(ax, ay), kb.const_f32(1e-20f));
                    brass::Value* r = kb.div(mn, mx);
                    brass::Value* s = kb.mul(r, r);
                    brass::Value* p = kb.fma(s, kb.const_f32(-0.0464964749f), kb.const_f32(0.15931422f));
                    p = kb.fma(p, s, kb.const_f32(-0.327622764f));
                    p = kb.fma(kb.mul(p, s), r, r);
                    p = b.build_select(b.build_sgt(ay, ax), kb.sub(kb.const_f32(1.57079637f), p), p);
                    p = b.build_select(b.build_slt(a1, kb.const_f32(0.0f)), kb.sub(kb.const_f32(3.14159265f), p), p);
                    valMap[inst.result] = b.build_select(b.build_slt(a0, kb.const_f32(0.0f)), b.build_neg(p), p);
                } else if (key == "imul") {
                    valMap[inst.result] = kb.mul(asI32(getVal(inst.operands[1], brass::Type::i32())),
                                                 asI32(getVal(inst.operands[2], brass::Type::i32())));
                } else {
                    valMap[inst.result] = a0;
                }
                break;
            }

            case il::Op::GlobalGet:
            case il::Op::ResolveName: {
                std::string key = inst.keyIndex < ilMod->keyConstants.size()
                                      ? ilMod->keyConstants[inst.keyIndex]
                                      : "";
                if (key == "NaN") valMap[inst.result] = kb.const_f32(std::nanf(""));
                else if (key == "Infinity") valMap[inst.result] = kb.const_f32(INFINITY);
                break;
            }

            case il::Op::DynamicCall: {
                il::ValueId calleeId = !inst.operands.empty() ? originOf(inst.operands[0]) : il::kNoValue;
                std::string name = globalNames.count(calleeId) ? globalNames[calleeId] : "";
                if (name == "tid_x") valMap[inst.result] = kb.tid_x();
                else if (name == "tid_y") valMap[inst.result] = kb.tid_y();
                else if (name == "tid_z") valMap[inst.result] = kb.tid_z();
                else if (name == "ctaid_x") valMap[inst.result] = kb.ctaid_x();
                else if (name == "ctaid_y") valMap[inst.result] = kb.ctaid_y();
                else if (name == "ctaid_z") valMap[inst.result] = kb.ctaid_z();
                else if (name == "ntid_x") valMap[inst.result] = kb.ntid_x();
                else if (name == "ntid_y") valMap[inst.result] = kb.ntid_y();
                else if (name == "ntid_z") valMap[inst.result] = kb.ntid_z();
                else if (name == "nctaid_x") valMap[inst.result] = kb.nctaid_x();
                else if (name == "nctaid_y") valMap[inst.result] = kb.nctaid_y();
                else if (name == "nctaid_z") valMap[inst.result] = kb.nctaid_z();
                else if (name == "global_tid_x") valMap[inst.result] = kb.global_tid_x();
                else if (name == "lane_id") valMap[inst.result] = kb.lane_id();
                else if (name == "warp_id") valMap[inst.result] = kb.warp_id();
                else if (name == "sync" || name == "bar_sync") {
                    kb.sync();
                    valMap[inst.result] = kb.const_i32(0);
                }
                break;
            }

            case il::Op::Jump: {
                std::vector<brass::Value*> args;
                const auto& dstParams = fn.blocks[inst.target.block].params;
                for (size_t k = 0; k < dstParams.size() && k < inst.target.args.size(); ++k) {
                    bool wantFl = mustBeFloat.count(dstParams[k].id);
                    brass::Value* v = getVal(inst.target.args[k], wantFl ? brass::Type::f32() : brass::Type::i32());
                    args.push_back(wantFl ? asF32(v) : asI32(v));
                }
                b.build_br(mirBlocks[inst.target.block], std::move(args));
                break;
            }

            case il::Op::Branch: {
                brass::Value* cond = asI32(getVal(inst.operands[0], brass::Type::i32()));
                auto buildArgs = [&](const il::BlockTarget& t) {
                    std::vector<brass::Value*> args;
                    const auto& dstParams = fn.blocks[t.block].params;
                    for (size_t k = 0; k < dstParams.size() && k < t.args.size(); ++k) {
                        bool wantFl = mustBeFloat.count(dstParams[k].id);
                        brass::Value* v = getVal(t.args[k], wantFl ? brass::Type::f32() : brass::Type::i32());
                        args.push_back(wantFl ? asF32(v) : asI32(v));
                    }
                    return args;
                };
                b.build_br_if(cond,
                              mirBlocks[inst.target.block], buildArgs(inst.target),
                              mirBlocks[inst.elseTarget.block], buildArgs(inst.elseTarget));
                break;
            }

            case il::Op::Ret:
                b.build_ret_void();
                break;

            default:
                break;
            }
        }
    }

    // 5. Run Brass MIR Optimizations (#79 SMT Guard Elimination, #78/#80 Superopt, Scalar Cleanup)
    if (opts.runOptimizations) {
        brass::scalar_cleanup(*mirFn);
        brass::run_smt_guard_elimination(*mirFn, mirMod);
        brass::run_gpu_mir_superopt(*mirFn, mirMod);
        brass::eliminate_dead_code(*mirFn);
    }

    // 6. Emit and verify Vulkan SPIR-V 1.5 and CUDA PTX
    try {
        brass::target::SpirvOptions spvOpts;
        spvOpts.spirv_version = opts.spirvVersion;
        spvOpts.local_size_x = opts.localSizeX;
        spvOpts.local_size_y = opts.localSizeY;
        spvOpts.local_size_z = opts.localSizeZ;

        brass::spirv::Module spvMod(spvOpts.spirv_version);
        brass::spirv::SpirvISel spvIsel(spvMod, spvOpts);
        spvIsel.lower(*mirFn);
        res.spirvDiagnostics = brass::spirv::verify(spvMod);
        if (!res.spirvDiagnostics.empty()) {
            res.errorMessage = "SPIR-V verification failed:\n" +
                               brass::spirv::format_diagnostics(res.spirvDiagnostics);
            res.diagnostics.push_back(res.errorMessage);
            return res;
        }
        res.spirvWords = brass::spirv::write(spvMod);
        res.spirvDisassembly = brass::spirv::dump(spvMod);

        brass::target::PtxOptions ptxOpts;
        ptxOpts.sm_arch = "sm_" + std::to_string(opts.ptxSmVersion);
        brass::ptx::PtxISel ptxIsel;
        brass::ptx::Function ptxFn = ptxIsel.lower(*mirFn);
        if (ptxOpts.cleanup) brass::ptx::cleanup(ptxFn);
        res.ptxDiagnostics = brass::ptx::verify(ptxFn);
        if (!res.ptxDiagnostics.empty()) {
            res.errorMessage = "PTX verification failed:\n" +
                               brass::ptx::format_diagnostics(res.ptxDiagnostics);
            res.diagnostics.push_back(res.errorMessage);
            return res;
        }
        res.ptxAssembly = brass::ptx::print(ptxFn, ptxOpts);

        // 7. Cross-backend SMT equivalence & GPU memory/barrier safety verification (#95, #96)
        if (opts.verifyEquivalence) {
            std::string equivErr;
            res.equivalence = brass::verify::verify_spirv_ptx_equivalence(
                *mirFn, spvMod, ptxFn, &equivErr);
            if (!res.equivalence.is_equivalent()) {
                res.errorMessage = "Cross-backend SPIR-V/PTX equivalence check failed: " + equivErr;
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
            if (!res.memorySafety.is_safe() || !res.barrierUniformity.is_uniform()) {
                res.errorMessage = "GPU kernel safety verification failed: " +
                                   res.memorySafety.diagnostic + " " +
                                   res.barrierUniformity.diagnostic;
                res.diagnostics.push_back(res.errorMessage);
                return res;
            }
        }
    } catch (const std::exception& ex) {
        res.errorMessage = std::string("error: GPU codegen failed: ") + ex.what() + "\n";
        res.diagnostics.push_back(res.errorMessage);
        return res;
    }

    res.ok = true;
    return res;
}

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
        else if (sourcePath.empty()) sourcePath = arg;
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
        std::fputs(res.ptxAssembly.c_str(), stdout);
    }
    return 0;
}

}  // namespace bronze
